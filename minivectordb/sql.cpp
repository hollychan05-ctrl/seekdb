// Hand-written lexer + recursive-descent parser for the MiniVectorDB SQL
// subset.  Unknown leading keywords become UNSUPPORTED (reported as
// unsupported_sql) instead of a syntax error, which is what the task asks for.
#include "mvd.hpp"

#include <cctype>
#include <cstdlib>
#include <cstring>

namespace mvd {
namespace {

enum class Tok {
  END,
  IDENT,
  NUMBER,
  STRING,
  PUNCT,      // ( ) , . * [ ] ;
  EQ, LT, LE, GT, GE,
  STAR
};

struct Token {
  Tok kind = Tok::END;
  std::string text;  // ident (lower-cased) or punctuation
  std::string raw;   // original text for identifiers/strings
  int64_t num = 0;
  bool num_is_double = false;
  double dnum = 0;
};

bool identStart(unsigned char c) {
  return std::isalpha(c) || c == '_' || c == '$' || c >= 0x80;
}
bool identPart(unsigned char c) { return std::isalnum(c) || c == '_' || c == '$' || c >= 0x80; }

class Lexer {
 public:
  explicit Lexer(const std::string& s) : s_(s) {}

  std::vector<Token> run(std::string& err) {
    std::vector<Token> out;
    while (true) {
      Token t = next(err);
      if (!err.empty()) return out;
      out.push_back(t);
      if (t.kind == Tok::END) return out;
    }
  }

 private:
  const std::string& s_;
  size_t p_ = 0;

