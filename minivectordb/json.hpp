// Minimal JSON reader/writer for the MiniVectorDB stdin/stdout protocol.
// Integers are kept as int64 so BIGINT values never lose precision; a number is
// only treated as a double when it has a fraction or an exponent.
#pragma once

#include <cstdint>
#include <cstdio>
#include <map>
#include <stdexcept>
#include <string>
#include <vector>

namespace mvd {

class JsonError : public std::runtime_error {
 public:
  explicit JsonError(const std::string& what) : std::runtime_error(what) {}
};

struct JValue {
  enum Kind { NUL, BOOL, INT, DOUBLE, STR, ARR, OBJ } kind = NUL;
  bool b = false;
  int64_t i = 0;
  double d = 0;
  std::string s;
  std::vector<JValue> arr;
  std::vector<std::pair<std::string, JValue>> obj;  // preserves field order

  JValue() = default;
  static JValue makeInt(int64_t v) {
    JValue j;
    j.kind = INT;
    j.i = v;
    return j;
  }
  static JValue makeBool(bool v) {
    JValue j;
    j.kind = BOOL;
    j.b = v;
    return j;
  }
  static JValue makeStr(const std::string& v) {
    JValue j;
    j.kind = STR;
    j.s = v;
    return j;
  }
  static JValue makeArr() {
    JValue j;
    j.kind = ARR;
    return j;
  }
  static JValue makeObj() {
    JValue j;
    j.kind = OBJ;
    return j;
  }
  static JValue makeDouble(double v) {
    JValue j;
    j.kind = DOUBLE;
    j.d = v;
    return j;
  }

  void set(const std::string& key, JValue v) { obj.emplace_back(key, std::move(v)); }
  const JValue* find(const std::string& key) const {
    for (const auto& kv : obj) {
      if (kv.first == key) return &kv.second;
    }
    return nullptr;
  }

  bool isObj() const { return kind == OBJ; }
  bool isArr() const { return kind == ARR; }
  bool isStr() const { return kind == STR; }
  bool isInt() const { return kind == INT; }
  bool isNum() const { return kind == INT || kind == DOUBLE; }
  double numAsDouble() const { return kind == INT ? static_cast<double>(i) : d; }
};

// ---------------------------------------------------------------- serialising

inline void escapeInto(const std::string& in, std::string& out) {
  out.push_back('"');
  for (unsigned char c : in) {
    switch (c) {
      case '"': out += "\\\""; break;
      case '\\': out += "\\\\"; break;
      case '\n': out += "\\n"; break;
      case '\r': out += "\\r"; break;
      case '\t': out += "\\t"; break;
      case '\b': out += "\\b"; break;
      case '\f': out += "\\f"; break;
      default:
        if (c < 0x20) {
          char buf[8];
          std::snprintf(buf, sizeof(buf), "\\u%04x", c);
          out += buf;
        } else {
          out.push_back(static_cast<char>(c));
        }
    }
  }
  out.push_back('"');
}

// Doubles are printed with enough digits to round-trip a float, but never as a
// non-finite value (the protocol forbids NaN/Infinity).
inline void numberInto(double v, std::string& out) {
  if (!(v == v) || v == static_cast<double>(1.0 / 0.0) || v == static_cast<double>(-1.0 / 0.0)) {
    out += "0";
    return;
  }
  char buf[64];
  std::snprintf(buf, sizeof(buf), "%.10g", v);
  out += buf;
}

inline void dumpInto(const JValue& v, std::string& out) {
  switch (v.kind) {
    case JValue::NUL: out += "null"; break;
    case JValue::BOOL: out += v.b ? "true" : "false"; break;
    case JValue::INT: out += std::to_string(v.i); break;
    case JValue::DOUBLE: numberInto(v.d, out); break;
    case JValue::STR: escapeInto(v.s, out); break;
    case JValue::ARR: {
      out.push_back('[');
      for (size_t k = 0; k < v.arr.size(); ++k) {
        if (k) out.push_back(',');
        dumpInto(v.arr[k], out);
      }
      out.push_back(']');
      break;
    }
    case JValue::OBJ: {
      out.push_back('{');
      for (size_t k = 0; k < v.obj.size(); ++k) {
        if (k) out.push_back(',');
        escapeInto(v.obj[k].first, out);
        out.push_back(':');
        dumpInto(v.obj[k].second, out);
      }
      out.push_back('}');
      break;
    }
  }
}

inline std::string dump(const JValue& v) {
  std::string out;
  dumpInto(v, out);
  return out;
}

// ------------------------------------------------------------------- parsing

class JsonParser {
 public:
  explicit JsonParser(const std::string& text) : s_(text) {}

  JValue parse() {
    skipWs();
    JValue v = parseValue();
    skipWs();
    if (pos_ != s_.size()) throw JsonError("trailing data after JSON value");
    return v;
  }

 private:
  const std::string& s_;
  size_t pos_ = 0;

  void skipWs() {
    while (pos_ < s_.size()) {
      char c = s_[pos_];
      if (c == ' ' || c == '\t' || c == '\n' || c == '\r') ++pos_;
      else break;
    }
  }
  char peek() {
    if (pos_ >= s_.size()) throw JsonError("unexpected end of JSON input");
    return s_[pos_];
  }
  void expect(char c) {
    if (pos_ >= s_.size() || s_[pos_] != c) throw JsonError(std::string("expected '") + c + "'");
    ++pos_;
  }
  void literal(const char* word) {
    size_t n = std::char_traits<char>::length(word);
    if (s_.compare(pos_, n, word) != 0) throw JsonError("invalid literal");
    pos_ += n;
  }