  Token next(std::string& err) {
    Token t;
    while (p_ < s_.size()) {
      char c = s_[p_];
      if (c == ' ' || c == '\t' || c == '\r' || c == '\n') {
        ++p_;
        continue;
      }
      // -- line comment or # comment
      if (c == '#' || (c == '-' && p_ + 1 < s_.size() && s_[p_ + 1] == '-')) {
        while (p_ < s_.size() && s_[p_] != '\n') ++p_;
        continue;
      }
      if (c == '/' && p_ + 1 < s_.size() && s_[p_ + 1] == '*') {
        p_ += 2;
        while (p_ + 1 < s_.size() && !(s_[p_] == '*' && s_[p_ + 1] == '/')) ++p_;
        p_ = (p_ + 1 < s_.size()) ? p_ + 2 : s_.size();
        continue;
      }
      break;
    }
    if (p_ >= s_.size()) {
      t.kind = Tok::END;
      return t;
    }
    unsigned char c = static_cast<unsigned char>(s_[p_]);
    if (identStart(c)) {
      size_t start = p_;
      while (p_ < s_.size() && identPart(static_cast<unsigned char>(s_[p_]))) ++p_;
      t.kind = Tok::IDENT;
      t.raw = s_.substr(start, p_ - start);
      t.text = t.raw;
      for (char& ch : t.text) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
      return t;
    }
    if (c == '`') {
      ++p_;
      std::string name;
      while (p_ < s_.size() && s_[p_] != '`') name.push_back(s_[p_++]);
      if (p_ < s_.size()) ++p_;
      t.kind = Tok::IDENT;
      t.raw = name;
      t.text = name;
      for (char& ch : t.text) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
      return t;
    }
    if (std::isdigit(c) || (c == '.' && p_ + 1 < s_.size() && std::isdigit(static_cast<unsigned char>(s_[p_ + 1])))) {
      size_t start = p_;
      bool isDouble = false;
      while (p_ < s_.size()) {
        char d = s_[p_];
        if (std::isdigit(static_cast<unsigned char>(d))) {
          ++p_;
        } else if (d == '.' || d == 'e' || d == 'E') {
          isDouble = true;
          ++p_;
          if ((d == 'e' || d == 'E') && p_ < s_.size() && (s_[p_] == '+' || s_[p_] == '-')) ++p_;
        } else {
          break;
        }
      }
      t.kind = Tok::NUMBER;
      t.raw = s_.substr(start, p_ - start);
      if (isDouble) {
        t.num_is_double = true;
        t.dnum = std::strtod(t.raw.c_str(), nullptr);
      } else {
        t.num = std::strtoll(t.raw.c_str(), nullptr, 10);
      }
      return t;
    }
    if (c == '\'' || c == '"') {
      char quote = static_cast<char>(c);
      ++p_;
      std::string value;
      bool closed = false;
      while (p_ < s_.size()) {
        char d = s_[p_++];
        if (d == quote) {
          if (p_ < s_.size() && s_[p_] == quote) {
            value.push_back(quote);
            ++p_;
            continue;
          }
          closed = true;
          break;
        }
        if (d == '\\' && p_ < s_.size()) {
          char e = s_[p_++];
          switch (e) {
            case 'n': value.push_back('\n'); break;
            case 't': value.push_back('\t'); break;
            case 'r': value.push_back('\r'); break;
            case '0': value.push_back('\0'); break;
            case '\\': value.push_back('\\'); break;
            case '\'': value.push_back('\''); break;
            case '"': value.push_back('"'); break;
            default: value.push_back(e); break;
          }
          continue;
        }
        value.push_back(d);
      }
      if (!closed) {
        err = "unterminated string literal";
        return t;
      }
      t.kind = Tok::STRING;
      t.raw = value;
      return t;
    }
    switch (c) {
      case '(': case ')': case ',': case '.': case ';': {
        t.kind = Tok::PUNCT;
        t.text.assign(1, static_cast<char>(c));
        ++p_;
        return t;
      }
      case '[': {
        t.kind = Tok::PUNCT;
        t.text = "[";
        ++p_;
        return t;
      }
      case ']': {
        t.kind = Tok::PUNCT;
        t.text = "]";
        ++p_;
        return t;
      }
      case '*': {
        t.kind = Tok::STAR;
        t.text = "*";
        ++p_;
        return t;
      }
      case '-': {
        t.kind = Tok::PUNCT;
        t.text = "-";
        ++p_;
        return t;
      }
      case '+': {
        t.kind = Tok::PUNCT;
        t.text = "+";
        ++p_;
        return t;
      }
      case '=': {
        t.kind = Tok::EQ;
        t.text = "=";
        ++p_;
        return t;
      }
      case '<': {
        ++p_;
        if (p_ < s_.size() && s_[p_] == '=') {
          ++p_;
          t.kind = Tok::LE;
          t.text = "<=";
        } else if (p_ < s_.size() && s_[p_] == '>') {
          ++p_;
          t.kind = Tok::PUNCT;
          t.text = "<>";
        } else {
          t.kind = Tok::LT;
          t.text = "<";
        }
        return t;
      }
      case '>': {
        ++p_;
        if (p_ < s_.size() && s_[p_] == '=') {
          ++p_;
          t.kind = Tok::GE;
          t.text = ">=";
        } else {
          t.kind = Tok::GT;
          t.text = ">";
        }
        return t;
      }
      case '!': {
        ++p_;
        if (p_ < s_.size() && s_[p_] == '=') {
          ++p_;
          t.kind = Tok::PUNCT;
          t.text = "!=";
          return t;
        }
        err = "unexpected '!'";
        return t;
      }
      default: {
        err = std::string("unexpected character '") + static_cast<char>(c) + "'";
        return t;
      }
    }
  }
};

bool isKeyword(const Token& t, const char* kw) { return t.kind == Tok::IDENT && t.text == kw; }

// Words that start the next clause; they must never be taken as a bare
// (AS-less) output alias.
bool isClauseKeyword(const std::string& t) {
  static const char* kws[] = {"from", "where", "order", "group",  "having", "limit",
                              "offset", "and",   "or",    "asc",    "desc",   "as",
                              "union",  "join",  "on",    "using",  "into"};
  for (const char* k : kws) {
    if (t == k) return true;
  }
  return false;
}

class Parser {
 public:
  Parser(std::vector<Token> toks, std::string& err) : toks_(std::move(toks)), err_(err) {}