  JValue parseValue() {
    char c = peek();
    switch (c) {
      case '{': return parseObject();
      case '[': return parseArray();
      case '"': return JValue::makeStr(parseString());
      case 't': literal("true"); return JValue::makeBool(true);
      case 'f': literal("false"); return JValue::makeBool(false);
      case 'n': literal("null"); return JValue();
      default: return parseNumber();
    }
  }

  JValue parseObject() {
    JValue o = JValue::makeObj();
    expect('{');
    skipWs();
    if (peek() == '}') {
      ++pos_;
      return o;
    }
    for (;;) {
      skipWs();
      std::string key = parseString();
      if (o.find(key) != nullptr) throw JsonError("duplicate JSON key: " + key);
      skipWs();
      expect(':');
      skipWs();
      o.obj.emplace_back(key, parseValue());
      skipWs();
      char c = peek();
      if (c == ',') {
        ++pos_;
        continue;
      }
      if (c == '}') {
        ++pos_;
        return o;
      }
      throw JsonError("expected ',' or '}' in object");
    }
  }

  JValue parseArray() {
    JValue a = JValue::makeArr();
    expect('[');
    skipWs();
    if (peek() == ']') {
      ++pos_;
      return a;
    }
    for (;;) {
      skipWs();
      a.arr.push_back(parseValue());
      skipWs();
      char c = peek();
      if (c == ',') {
        ++pos_;
        continue;
      }
      if (c == ']') {
        ++pos_;
        return a;
      }
      throw JsonError("expected ',' or ']' in array");
    }
  }

  std::string parseString() {
    expect('"');
    std::string out;
    for (;;) {
      if (pos_ >= s_.size()) throw JsonError("unterminated string");
      unsigned char c = static_cast<unsigned char>(s_[pos_++]);
      if (c == '"') return out;
      if (c != '\\') {
        out.push_back(static_cast<char>(c));
        continue;
      }
      if (pos_ >= s_.size()) throw JsonError("unterminated escape");
      char e = s_[pos_++];
      switch (e) {
        case '"': out.push_back('"'); break;
        case '\\': out.push_back('\\'); break;
        case '/': out.push_back('/'); break;
        case 'b': out.push_back('\b'); break;
        case 'f': out.push_back('\f'); break;
        case 'n': out.push_back('\n'); break;
        case 'r': out.push_back('\r'); break;
        case 't': out.push_back('\t'); break;
        case 'u': {
          uint32_t cp = parseHex4();
          if (cp >= 0xD800 && cp <= 0xDBFF && pos_ + 1 < s_.size() && s_[pos_] == '\\'
              && s_[pos_ + 1] == 'u') {
            pos_ += 2;
            uint32_t lo = parseHex4();
            if (lo >= 0xDC00 && lo <= 0xDFFF) {
              cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
            }
          }
          appendUtf8(cp, out);
          break;
        }
        default: throw JsonError("invalid escape in string");
      }
    }
  }

  uint32_t parseHex4() {
    if (pos_ + 4 > s_.size()) throw JsonError("bad \\u escape");
    uint32_t v = 0;
    for (int k = 0; k < 4; ++k) {
      char c = s_[pos_++];
      v <<= 4;
      if (c >= '0' && c <= '9') v |= static_cast<uint32_t>(c - '0');
      else if (c >= 'a' && c <= 'f') v |= static_cast<uint32_t>(c - 'a' + 10);
      else if (c >= 'A' && c <= 'F') v |= static_cast<uint32_t>(c - 'A' + 10);
      else throw JsonError("bad hex digit in \\u escape");
    }
    return v;
  }

  static void appendUtf8(uint32_t cp, std::string& out) {
    if (cp < 0x80) {
      out.push_back(static_cast<char>(cp));
    } else if (cp < 0x800) {
      out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
      out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else if (cp < 0x10000) {
      out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
      out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
      out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else {
      out.push_back(static_cast<char>(0xF0 | (cp >> 18)));
      out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
      out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
      out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    }
  }

  JValue parseNumber() {
    size_t start = pos_;
    bool isDouble = false;
    if (pos_ < s_.size() && (s_[pos_] == '-' || s_[pos_] == '+')) ++pos_;
    while (pos_ < s_.size()) {
      char c = s_[pos_];
      if (c >= '0' && c <= '9') {
        ++pos_;
      } else if (c == '.' || c == 'e' || c == 'E' || c == '+' || c == '-') {
        isDouble = true;
        ++pos_;
      } else {
        break;
      }
    }
    if (pos_ == start) throw JsonError("invalid number");
    std::string text = s_.substr(start, pos_ - start);
    if (!isDouble) {
      try {
        size_t used = 0;
        int64_t v = std::stoll(text, &used);
        if (used == text.size()) return JValue::makeInt(v);
      } catch (...) {
      }
      isDouble = true;
    }
    try {
      return JValue::makeDouble(std::stod(text));
    } catch (...) {
      throw JsonError("invalid number: " + text);
    }
  }
};

inline JValue parseJson(const std::string& text) { return JsonParser(text).parse(); }

}  // namespace mvd