  bool parse(Stmt& out) {
    skipSemicolons();
    if (atEnd()) {
      out.kind = Stmt::NOOP;
      return true;
    }
    const Token& t = peek();
    if (t.kind == Tok::IDENT) {
      if (t.text == "create") return parseCreate(out);
      if (t.text == "insert") return parseInsert(out);
      if (t.text == "select") return parseSelect(out);
      if (t.text == "update") return parseUpdate(out);
      if (t.text == "delete") return parseDelete(out);
      if (t.text == "drop") return parseDrop(out);
      if (t.text == "begin" || t.text == "start") { advance(); out.kind = Stmt::BEGIN; return true; }
      if (t.text == "commit" || t.text == "end") { advance(); out.kind = Stmt::COMMIT; return true; }
      if (t.text == "rollback") { advance(); out.kind = Stmt::ROLLBACK; return true; }
      if (t.text == "set") { out.kind = Stmt::NOOP; return true; }
      if (t.text == "explain" || t.text == "desc" || t.text == "describe" || t.text == "show" ||
          t.text == "use") {
        out.kind = Stmt::NOOP;
        return true;
      }
      out.kind = Stmt::UNSUPPORTED;
      return true;
    }
    out.kind = Stmt::UNSUPPORTED;
    return true;
  }

 private:
  std::vector<Token> toks_;
  size_t p_ = 0;
  std::string& err_;

  const Token& peek(size_t k = 0) const {
    static Token endTok;
    size_t idx = p_ + k;
    return idx < toks_.size() ? toks_[idx] : endTok;
  }
  const Token& advance() {
    const Token& t = peek();
    if (p_ < toks_.size()) ++p_;
    return t;
  }
  bool atEnd() const { return peek().kind == Tok::END; }
  void skipSemicolons() {
    while (peek().kind == Tok::PUNCT && peek().text == ";") advance();
  }
  bool fail(const std::string& m) {
    if (err_.empty()) err_ = m;
    return false;
  }
  bool expectPunct(const char* what) {
    if (peek().kind == Tok::PUNCT && peek().text == what) {
      advance();
      return true;
    }
    return fail(std::string("expected '") + what + "'");
  }
  bool ident(std::string& out) {
    if (peek().kind != Tok::IDENT) return fail("expected identifier");
    out = peek().text;
    advance();
    return true;
  }

  bool parseType(ColumnDef& col) {
    std::string name;
    if (!ident(name)) return false;
    if (name == "bigint" || name == "int" || name == "integer" || name == "smallint" ||
        name == "tinyint" || name == "mediumint") {
      col.type = VType::BIGINT;
      return true;
    }
    if (name == "varchar" || name == "char" || name == "text" || name == "longtext" ||
        name == "string") {
      col.type = VType::VARCHAR;
      col.len = 1024;
      if (peek().kind == Tok::PUNCT && peek().text == "(") {
        advance();
        if (peek().kind != Tok::NUMBER) return fail("expected VARCHAR length");
        col.len = peek().num;
        advance();
        if (!expectPunct(")")) return false;
      }
      if (col.len <= 0) return fail("VARCHAR length must be positive");
      return true;
    }
    if (name == "vector") {
      col.type = VType::VECTOR;
      if (peek().kind == Tok::PUNCT && peek().text == "(") {
        advance();
        if (peek().kind != Tok::NUMBER) return fail("expected VECTOR dimension");
        col.len = peek().num;
        advance();
        if (!expectPunct(")")) return false;
      } else {
        return fail("VECTOR requires an explicit dimension");
      }
      if (col.len <= 0 || col.len > 16384) return fail("unsupported VECTOR dimension");
      return true;
    }
    return fail("unsupported column type: " + name);
  }

  bool parseCreate(Stmt& out) {
    advance();  // create
    if (isKeyword(peek(), "vector")) {
      // CREATE VECTOR INDEX name ON table(col)
      advance();
      if (!(isKeyword(peek(), "index"))) { out.kind = Stmt::UNSUPPORTED; return true; }
      advance();
      out.kind = Stmt::CREATE_VECTOR_INDEX;
      if (!ident(out.name)) return false;
      if (!isKeyword(peek(), "on")) { out.kind = Stmt::UNSUPPORTED; return true; }
      advance();
      if (!ident(out.table)) return false;
      if (!expectPunct("(")) return false;
      if (!ident(out.column)) return false;
      if (!expectPunct(")")) return false;
      return true;
    }
    if (isKeyword(peek(), "table")) {
      advance();
      out.kind = Stmt::CREATE_TABLE;
      if (!ident(out.name)) return false;
      if (!expectPunct("(")) return false;
      for (;;) {
        ColumnDef col;
        if (!ident(col.name)) return false;
        if (!parseType(col)) return false;
        while (!(peek().kind == Tok::PUNCT && (peek().text == "," || peek().text == ")"))) {
          // Column options we accept and ignore, plus PRIMARY KEY.
          if (isKeyword(peek(), "primary")) {
            advance();
            if (isKeyword(peek(), "key")) advance();
            col.primary = true;
          } else if (isKeyword(peek(), "not") || isKeyword(peek(), "null") ||
                     isKeyword(peek(), "unique") || isKeyword(peek(), "auto_increment") ||
                     isKeyword(peek(), "default") || isKeyword(peek(), "comment")) {
            advance();
            if (peek().kind == Tok::STRING || peek().kind == Tok::NUMBER) advance();
          } else {
            break;
          }
        }
        out.columns.push_back(col);
        if (peek().kind == Tok::PUNCT && peek().text == ",") {
          advance();
          continue;
        }
        break;
      }
      // Table-level PRIMARY KEY (col)
      if (peek().kind == Tok::PUNCT && peek().text == ",") {
        advance();
        if (isKeyword(peek(), "primary")) {
          advance();
          if (isKeyword(peek(), "key")) advance();
          if (!expectPunct("(")) return false;
          std::string pkName;
          if (!ident(pkName)) return false;
          if (!expectPunct(")")) return false;
          for (auto& c : out.columns) {
            if (c.name == pkName) c.primary = true;
          }
        }
      }
      if (!expectPunct(")")) return false;
      return true;
    }
    out.kind = Stmt::UNSUPPORTED;
    return true;
  }

  // Reads an optionally signed numeric literal.  The magnitude is re-parsed
  // from the token text with the sign attached, so INT64_MIN
  // (-9223372036854775808) is not clamped by an intermediate unsigned parse.
  bool parseSignedLiteral(Literal& lit) {
    if (peek().kind == Tok::PUNCT && (peek().text == "-" || peek().text == "+")) {
      bool neg = peek().text == "-";
      advance();
      if (peek().kind != Tok::NUMBER) return fail("expected number after sign");
      const Token& t = peek();
      if (t.num_is_double) {
        lit.kind = Literal::VEC;
        lit.v = {static_cast<float>(neg ? -t.dnum : t.dnum)};
      } else {
        std::string text = (neg ? "-" : "") + t.raw;
        lit.kind = Literal::INT;
        lit.i = static_cast<int64_t>(std::strtoll(text.c_str(), nullptr, 10));
      }
      advance();
      return true;
    }
    return parseLiteral(lit);
  }

  bool parseLiteral(Literal& lit) {
    const Token& t = peek();
    if (t.kind == Tok::NUMBER) {
      if (t.num_is_double) {
        lit.kind = Literal::VEC;  // placeholder; not valid as a scalar
        lit.v = {static_cast<float>(t.dnum)};
      } else {
        lit.kind = Literal::INT;
        lit.i = t.num;
      }
      advance();
      return true;
    }
    if (t.kind == Tok::STRING) {
      lit.kind = Literal::STR;
      lit.s = t.raw;
      advance();
      return true;
    }
    if (t.kind == Tok::PUNCT && t.text == "[") {
      return parseVectorLiteral(lit);
    }
    if (isKeyword(t, "null")) {
      // NULL is not required by the task; treat it as an empty string marker.
      lit.kind = Literal::STR;
      lit.s.clear();
      advance();
      return true;
    }
    return fail("expected literal value");
  }

  bool parseVectorLiteral(Literal& lit) {
    if (!expectPunct("[")) return false;
    lit.kind = Literal::VEC;
    lit.v.clear();
    if (peek().kind == Tok::PUNCT && peek().text == "]") {
      advance();
      return true;
    }
    for (;;) {
      const Token& t = peek();
      if (t.kind == Tok::NUMBER || (t.kind == Tok::PUNCT && (t.text == "-" || t.text == "+"))) {
        double v = 0;
        if (t.kind == Tok::PUNCT) {
          bool neg = t.text == "-";
          advance();
          if (peek().kind != Tok::NUMBER) return fail("expected number in vector literal");
          v = peek().num_is_double ? peek().dnum : static_cast<double>(peek().num);
          if (neg) v = -v;
          advance();
        } else {
          v = t.num_is_double ? t.dnum : static_cast<double>(t.num);
          advance();
        }
        lit.v.push_back(static_cast<float>(v));
      } else {
        return fail("expected number in vector literal");
      }
      if (peek().kind == Tok::PUNCT && peek().text == ",") {
        advance();
        continue;
      }
      break;
    }
    if (!expectPunct("]")) return false;
    return true;
  }

  bool parseInsert(Stmt& out) {
    advance();  // insert
    if (!isKeyword(peek(), "into")) return fail("expected INTO");
    advance();
    out.kind = Stmt::INSERT;
    if (!ident(out.table)) return false;
    if (peek().kind == Tok::PUNCT && peek().text == "(") {
      advance();
      for (;;) {
        std::string c;
        if (!ident(c)) return false;
        out.insert_columns.push_back(c);
        if (peek().kind == Tok::PUNCT && peek().text == ",") {
          advance();
          continue;
        }
        break;
      }
      if (!expectPunct(")")) return false;
    }
    if (!isKeyword(peek(), "values")) return fail("expected VALUES");
    advance();
    for (;;) {
      if (!expectPunct("(")) return false;
      std::vector<Literal> row;
      for (;;) {
        Literal lit;
        if (!parseSignedLiteral(lit)) return false;
        row.push_back(lit);
        if (peek().kind == Tok::PUNCT && peek().text == ",") {
          advance();
          continue;
        }
        break;
      }
      if (!expectPunct(")")) return false;
      out.insert_rows.push_back(std::move(row));
      if (peek().kind == Tok::PUNCT && peek().text == ",") {
        advance();
        continue;
      }
      break;
    }
    return true;
  }

  bool parseWhere(Stmt& out) {
    for (;;) {
      WhereCond cond;
      if (!ident(cond.column)) return false;
      const Token& t = peek();
      if (t.kind == Tok::EQ) cond.op = "=";
      else if (t.kind == Tok::LT) cond.op = "<";
      else if (t.kind == Tok::LE) cond.op = "<=";
      else if (t.kind == Tok::GT) cond.op = ">";
      else if (t.kind == Tok::GE) cond.op = ">=";
      else if (t.kind == Tok::PUNCT && (t.text == "<>" || t.text == "!=")) cond.op = "!=";
      else return fail("expected comparison operator");
      advance();
      Literal lit;
      if (!parseSignedLiteral(lit)) return false;
      cond.value = lit;
      out.where.push_back(cond);
      if (isKeyword(peek(), "and")) {
        advance();
        continue;
      }
      break;
    }
    return true;
  }

  bool parseSelectItem(SelectItem& item) {
    if (peek().kind == Tok::STAR) {
      advance();
      item.kind = SelectItem::STAR;
      return true;
    }
    if (isKeyword(peek(), "l2_distance") || isKeyword(peek(), "euclidean_distance") ||
        isKeyword(peek(), "cosine_distance") || isKeyword(peek(), "l2_distance_squared")) {
      std::string fn = peek().text;
      advance();
      if (!expectPunct("(")) return false;
      item.kind = SelectItem::L2DIST;
      if (!ident(item.column)) return false;
      if (!expectPunct(",")) return false;
      if (!parseLiteral(item.vec)) return false;
      if (!expectPunct(")")) return false;
      item.alias = fn + "(" + item.column + ")";
      if (isKeyword(peek(), "as")) {
        advance();
        if (!ident(item.alias)) return false;
      } else if (peek().kind == Tok::IDENT && !isClauseKeyword(peek().text)) {
        if (!ident(item.alias)) return false;
      }
      return true;
    }
    item.kind = SelectItem::COLUMN;
    if (!ident(item.column)) return false;
    if (peek().kind == Tok::PUNCT && peek().text == ".") {
      advance();
      if (!ident(item.column)) return false;
    }
    item.alias = item.column;
    if (isKeyword(peek(), "as")) {
      advance();
      if (!ident(item.alias)) return false;
    } else if (peek().kind == Tok::IDENT && !isClauseKeyword(peek().text)) {
      if (!ident(item.alias)) return false;
    }
    return true;
  }

  bool parseSelect(Stmt& out) {
    advance();  // select
    out.kind = Stmt::SELECT;
    for (;;) {
      SelectItem item;
      if (!parseSelectItem(item)) return false;
      out.select_items.push_back(std::move(item));
      if (peek().kind == Tok::PUNCT && peek().text == ",") {
        advance();
        continue;
      }
      break;
    }
    if (!isKeyword(peek(), "from")) return fail("expected FROM");
    advance();
    if (!ident(out.table)) return false;
    if (peek().kind == Tok::PUNCT && peek().text == ".") {
      advance();
      if (!ident(out.table)) return false;
    }
    if (isKeyword(peek(), "where")) {
      advance();
      if (!parseWhere(out)) return false;
    }
    if (isKeyword(peek(), "order")) {
      advance();
      if (!isKeyword(peek(), "by")) return fail("expected BY after ORDER");
      advance();
      for (;;) {
        OrderItem oi;
        if (!ident(oi.column)) return false;
        if (peek().kind == Tok::PUNCT && peek().text == ".") {
          advance();
          if (!ident(oi.column)) return false;
        }
        if (isKeyword(peek(), "asc")) {
          advance();
        } else if (isKeyword(peek(), "desc")) {
          oi.desc = true;
          advance();
        }
        out.order_by.push_back(oi);
        if (peek().kind == Tok::PUNCT && peek().text == ",") {
          advance();
          continue;
        }
        break;
      }
    }
    if (isKeyword(peek(), "limit")) {
      advance();
      if (peek().kind != Tok::NUMBER) return fail("expected LIMIT value");
      int64_t first = peek().num;
      advance();
      out.has_limit = true;
      if (peek().kind == Tok::PUNCT && peek().text == ",") {
        advance();
        if (peek().kind != Tok::NUMBER) return fail("expected LIMIT count");
        out.offset = first;
        out.limit = peek().num;
        advance();
      } else {
        out.limit = first;
        if (isKeyword(peek(), "offset")) {
          advance();
          if (peek().kind != Tok::NUMBER) return fail("expected OFFSET value");
          out.offset = peek().num;
          advance();
        }
      }
    }
    return true;
  }

  bool parseUpdate(Stmt& out) {
    advance();  // update
    out.kind = Stmt::UPDATE;
    if (!ident(out.table)) return false;
    if (!isKeyword(peek(), "set")) return fail("expected SET");
    advance();
    for (;;) {
      Assign a;
      if (!ident(a.column)) return false;
      if (peek().kind != Tok::EQ) return fail("expected '=' in SET");
      advance();
      if (!parseSignedLiteral(a.value)) return false;
      out.assigns.push_back(std::move(a));
      if (peek().kind == Tok::PUNCT && peek().text == ",") {
        advance();
        continue;
      }
      break;
    }
    if (isKeyword(peek(), "where")) {
      advance();
      if (!parseWhere(out)) return false;
    }
    return true;
  }

  bool parseDelete(Stmt& out) {
    advance();  // delete
    if (!isKeyword(peek(), "from")) return fail("expected FROM");
    advance();
    out.kind = Stmt::DELETE_ROW;
    if (!ident(out.table)) return false;
    if (isKeyword(peek(), "where")) {
      advance();
      if (!parseWhere(out)) return false;
    }
    return true;
  }

  bool parseDrop(Stmt& out) {
    advance();  // drop
    if (!isKeyword(peek(), "table")) {
      out.kind = Stmt::UNSUPPORTED;
      return true;
    }
    advance();
    out.kind = Stmt::DROP_TABLE;
    if (isKeyword(peek(), "if")) {
      advance();
      if (isKeyword(peek(), "exists")) advance();
      out.if_exists = true;
    }
    if (!ident(out.name)) return false;
    return true;
  }
};

}  // namespace

bool parseSql(const std::string& sql, Stmt& out, std::string& err) {
  err.clear();
  Lexer lex(sql);
  std::vector<Token> toks = lex.run(err);
  if (!err.empty()) return false;
  Parser parser(std::move(toks), err);
  if (!parser.parse(out)) {
    if (err.empty()) err = "syntax error";
    return false;
  }
  return true;
}

}  // namespace mvd
