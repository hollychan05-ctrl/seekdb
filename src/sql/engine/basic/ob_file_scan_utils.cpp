/*
 * Copyright (c) 2025 OceanBase.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#define USING_LOG_PREFIX SQL_ENG
#include "ob_file_scan_utils.h"
#include "lib/ob_errno.h"
#include "lib/oblog/ob_log_module.h"
#include <glob.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <errno.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <limits.h>
#include <math.h>

namespace oceanbase
{
using namespace common;
namespace sql
{

namespace
{

const int64_t FILE_SQL_IO_BUF_SIZE = 1 << 16;
const int64_t FILE_SQL_MAX_FIELD = 1 << 20;

bool fs_is_digit(const char c) { return c >= '0' && c <= '9'; }

// text is not NUL terminated, so hand strtod/strtoll a terminated copy.
int fs_to_cstr(const ObString &text, char *buf, const int64_t cap)
{
  int ret = OB_SUCCESS;
  if (text.length() < 0 || text.length() + 1 > cap) {
    ret = OB_SIZE_OVERFLOW;
  } else {
    if (text.length() > 0) {
      MEMCPY(buf, text.ptr(), text.length());
    }
    buf[text.length()] = '\0';
  }
  return ret;
}

// Days from 1970-01-01 (Howard Hinnant's days_from_civil).
int64_t fs_days_from_civil(int64_t y, const int64_t m, const int64_t d)
{
  y -= m <= 2;
  const int64_t era = (y >= 0 ? y : y - 399) / 400;
  const int64_t yoe = y - era * 400;
  const int64_t doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
  const int64_t doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  return era * 146097 + doe - 719468;
}

int fs_append(ObIAllocator &alloc, char *&buf, int64_t &len, int64_t &cap, const char c)
{
  int ret = OB_SUCCESS;
  if (len + 1 > FILE_SQL_MAX_FIELD) {
    ret = OB_SIZE_OVERFLOW;
  } else if (len < cap) {
    buf[len++] = c;
  } else {
    int64_t new_cap = (cap > 0 ? cap * 2 : 64);
    char *new_buf = static_cast<char *>(alloc.alloc(new_cap));
    if (OB_ISNULL(new_buf)) {
      ret = OB_ALLOCATE_MEMORY_FAILED;
    } else {
      if (len > 0) {
        MEMCPY(new_buf, buf, len);
      }
      buf = new_buf;
      cap = new_cap;
      buf[len++] = c;
    }
  }
  return ret;
}

// A single "key": value pair of a flat json object.
struct FileSqlJsonPair
{
  ObString key_;
  ObString raw_;      // string contents are unescaped, numbers keep their text
  bool is_null_;
};

int fs_decode_json_string(ObIAllocator &alloc, const ObString &text, const int64_t begin,
                          int64_t &next, ObString &out)
{
  int ret = OB_SUCCESS;
  char *buf = NULL;
  int64_t len = 0;
  int64_t cap = 0;
  int64_t i = begin;
  bool closed = false;
  while (OB_SUCC(ret) && i < text.length() && !closed) {
    const char c = text[i++];
    if ('"' == c) {
      closed = true;
    } else if ('\\' == c && i < text.length()) {
      const char e = text[i++];
      char decoded = e;
      switch (e) {
        case 'n': decoded = '\n'; break;
        case 't': decoded = '\t'; break;
        case 'r': decoded = '\r'; break;
        case 'b': decoded = '\b'; break;
        case 'f': decoded = '\f'; break;
        default: decoded = e; break;  // \" \\ \/ and \uXXXX (kept literally)
      }
      if (OB_FAIL(fs_append(alloc, buf, len, cap, decoded))) {
      }
    } else if (OB_FAIL(fs_append(alloc, buf, len, cap, c))) {
    }
  }
  if (OB_SUCC(ret) && !closed) {
    ret = OB_INVALID_DATA;
  } else if (OB_SUCC(ret)) {
    out.assign_ptr(buf, static_cast<ObString::obstr_size_t>(len));
    next = i;
  }
  return ret;
}

// Parse one line as a flat json object. Nested objects/arrays are kept as raw text.
int fs_parse_json_line(ObIAllocator &alloc, const ObString &line,
                       ObIArray<FileSqlJsonPair> &pairs)
{
  int ret = OB_SUCCESS;
  int64_t i = 0;
  const int64_t n = line.length();
  const char *p = line.ptr();
  while (i < n && (p[i] == ' ' || p[i] == '\t' || p[i] == '\r')) { ++i; }
  if (i >= n || p[i] != '{') {
    ret = OB_INVALID_DATA;
  }
  ++i;
  bool done = false;
  while (OB_SUCC(ret) && !done) {
    while (i < n && (p[i] == ' ' || p[i] == '\t' || p[i] == ',')) { ++i; }
    if (i >= n) {
      done = true;
    } else if (p[i] == '}') {
      done = true;
      ++i;
    } else if (p[i] != '"') {
      ret = OB_INVALID_DATA;
    } else {
      FileSqlJsonPair pair;
      pair.is_null_ = false;
      ObString key;
      int64_t next = 0;
      if (OB_FAIL(fs_decode_json_string(alloc, line, i + 1, next, key))) {
      } else {
        pair.key_ = key;
        i = next;
        while (i < n && (p[i] == ' ' || p[i] == '\t')) { ++i; }
        if (i >= n || p[i] != ':') {
          ret = OB_INVALID_DATA;
        } else {
          ++i;
          while (i < n && (p[i] == ' ' || p[i] == '\t')) { ++i; }
          if (i >= n) {
            ret = OB_INVALID_DATA;
          } else if (p[i] == '"') {
            ObString val;
            if (OB_FAIL(fs_decode_json_string(alloc, line, i + 1, next, val))) {
            } else {
              pair.raw_ = val;
              i = next;
            }
          } else if (p[i] == 'n' && i + 4 <= n && 0 == MEMCMP(p + i, "null", 4)) {
            pair.is_null_ = true;
            i += 4;
          } else if (p[i] == '{' || p[i] == '[') {
            const char open = p[i];
            const char close = (open == '{' ? '}' : ']');
            int64_t depth = 0;
            const int64_t start = i;
            while (i < n) {
              if (p[i] == open) { ++depth; }
              else if (p[i] == close) { --depth; if (0 == depth) { ++i; break; } }
              ++i;
            }
            pair.raw_.assign_ptr(const_cast<char *>(p + start), static_cast<ObString::obstr_size_t>(i - start));
          } else {
            const int64_t start = i;
            while (i < n && p[i] != ',' && p[i] != '}' && p[i] != ' ') { ++i; }
            pair.raw_.assign_ptr(const_cast<char *>(p + start), static_cast<ObString::obstr_size_t>(i - start));
          }
        }
      }
      if (OB_SUCC(ret) && OB_FAIL(pairs.push_back(pair))) {
      }
    }
  }
  return ret;
}

} // anonymous namespace

const char *ob_file_column_type_name(const ObFileColumnType type)
{
  const char *name = "VARCHAR";
  switch (type) {
    case ObFileColumnType::NULLTYPE: name = "VARCHAR"; break;
    case ObFileColumnType::BOOLEAN:  name = "BOOLEAN"; break;
    case ObFileColumnType::BIGINT:   name = "BIGINT"; break;
    case ObFileColumnType::DOUBLE:   name = "DOUBLE"; break;
    case ObFileColumnType::DATE:     name = "DATE"; break;
    case ObFileColumnType::DATETIME: name = "DATETIME"; break;
    case ObFileColumnType::VARCHAR:  name = "VARCHAR"; break;
    default: name = "VARCHAR"; break;
  }
  return name;
}

static bool fs_is_numeric(const ObFileColumnType t)
{
  return ObFileColumnType::BOOLEAN == t || ObFileColumnType::BIGINT == t
      || ObFileColumnType::DOUBLE == t;
}

ObFileColumnType ObFileScanUtils::widen(ObFileColumnType left, ObFileColumnType right)
{
  ObFileColumnType result = ObFileColumnType::VARCHAR;
  if (left == right) {
    result = left;
  } else if (ObFileColumnType::NULLTYPE == left) {
    result = right;
  } else if (ObFileColumnType::NULLTYPE == right) {
    result = left;
  } else if (fs_is_numeric(left) && fs_is_numeric(right)) {
    if (ObFileColumnType::DOUBLE == left || ObFileColumnType::DOUBLE == right) {
      result = ObFileColumnType::DOUBLE;
    } else if (ObFileColumnType::BIGINT == left || ObFileColumnType::BIGINT == right) {
      result = ObFileColumnType::BIGINT;
    } else {
      result = ObFileColumnType::BOOLEAN;
    }
  } else if ((ObFileColumnType::DATE == left || ObFileColumnType::DATETIME == left)
             && (ObFileColumnType::DATE == right || ObFileColumnType::DATETIME == right)) {
    result = ObFileColumnType::DATETIME;
  } else {
    result = ObFileColumnType::VARCHAR;
  }
  return result;
}

int ObFileScanUtils::parse_bool(const ObString &text, bool &value, bool &ok)
{
  int ret = OB_SUCCESS;
  ok = false;
  if (4 == text.length() && 0 == text.case_compare(ObString::make_string("true"))) {
    value = true;
    ok = true;
  } else if (5 == text.length() && 0 == text.case_compare(ObString::make_string("false"))) {
    value = false;
    ok = true;
  }
  return ret;
}

int ObFileScanUtils::parse_bigint(const ObString &text, int64_t &value, bool &ok)
{
  int ret = OB_SUCCESS;
  char buf[64];
  ok = false;
  if (text.length() > 0 && OB_SUCC(fs_to_cstr(text, buf, sizeof(buf)))) {
    char *end = NULL;
    errno = 0;
    const long long v = ::strtoll(buf, &end, 10);
    if (0 == errno && NULL != end && end == buf + text.length()) {
      value = static_cast<int64_t>(v);
      ok = true;
    }
  }
  return ret;
}

int ObFileScanUtils::parse_double(const ObString &text, double &value, bool &ok)
{
  int ret = OB_SUCCESS;
  char buf[64];
  ok = false;
  if (text.length() > 0 && OB_SUCC(fs_to_cstr(text, buf, sizeof(buf)))) {
    char *end = NULL;
    errno = 0;
    const double v = ::strtod(buf, &end);
    if (0 == errno && NULL != end && end == buf + text.length()) {
      value = v;
      ok = true;
    }
  }
  return ret;
}

int ObFileScanUtils::parse_date(const ObString &text, int32_t &days, bool &ok)
{
  int ret = OB_SUCCESS;
  ok = false;
  const char *p = text.ptr();
  const int64_t n = text.length();
  if (10 == n && '-' == p[4] && '-' == p[7]) {
    bool digits = true;
    for (int64_t i = 0; i < n && digits; ++i) {
      if (i != 4 && i != 7 && !fs_is_digit(p[i])) { digits = false; }
    }
    if (digits) {
      const int64_t y = (p[0] - '0') * 1000 + (p[1] - '0') * 100 + (p[2] - '0') * 10 + (p[3] - '0');
      const int64_t m = (p[5] - '0') * 10 + (p[6] - '0');
      const int64_t d = (p[8] - '0') * 10 + (p[9] - '0');
      if (m >= 1 && m <= 12 && d >= 1 && d <= 31) {
        days = static_cast<int32_t>(fs_days_from_civil(y, m, d));
        ok = true;
      }
    }
  }
  return ret;
}

int ObFileScanUtils::parse_datetime(const ObString &text, int64_t &usec, bool &ok)
{
  int ret = OB_SUCCESS;
  ok = false;
  const char *p = text.ptr();
  const int64_t n = text.length();
  if (19 == n && (' ' == p[10] || 'T' == p[10])
      && '-' == p[4] && '-' == p[7] && ':' == p[13] && ':' == p[16]) {
    int32_t days = 0;
    ObString date_part;
    date_part.assign_ptr(const_cast<char *>(p), 10);
    bool date_ok = false;
    if (OB_FAIL(parse_date(date_part, days, date_ok))) {
    } else if (!date_ok) {
    } else {
      bool digits = true;
      for (int64_t i = 11; i < n && digits; ++i) {
        if (i != 13 && i != 16 && !fs_is_digit(p[i])) { digits = false; }
      }
      if (digits) {
        const int64_t hh = (p[11] - '0') * 10 + (p[12] - '0');
        const int64_t mm = (p[14] - '0') * 10 + (p[15] - '0');
        const int64_t ss = (p[17] - '0') * 10 + (p[18] - '0');
        if (hh <= 23 && mm <= 59 && ss <= 59) {
          usec = (static_cast<int64_t>(days) * 86400 + hh * 3600 + mm * 60 + ss);
          ok = true;
        }
      }
    }
  }
  return ret;
}

ObFileColumnType ObFileScanUtils::type_of_text(const ObString &text)
{
  ObFileColumnType type = ObFileColumnType::VARCHAR;
  if (text.empty()) {
    type = ObFileColumnType::NULLTYPE;
  } else {
    bool ok = false;
    bool bv = false;
    int64_t iv = 0;
    double dv = 0.0;
    int32_t days = 0;
    int64_t usec = 0;
    if (OB_SUCCESS == parse_bool(text, bv, ok) && ok) {
      type = ObFileColumnType::BOOLEAN;
    } else if (OB_SUCCESS == parse_bigint(text, iv, ok) && ok) {
      type = ObFileColumnType::BIGINT;
    } else if (OB_SUCCESS == parse_double(text, dv, ok) && ok) {
      type = ObFileColumnType::DOUBLE;
    } else if (OB_SUCCESS == parse_datetime(text, usec, ok) && ok) {
      type = ObFileColumnType::DATETIME;
    } else if (OB_SUCCESS == parse_date(text, days, ok) && ok) {
      type = ObFileColumnType::DATE;
    } else {
      type = ObFileColumnType::VARCHAR;
    }
  }
  return type;
}

int ObFileScanUtils::convert_field(ObIAllocator &alloc, const ObFileColumnType type,
                                   const ObString &text, bool &is_null, int64_t &int_value,
                                   double &double_value, ObString &str_value)
{
  int ret = OB_SUCCESS;
  is_null = false;
  int_value = 0;
  double_value = 0.0;
  str_value.reset();
  bool ok = false;
  if (ObFileColumnType::NULLTYPE == type || text.empty()) {
    is_null = true;
  } else if (ObFileColumnType::BOOLEAN == type) {
    bool bv = false;
    if (OB_FAIL(parse_bool(text, bv, ok))) {
    } else if (ok) {
      int_value = bv ? 1 : 0;
    } else {
      is_null = true;
    }
  } else if (ObFileColumnType::BIGINT == type) {
    if (OB_FAIL(parse_bigint(text, int_value, ok))) {
    } else if (!ok) {
      is_null = true;
    }
  } else if (ObFileColumnType::DOUBLE == type) {
    if (OB_FAIL(parse_double(text, double_value, ok))) {
    } else if (!ok) {
      is_null = true;
    }
  } else if (ObFileColumnType::DATE == type) {
    int32_t days = 0;
    if (OB_FAIL(parse_date(text, days, ok))) {
    } else if (!ok) {
      is_null = true;
    } else {
      str_value = text;
    }
  } else if (ObFileColumnType::DATETIME == type) {
    int64_t usec = 0;
    if (OB_FAIL(parse_datetime(text, usec, ok))) {
    } else if (!ok) {
      is_null = true;
    } else if (19 == text.length() && 'T' == text.ptr()[10]) {
      // normalise the isoT separator to the space OceanBase uses
      char *copy = static_cast<char *>(alloc.alloc(text.length()));
      if (OB_ISNULL(copy)) {
        ret = OB_ALLOCATE_MEMORY_FAILED;
      } else {
        MEMCPY(copy, text.ptr(), text.length());
        copy[10] = ' ';
        str_value.assign_ptr(copy, text.length());
      }
    } else {
      str_value = text;
    }
  } else {
    str_value = text;
  }
  return ret;
}

int ObFileScanUtils::unique_column_name(ObIAllocator &alloc,
                                        ObIArray<ObFileColumnDef> &columns, ObString &name)
{
  int ret = OB_SUCCESS;
  bool exists = false;
  for (int64_t i = 0; !exists && i < columns.count(); ++i) {
    if (0 == columns.at(i).name_.case_compare(name)) {
      exists = true;
    }
  }
  if (exists) {
    int64_t suffix = 2;
    bool found = false;
    while (OB_SUCC(ret) && !found) {
      char num[16];
      const int len = ::snprintf(num, sizeof(num), "__%ld", static_cast<long>(suffix));
      const int64_t total = name.length() + len;
      char *buf = static_cast<char *>(alloc.alloc(total));
      if (OB_ISNULL(buf)) {
        ret = OB_ALLOCATE_MEMORY_FAILED;
      } else {
        MEMCPY(buf, name.ptr(), name.length());
        MEMCPY(buf + name.length(), num, len);
        ObString candidate;
        candidate.assign_ptr(buf, static_cast<ObString::obstr_size_t>(total));
        bool taken = false;
        for (int64_t i = 0; !taken && i < columns.count(); ++i) {
          if (0 == columns.at(i).name_.case_compare(candidate)) {
            taken = true;
          }
        }
        if (!taken) {
          name = candidate;
          found = true;
        } else {
          ++suffix;
        }
      }
    }
  }
  return ret;
}

int ObFileScanUtils::detect_format(const ObString &path, ObString &format)
{
  int ret = OB_SUCCESS;
  const char *p = path.ptr();
  const int64_t n = path.length();
  int64_t dot = -1;
  for (int64_t i = n - 1; i >= 0; --i) {
    if ('.' == p[i]) { dot = i; break; }
    if ('/' == p[i]) { break; }
  }
  if (dot < 0 || dot + 1 >= n) {
    ret = OB_NOT_SUPPORTED;
  } else {
    ObString ext;
    ext.assign_ptr(const_cast<char *>(p + dot + 1), static_cast<ObString::obstr_size_t>(n - dot - 1));
    if (0 == ext.case_compare(ObString::make_string("csv"))) {
      format = ObString::make_string("csv");
    } else if (0 == ext.case_compare(ObString::make_string("json"))
               || 0 == ext.case_compare(ObString::make_string("jsonl"))
               || 0 == ext.case_compare(ObString::make_string("ndjson"))) {
      format = ObString::make_string("json");
    } else if (0 == ext.case_compare(ObString::make_string("parquet"))) {
      format = ObString::make_string("parquet");
    } else {
      ret = OB_NOT_SUPPORTED;
    }
  }
  return ret;
}

int ObFileScanUtils::glob_files(ObIAllocator &alloc, const ObString &pattern,
                                ObIArray<ObString> &files)
{
  int ret = OB_SUCCESS;
  char pat[PATH_MAX];
  if (OB_FAIL(fs_to_cstr(pattern, pat, sizeof(pat)))) {
  } else {
    glob_t globbuf;
    MEMSET(&globbuf, 0, sizeof(globbuf));
    const int rc = ::glob(pat, 0, NULL, &globbuf);
    if (GLOB_NOMATCH == rc) {
      ret = OB_ENTRY_NOT_EXIST;
    } else if (0 != rc) {
      ret = OB_ERR_UNEXPECTED;
    } else {
      for (size_t i = 0; OB_SUCC(ret) && i < globbuf.gl_pathc; ++i) {
        const char *match = globbuf.gl_pathv[i];
        const int64_t len = static_cast<int64_t>(strlen(match));
        char *buf = static_cast<char *>(alloc.alloc(len));
        if (OB_ISNULL(buf)) {
          ret = OB_ALLOCATE_MEMORY_FAILED;
        } else {
          MEMCPY(buf, match, len);
          ObString s;
          s.assign_ptr(buf, static_cast<ObString::obstr_size_t>(len));
          if (OB_FAIL(files.push_back(s))) {
          }
        }
      }
    }
    ::globfree(&globbuf);
  }
  return ret;
}

// Fold one row into the column list: widen the type, remember nullability.
static int fs_fold_row(const ObFileRow &row, ObIArray<ObFileColumnDef> &cols)
{
  int ret = OB_SUCCESS;
  for (int64_t c = 0; OB_SUCC(ret) && c < cols.count(); ++c) {
    if (c < row.count() && !row.at(c).is_null_ && !row.at(c).text_.empty()) {
      const ObFileColumnType t = ObFileScanUtils::type_of_text(row.at(c).text_);
      if (ObFileColumnType::NULLTYPE == t) {
        cols.at(c).nullable_ = true;
      } else {
        cols.at(c).type_ = ObFileScanUtils::widen(cols.at(c).type_, t);
      }
    } else {
      cols.at(c).nullable_ = true;
    }
  }
  return ret;
}

int ObFileScanUtils::infer_csv_schema(ObIAllocator &alloc, const ObString &path,
                                      const ObFileTableArgs &args, ObFileTableMeta &meta)
{
  int ret = OB_SUCCESS;
  if (args.has_sample_size_ && -1 != args.sample_size_) {
    // only "look at the whole file" is supported
    ret = OB_NOT_SUPPORTED;
  } else {
    ObFileTableArgs open_args = args;
    open_args.has_header_ = true;
    open_args.header_ = false;  // read the first physical line as data
    ObFileCsvReader reader;
    ObSEArray<ObFileColumnDef, 16> no_cols;
    ObFileRow first_row;
    ObFileRow second_row;
    bool have_first = false;
    bool have_second = false;
    int64_t row_count = 0;
    ObSEArray<ObFileColumnDef, 16> cols;
    if (OB_FAIL(reader.open(alloc, path, open_args, no_cols))) {
    } else if (OB_FAIL(reader.get_next_row(alloc, first_row))) {
      if (OB_ITER_END == ret) { ret = OB_SUCCESS; }
    } else {
      have_first = true;
    }
    if (OB_SUCC(ret) && have_first) {
      if (OB_FAIL(reader.get_next_row(alloc, second_row))) {
        if (OB_ITER_END == ret) { ret = OB_SUCCESS; }
      } else {
        have_second = true;
      }
    }
    bool has_header = false;
    if (OB_SUCC(ret) && have_first) {
      if (args.has_header_) {
        has_header = args.header_;
      } else {
        bool first_is_text = true;
        for (int64_t i = 0; i < first_row.count(); ++i) {
          if (ObFileColumnType::VARCHAR != type_of_text(first_row.at(i).text_)) {
            first_is_text = false;
          }
        }
        bool second_has_value = false;
        for (int64_t i = 0; i < second_row.count(); ++i) {
          const ObFileColumnType t = type_of_text(second_row.at(i).text_);
          if (ObFileColumnType::VARCHAR != t && ObFileColumnType::NULLTYPE != t) {
            second_has_value = true;
          }
        }
        has_header = first_is_text && (!have_second || second_has_value);
      }
    }
    if (OB_SUCC(ret) && have_first) {
      const ObFileRow &name_row = first_row;
      for (int64_t i = 0; OB_SUCC(ret) && i < name_row.count(); ++i) {
        ObFileColumnDef def;
        def.type_ = ObFileColumnType::NULLTYPE;
        if (has_header) {
          def.name_ = name_row.at(i).text_;
          if (OB_FAIL(unique_column_name(alloc, cols, def.name_))) {
          }
        } else {
          char buf[32];
          const int len = ::snprintf(buf, sizeof(buf), "column%ld", static_cast<long>(i));
          char *copy = static_cast<char *>(alloc.alloc(len));
          if (OB_ISNULL(copy)) {
            ret = OB_ALLOCATE_MEMORY_FAILED;
          } else {
            MEMCPY(copy, buf, len);
            def.name_.assign_ptr(copy, static_cast<ObString::obstr_size_t>(len));
          }
        }
        if (OB_SUCC(ret) && OB_FAIL(cols.push_back(def))) {
        }
      }
    }
    if (OB_SUCC(ret) && has_header && have_second) {
      if (OB_FAIL(fs_fold_row(second_row, cols))) {
      } else {
        ++row_count;
      }
    } else if (OB_SUCC(ret) && !has_header && have_first) {
      if (OB_FAIL(fs_fold_row(first_row, cols))) {
      } else {
        ++row_count;
      }
    }
    while (OB_SUCC(ret)) {
      ObFileRow row;
      if (OB_FAIL(reader.get_next_row(alloc, row))) {
        if (OB_ITER_END == ret) { ret = OB_SUCCESS; }
        break;
      } else if (OB_FAIL(fs_fold_row(row, cols))) {
      } else {
        ++row_count;
      }
    }
    if (OB_SUCC(ret)) {
      for (int64_t i = 0; i < cols.count(); ++i) {
        if (ObFileColumnType::NULLTYPE == cols.at(i).type_) {
          cols.at(i).type_ = ObFileColumnType::VARCHAR;
        }
      }
      meta.has_header_ = has_header;
      meta.row_count_ = row_count;
      meta.file_path_ = path;
      meta.columns_.reset();
      for (int64_t i = 0; OB_SUCC(ret) && i < cols.count(); ++i) {
        if (OB_FAIL(meta.columns_.push_back(cols.at(i)))) {
        }
      }
    }
    reader.close();
  }
  return ret;
}

int ObFileScanUtils::infer_json_schema(ObIAllocator &alloc, const ObString &path,
                                       ObFileTableMeta &meta)
{
  int ret = OB_SUCCESS;
  ObFileJsonReader reader;
  ObSEArray<ObFileColumnDef, 16> no_cols;
  ObSEArray<ObFileColumnDef, 16> cols;
  int64_t row_count = 0;
  if (OB_FAIL(reader.open(alloc, path, no_cols))) {
  } else {
    while (OB_SUCC(ret)) {
      ObString line;
      if (OB_FAIL(reader.next_line(alloc, line))) {
        if (OB_ITER_END == ret) { ret = OB_SUCCESS; }
        break;
      } else {
        ObSEArray<FileSqlJsonPair, 16> pairs;
        if (OB_FAIL(fs_parse_json_line(alloc, line, pairs))) {
          // a malformed line is skipped rather than failing the whole inference
          ret = OB_SUCCESS;
        } else {
          ++row_count;
          for (int64_t j = 0; OB_SUCC(ret) && j < pairs.count(); ++j) {
            bool known = false;
            for (int64_t c = 0; !known && c < cols.count(); ++c) {
              if (0 == cols.at(c).name_.compare(pairs.at(j).key_)) {
                known = true;
              }
            }
            if (!known) {
              ObFileColumnDef def;
              def.name_ = pairs.at(j).key_;
              if (pairs.at(j).is_null_) {
                def.type_ = ObFileColumnType::NULLTYPE;
                def.nullable_ = true;
              } else {
                def.type_ = type_of_text(pairs.at(j).raw_);
              }
              if (OB_FAIL(cols.push_back(def))) {
              }
            }
          }
          for (int64_t c = 0; OB_SUCC(ret) && c < cols.count(); ++c) {
            bool found = false;
            for (int64_t j = 0; !found && j < pairs.count(); ++j) {
              if (0 == cols.at(c).name_.compare(pairs.at(j).key_)) {
                found = true;
                if (pairs.at(j).is_null_) {
                  cols.at(c).nullable_ = true;
                } else {
                  cols.at(c).type_ = widen(cols.at(c).type_, type_of_text(pairs.at(j).raw_));
                }
              }
            }
            if (!found) {
              cols.at(c).nullable_ = true;
            }
          }
        }
      }
    }
    if (OB_SUCC(ret)) {
      for (int64_t i = 0; i < cols.count(); ++i) {
        if (ObFileColumnType::NULLTYPE == cols.at(i).type_) {
          cols.at(i).type_ = ObFileColumnType::VARCHAR;
        }
      }
      meta.has_header_ = true;
      meta.row_count_ = row_count;
      meta.file_path_ = path;
      meta.columns_.reset();
      for (int64_t i = 0; OB_SUCC(ret) && i < cols.count(); ++i) {
        if (OB_FAIL(meta.columns_.push_back(cols.at(i)))) {
        }
      }
    }
    reader.close();
  }
  return ret;
}

// --------------------------------------------------------------------------
// CSV
// --------------------------------------------------------------------------

ObFileCsvReader::ObFileCsvReader()
    : fd_(-1), is_inited_(false), eof_(false), delimiter_(','), nullstr_(), buffer_(),
      pos_(0), io_buf_(NULL), column_count_(0), skip_header_(true), header_consumed_(false)
{}

ObFileCsvReader::~ObFileCsvReader() { close(); }

int ObFileCsvReader::open(ObIAllocator &alloc, const ObString &path,
                          const ObFileTableArgs &args,
                          const ObSEArray<ObFileColumnDef, 16> &columns)
{
  int ret = OB_SUCCESS;
  char cpath[PATH_MAX];
  if (is_inited_) {
    ret = OB_INIT_TWICE;
  } else if (OB_FAIL(fs_to_cstr(path, cpath, sizeof(cpath)))) {
    LOG_WARN("path too long", K(ret), K(path));
  } else if (-1 == (fd_ = ::open(cpath, O_RDONLY))) {
    ret = OB_IO_ERROR;
    LOG_WARN("fail to open file", K(ret), K(path), K(errno));
  } else if (OB_ISNULL(io_buf_ = static_cast<char *>(alloc.alloc(FILE_SQL_IO_BUF_SIZE)))) {
    ret = OB_ALLOCATE_MEMORY_FAILED;
  } else {
    delimiter_ = args.has_delimiter_ ? args.delimiter_ : ',';
    nullstr_ = args.has_nullstr_ ? args.nullstr_ : ObString();
    column_count_ = columns.count();
    skip_header_ = args.has_header_ ? args.header_ : true;
    is_inited_ = true;
  }
  return ret;
}

void ObFileCsvReader::close()
{
  if (fd_ >= 0) {
    ::close(fd_);
    fd_ = -1;
  }
  is_inited_ = false;
}

int ObFileCsvReader::refill()
{
  int ret = OB_SUCCESS;
  const ssize_t n = ::read(fd_, io_buf_, FILE_SQL_IO_BUF_SIZE);
  if (n < 0) {
    ret = OB_IO_ERROR;
    LOG_WARN("fail to read file", K(ret), K(errno));
  } else if (0 == n) {
    eof_ = true;
  } else {
    buffer_.assign_ptr(io_buf_, static_cast<ObString::obstr_size_t>(n));
    pos_ = 0;
  }
  return ret;
}

int ObFileCsvReader::read_field(ObIAllocator &alloc, ObFileRowValue &value, bool &row_end)
{
  int ret = OB_SUCCESS;
  value.reset();
  row_end = false;
  char *buf = NULL;
  int64_t len = 0;
  int64_t cap = 0;
  bool was_quoted = false;
  bool in_quotes = false;
  bool any = false;
  bool at_eof = false;
  bool stop = false;
  while (OB_SUCC(ret) && !stop) {
    if (pos_ >= buffer_.length()) {
      if (!eof_ && OB_FAIL(refill())) {
      } else if (pos_ >= buffer_.length()) {
        at_eof = true;
      }
    }
    if (OB_FAIL(ret)) {
    } else if (at_eof) {
      if (!any) {
        ret = OB_ITER_END;
      } else {
        row_end = true;
        stop = true;
      }
    } else {
      const char c = buffer_[pos_++];
      if (!any) {
        any = true;
        if ('"' == c) {
          in_quotes = true;
          was_quoted = true;
        } else if (delimiter_ == c) {
          row_end = false;
          stop = true;
        } else if ('\n' == c) {
          row_end = true;
          stop = true;
        } else if ('\r' == c) {
          if (pos_ >= buffer_.length() && !eof_ && OB_FAIL(refill())) {
          }
          if (OB_SUCC(ret) && pos_ < buffer_.length() && '\n' == buffer_[pos_]) { ++pos_; }
          row_end = true;
          stop = true;
        } else if (OB_FAIL(fs_append(alloc, buf, len, cap, c))) {
        }
      } else if (in_quotes) {
        if ('"' == c) {
          if (pos_ >= buffer_.length() && !eof_ && OB_FAIL(refill())) {
          }
          if (OB_SUCC(ret) && pos_ < buffer_.length() && '"' == buffer_[pos_]) {
            ++pos_;
            if (OB_FAIL(fs_append(alloc, buf, len, cap, '"'))) {
            }
          } else {
            in_quotes = false;
          }
        } else if (OB_FAIL(fs_append(alloc, buf, len, cap, c))) {
        }
      } else {
        if (delimiter_ == c) {
          stop = true;
        } else if ('\n' == c) {
          row_end = true;
          stop = true;
        } else if ('\r' == c) {
          if (pos_ >= buffer_.length() && !eof_ && OB_FAIL(refill())) {
          }
          if (OB_SUCC(ret) && pos_ < buffer_.length() && '\n' == buffer_[pos_]) { ++pos_; }
          row_end = true;
          stop = true;
        } else if (OB_FAIL(fs_append(alloc, buf, len, cap, c))) {
        }
      }
    }
  }
  if (OB_SUCC(ret) && OB_SUCC(ret)) {
    if (0 == len && !was_quoted) {
      value.is_null_ = true;
    } else if (!was_quoted && !nullstr_.empty() && nullstr_.length() == len
               && 0 == MEMCMP(nullstr_.ptr(), buf, len)) {
      value.is_null_ = true;
    } else {
      value.text_.assign_ptr(buf, static_cast<ObString::obstr_size_t>(len));
    }
  }
  return ret;
}

int ObFileCsvReader::get_next_row(ObIAllocator &alloc, ObFileRow &row)
{
  int ret = OB_SUCCESS;
  row.reset();
  if (!is_inited_) {
    ret = OB_NOT_INIT;
  } else if (!skip_header_ || header_consumed_) {
    // fall through, we are at a data row
  }
  if (OB_SUCC(ret) && skip_header_ && !header_consumed_) {
    // consume the header fields, dropping them
    ObFileRowValue tmp;
    bool row_end = false;
    bool more = true;
    while (OB_SUCC(ret) && more) {
      if (OB_FAIL(read_field(alloc, tmp, row_end))) {
      } else if (row_end) {
        more = false;
      }
    }
    if (OB_SUCC(ret)) {
      header_consumed_ = true;
    }
    if (OB_SUCC(ret)) {
      row.reset();
    }
    // if the header was the only line, the next read returns OB_ITER_END naturally
  }
  if (OB_SUCC(ret)) {
    bool row_end = false;
    bool at_start = true;
    // a row is a sequence of fields; the last one has row_end set
    while (OB_SUCC(ret) && (at_start || !row_end)) {
      at_start = false;
      ObFileRowValue value;
      if (OB_FAIL(read_field(alloc, value, row_end))) {
      } else if (OB_FAIL(row.push_back(value))) {
      }
    }
    if (OB_ITER_END == ret) {
      if (row.count() > 0) {
        ret = OB_SUCCESS;  // last line had no trailing newline
      }
    }
  }
  return ret;
}

// --------------------------------------------------------------------------
// JSONL
// --------------------------------------------------------------------------

ObFileJsonReader::ObFileJsonReader()
    : fd_(-1), is_inited_(false), eof_(false), buffer_(), pos_(0), io_buf_(NULL), col_names_()
{}

ObFileJsonReader::~ObFileJsonReader() { close(); }

int ObFileJsonReader::open(ObIAllocator &alloc, const ObString &path,
                           const ObSEArray<ObFileColumnDef, 16> &columns)
{
  int ret = OB_SUCCESS;
  char cpath[PATH_MAX];
  if (is_inited_) {
    ret = OB_INIT_TWICE;
  } else if (OB_FAIL(fs_to_cstr(path, cpath, sizeof(cpath)))) {
    LOG_WARN("path too long", K(ret), K(path));
  } else if (-1 == (fd_ = ::open(cpath, O_RDONLY))) {
    ret = OB_IO_ERROR;
    LOG_WARN("fail to open file", K(ret), K(path), K(errno));
  } else if (OB_ISNULL(io_buf_ = static_cast<char *>(alloc.alloc(FILE_SQL_IO_BUF_SIZE)))) {
    ret = OB_ALLOCATE_MEMORY_FAILED;
  } else {
    for (int64_t i = 0; OB_SUCC(ret) && i < columns.count(); ++i) {
      if (OB_FAIL(col_names_.push_back(columns.at(i).name_))) {
      }
    }
    is_inited_ = true;
  }
  return ret;
}

void ObFileJsonReader::close()
{
  if (fd_ >= 0) {
    ::close(fd_);
    fd_ = -1;
  }
  is_inited_ = false;
}

int ObFileJsonReader::refill()
{
  int ret = OB_SUCCESS;
  const ssize_t n = ::read(fd_, io_buf_, FILE_SQL_IO_BUF_SIZE);
  if (n < 0) {
    ret = OB_IO_ERROR;
    LOG_WARN("fail to read file", K(ret), K(errno));
  } else if (0 == n) {
    eof_ = true;
  } else {
    buffer_.assign_ptr(io_buf_, static_cast<ObString::obstr_size_t>(n));
    pos_ = 0;
  }
  return ret;
}

int ObFileJsonReader::next_line(ObIAllocator &alloc, ObString &line)
{
  int ret = OB_SUCCESS;
  char *line_buf = NULL;
  int64_t line_len = 0;
  int64_t line_cap = 0;
  bool got_line = false;
  bool stop = false;
  while (OB_SUCC(ret) && !stop) {
    if (pos_ >= buffer_.length()) {
      if (eof_ || OB_FAIL(refill())) {
        if (OB_SUCC(ret)) {
          stop = true;
          if (0 == line_len) {
            ret = OB_ITER_END;
          }
        }
      }
    }
    if (OB_FAIL(ret)) {
    } else if (pos_ < buffer_.length()) {
      const char c = buffer_[pos_++];
      if ('\n' == c) {
        if (line_len > 0) {
          got_line = true;
          stop = true;
        }
        // else: blank line, keep looking
      } else if ('\r' == c) {
        // ignore
      } else if (OB_FAIL(fs_append(alloc, line_buf, line_len, line_cap, c))) {
      }
    }
  }
  if (OB_SUCC(ret) && got_line) {
    line.assign_ptr(line_buf, static_cast<ObString::obstr_size_t>(line_len));
  } else if (OB_SUCC(ret)) {
    ret = OB_ITER_END;
  }
  return ret;
}

int ObFileJsonReader::get_next_row(ObIAllocator &alloc, ObFileRow &row)
{
  int ret = OB_SUCCESS;
  row.reset();
  ObString line;
  if (!is_inited_) {
    ret = OB_NOT_INIT;
  } else if (OB_FAIL(next_line(alloc, line))) {
  } else {
    ObSEArray<FileSqlJsonPair, 16> pairs;
    if (OB_FAIL(fs_parse_json_line(alloc, line, pairs))) {
      LOG_WARN("fail to parse json line", K(ret), K(line));
    } else {
      for (int64_t i = 0; OB_SUCC(ret) && i < col_names_.count(); ++i) {
        ObFileRowValue value;
        bool found = false;
        for (int64_t j = 0; !found && j < pairs.count(); ++j) {
          if (0 == pairs.at(j).key_.compare(col_names_.at(i))) {
            found = true;
            if (pairs.at(j).is_null_) {
              value.is_null_ = true;
            } else {
              value.text_ = pairs.at(j).raw_;
            }
          }
        }
        if (!found) {
          value.is_null_ = true;
        }
        if (OB_FAIL(row.push_back(value))) {
        }
      }
    }
  }
  return ret;
}

// ---------------------------------------------------------------------------
// Parquet
// ---------------------------------------------------------------------------
// A parquet file keeps its schema and the location of every column chunk in a
// thrift-compact encoded footer, so reading one needs a thrift-compact reader, a
// snappy decoder (parquet's usual column codec) and the rle/bit-packed hybrid
// decoder used for definition levels. Only flat primitive columns are handled.
namespace
{

struct ParquetThrift
{
  const char *p_;
  const char *end_;
};

int pt_byte(ParquetThrift &t, uint8_t &v)
{
  int ret = OB_SUCCESS;
  if (t.p_ >= t.end_) {
    ret = OB_INVALID_DATA;
  } else {
    v = static_cast<uint8_t>(*t.p_++);
  }
  return ret;
}

int pt_varint(ParquetThrift &t, uint64_t &v)
{
  int ret = OB_SUCCESS;
  v = 0;
  int shift = 0;
  bool more = true;
  while (OB_SUCC(ret) && more) {
    uint8_t b = 0;
    if (OB_FAIL(pt_byte(t, b))) {
    } else if (shift > 63) {
      ret = OB_INVALID_DATA;
    } else {
      v |= static_cast<uint64_t>(b & 0x7f) << shift;
      more = (0 != (b & 0x80));
      shift += 7;
    }
  }
  return ret;
}

int pt_zigzag(ParquetThrift &t, int64_t &v)
{
  int ret = OB_SUCCESS;
  uint64_t u = 0;
  if (OB_FAIL(pt_varint(t, u))) {
  } else {
    v = static_cast<int64_t>(u >> 1) ^ -static_cast<int64_t>(u & 1);
  }
  return ret;
}

int pt_binary(ParquetThrift &t, const char *&ptr, int64_t &len)
{
  int ret = OB_SUCCESS;
  uint64_t n = 0;
  if (OB_FAIL(pt_varint(t, n))) {
  } else if (n > static_cast<uint64_t>(t.end_ - t.p_)) {
    ret = OB_INVALID_DATA;
  } else {
    ptr = t.p_;
    len = static_cast<int64_t>(n);
    t.p_ += n;
  }
  return ret;
}

int pt_skip(ParquetThrift &t, uint8_t type);

int pt_skip_struct(ParquetThrift &t)
{
  int ret = OB_SUCCESS;
  int64_t last = 0;
  bool stop = false;
  while (OB_SUCC(ret) && !stop) {
    uint8_t h = 0;
    if (OB_FAIL(pt_byte(t, h))) {
    } else {
      const uint8_t type = h & 0x0f;
      const uint8_t delta = h >> 4;
      if (0 == type) {
        stop = true;
      } else if (0 == delta) {
        int64_t id = 0;
        if (OB_FAIL(pt_zigzag(t, id))) {
        } else {
          last = id;
          if (OB_FAIL(pt_skip(t, type))) {
          }
        }
      } else {
        last += delta;
        if (OB_FAIL(pt_skip(t, type))) {
        }
      }
    }
  }
  return ret;
}

int pt_skip(ParquetThrift &t, uint8_t type)
{
  int ret = OB_SUCCESS;
  switch (type) {
    case 1:
    case 2: {
      break;  // bool in a field is in the header, no payload
    }
    case 3: {
      uint8_t b = 0;
      ret = pt_byte(t, b);
      break;
    }
    case 4:
    case 5:
    case 6: {
      int64_t v = 0;
      ret = pt_zigzag(t, v);
      break;
    }
    case 7: {
      if (t.end_ - t.p_ < 8) {
        ret = OB_INVALID_DATA;
      } else {
        t.p_ += 8;
      }
      break;
    }
    case 8: {
      const char *p = NULL;
      int64_t n = 0;
      ret = pt_binary(t, p, n);
      break;
    }
    case 9:
    case 10: {
      uint8_t h = 0;
      if (OB_FAIL(pt_byte(t, h))) {
      } else {
        uint64_t sz = h >> 4;
        const uint8_t et = h & 0x0f;
        if (15 == sz) {
          if (OB_FAIL(pt_varint(t, sz))) {
          }
        }
        for (uint64_t i = 0; OB_SUCC(ret) && i < sz; ++i) {
          if (1 == et || 2 == et) {
            uint8_t b = 0;
            if (OB_FAIL(pt_byte(t, b))) {
            }
          } else if (OB_FAIL(pt_skip(t, et))) {
          }
        }
      }
      break;
    }
    case 11: {
      uint8_t h = 0;
      if (OB_FAIL(pt_byte(t, h))) {
      } else {
        uint64_t sz = h >> 4;
        const uint8_t kt = h & 0x0f;
        const uint8_t vt = (h >> 4) & 0x0f;
        if (0 == sz) {
          if (OB_FAIL(pt_varint(t, sz))) {
          }
        }
        for (uint64_t i = 0; OB_SUCC(ret) && i < sz; ++i) {
          if (OB_FAIL(pt_skip(t, kt))) {
          } else if (OB_FAIL(pt_skip(t, vt))) {
          }
        }
      }
      break;
    }
    case 12: {
      ret = pt_skip_struct(t);
      break;
    }
    default: {
      ret = OB_INVALID_DATA;
      break;
    }
  }
  return ret;
}

// One leaf column of the file schema plus where to find its values.
struct ParquetColumn
{
  ParquetColumn()
      : physical_type_(0), repetition_(0), codec_(0), num_values_(0),
        data_page_offset_(0), name_() {}
  int32_t physical_type_;   // parquet Type: 0 bool 1 int32 2 int64 4 float 5 double 6 byte_array
  int32_t repetition_;      // 0 required 1 optional 2 repeated
  int32_t codec_;           // 0 none 1 snappy
  int64_t num_values_;
  int64_t data_page_offset_;
  common::ObString name_;
};

// A column chunk as described inside its row group.
struct ParquetChunk
{
  ParquetChunk() : col_idx_(0), codec_(0), num_values_(0), data_page_offset_(0) {}
  int64_t col_idx_;
  int32_t codec_;
  int64_t num_values_;
  int64_t data_page_offset_;
};

// Read the next field header of a struct: returns the field id and type, or
// stop = true at the end of the struct.
int pt_field(ParquetThrift &t, int64_t &last, bool &stop, int64_t &id, uint8_t &type)
{
  int ret = OB_SUCCESS;
  uint8_t h = 0;
  stop = false;
  if (OB_FAIL(pt_byte(t, h))) {
  } else {
    type = h & 0x0f;
    const uint8_t delta = h >> 4;
    if (0 == type) {
      stop = true;
    } else if (0 == delta) {
      if (OB_FAIL(pt_zigzag(t, id))) {
      } else {
        last = id;
      }
    } else {
      id = last + delta;
      last = id;
    }
  }
  return ret;
}

int pt_read_i32(ParquetThrift &t, int32_t &v)
{
  int ret = OB_SUCCESS;
  int64_t s = 0;
  if (OB_FAIL(pt_zigzag(t, s))) {
  } else {
    v = static_cast<int32_t>(s);
  }
  return ret;
}

int pt_read_i64(ParquetThrift &t, int64_t &v)
{
  return pt_zigzag(t, v);
}

// SchemaElement: 1 type, 3 repetition_type, 4 name.
int pt_parse_schema_element(ParquetThrift &t, int32_t &type, int32_t &repetition,
                            common::ObString &name)
{
  int ret = OB_SUCCESS;
  int64_t last = 0;
  bool stop = false;
  type = -1;
  repetition = 0;
  while (OB_SUCC(ret) && !stop) {
    int64_t id = 0;
    uint8_t ft = 0;
    if (OB_FAIL(pt_field(t, last, stop, id, ft))) {
    } else if (stop) {
    } else if (1 == id && 5 == ft) {
      if (OB_FAIL(pt_read_i32(t, type))) {
      }
    } else if (3 == id && 5 == ft) {
      if (OB_FAIL(pt_read_i32(t, repetition))) {
      }
    } else if (4 == id && 8 == ft) {
      const char *p = NULL;
      int64_t n = 0;
      if (OB_FAIL(pt_binary(t, p, n))) {
      } else {
        name.assign_ptr(p, static_cast<common::ObString::obstr_size_t>(n));
      }
    } else if (OB_FAIL(pt_skip(t, ft))) {
    }
  }
  return ret;
}

// ColumnMetaData: 1 type, 4 codec, 5 num_values, 9 data_page_offset.
int pt_parse_column_meta(ParquetThrift &t, ParquetChunk &chunk)
{
  int ret = OB_SUCCESS;
  int64_t last = 0;
  bool stop = false;
  while (OB_SUCC(ret) && !stop) {
    int64_t id = 0;
    uint8_t ft = 0;
    if (OB_FAIL(pt_field(t, last, stop, id, ft))) {
    } else if (stop) {
    } else if (1 == id && 5 == ft) {
      if (OB_FAIL(pt_read_i32(t, chunk.codec_ /*unused*/))) {
      }
    } else if (4 == id && 5 == ft) {
      if (OB_FAIL(pt_read_i32(t, chunk.codec_))) {
      }
    } else if (5 == id && 6 == ft) {
      if (OB_FAIL(pt_read_i64(t, chunk.num_values_))) {
      }
    } else if (9 == id && 6 == ft) {
      if (OB_FAIL(pt_read_i64(t, chunk.data_page_offset_))) {
      }
    } else if (OB_FAIL(pt_skip(t, ft))) {
    }
  }
  return ret;
}

// A row group: 1 columns (list<ColumnChunk>), 3 num_rows.
int pt_parse_row_group(ParquetThrift &t, common::ObIArray<ParquetChunk> &chunks,
                       int64_t &row_count)
{
  int ret = OB_SUCCESS;
  int64_t last = 0;
  bool stop = false;
  row_count = 0;
  while (OB_SUCC(ret) && !stop) {
    int64_t id = 0;
    uint8_t ft = 0;
    if (OB_FAIL(pt_field(t, last, stop, id, ft))) {
    } else if (stop) {
    } else if (1 == id && 9 == ft) {
      uint8_t h = 0;
      if (OB_FAIL(pt_byte(t, h))) {
      } else {
        uint64_t sz = h >> 4;
        const uint8_t et = h & 0x0f;
        if (15 == sz) {
          if (OB_FAIL(pt_varint(t, sz))) {
          }
        }
        for (uint64_t i = 0; OB_SUCC(ret) && i < sz; ++i) {
          if (12 != et) {
            ret = OB_INVALID_DATA;
          } else {
            ParquetChunk chunk;
            int64_t clast = 0;
            bool cstop = false;
            while (OB_SUCC(ret) && !cstop) {
              int64_t cid = 0;
              uint8_t ct = 0;
              if (OB_FAIL(pt_field(t, clast, cstop, cid, ct))) {
              } else if (cstop) {
              } else if (3 == cid && 12 == ct) {
                if (OB_FAIL(pt_parse_column_meta(t, chunk))) {
                }
              } else if (OB_FAIL(pt_skip(t, ct))) {
              }
            }
            if (OB_SUCC(ret) && OB_FAIL(chunks.push_back(chunk))) {
            }
          }
        }
      }
    } else if (3 == id && 6 == ft) {
      if (OB_FAIL(pt_read_i64(t, row_count))) {
      }
    } else if (OB_FAIL(pt_skip(t, ft))) {
    }
  }
  return ret;
}

// FileMetaData: 2 schema, 3 num_rows, 4 row_groups.
int pt_parse_file_meta(ParquetThrift &t,
                       common::ObIArray<ParquetColumn> &cols,
                       common::ObIArray<ParquetChunk> &chunks,
                       int64_t &num_rows)
{
  int ret = OB_SUCCESS;
  int64_t last = 0;
  bool stop = false;
  num_rows = 0;
  int64_t leaf_index = -1;
  while (OB_SUCC(ret) && !stop) {
    int64_t id = 0;
    uint8_t ft = 0;
    if (OB_FAIL(pt_field(t, last, stop, id, ft))) {
    } else if (stop) {
    } else if (2 == id && 9 == ft) {
      uint8_t h = 0;
      if (OB_FAIL(pt_byte(t, h))) {
      } else {
        uint64_t sz = h >> 4;
        const uint8_t et = h & 0x0f;
        if (15 == sz) {
          if (OB_FAIL(pt_varint(t, sz))) {
          }
        }
        for (uint64_t i = 0; OB_SUCC(ret) && i < sz; ++i) {
          if (12 != et) {
            ret = OB_INVALID_DATA;
          } else {
            ParquetColumn col;
            if (OB_FAIL(pt_parse_schema_element(t, col.physical_type_, col.repetition_,
                                                col.name_))) {
            } else if (col.physical_type_ < 0) {
              // the root element has no type; skip it
            } else if (OB_FAIL(cols.push_back(col))) {
            } else {
              ++leaf_index;
            }
          }
        }
      }
    } else if (3 == id && 6 == ft) {
      if (OB_FAIL(pt_read_i64(t, num_rows))) {
      }
    } else if (4 == id && 9 == ft) {
      uint8_t h = 0;
      if (OB_FAIL(pt_byte(t, h))) {
      } else {
        uint64_t sz = h >> 4;
        const uint8_t et = h & 0x0f;
        if (15 == sz) {
          if (OB_FAIL(pt_varint(t, sz))) {
          }
        }
        for (uint64_t i = 0; OB_SUCC(ret) && i < sz; ++i) {
          if (12 != et) {
            ret = OB_INVALID_DATA;
          } else {
            int64_t rc = 0;
            const int64_t before = chunks.count();
            if (OB_FAIL(pt_parse_row_group(t, chunks, rc))) {
            } else {
              for (int64_t k = before; OB_SUCC(ret) && k < chunks.count(); ++k) {
                chunks.at(k).col_idx_ = k - before;
              }
            }
          }
        }
      }
    } else if (OB_FAIL(pt_skip(t, ft))) {
    }
  }
  return ret;
}

int fs_read_whole_file(ObIAllocator &alloc, const ObString &path, char *&data, int64_t &size)
{
  int ret = OB_SUCCESS;
  char cpath[PATH_MAX];
  data = NULL;
  size = 0;
  if (OB_FAIL(fs_to_cstr(path, cpath, sizeof(cpath)))) {
  } else {
    const int fd = ::open(cpath, O_RDONLY);
    struct stat st;
    if (-1 == fd) {
      ret = OB_IO_ERROR;
    } else if (0 != ::fstat(fd, &st) || !S_ISREG(st.st_mode)) {
      ret = OB_IO_ERROR;
    } else if (OB_ISNULL(data = static_cast<char *>(alloc.alloc(st.st_size > 0 ? st.st_size : 1)))) {
      ret = OB_ALLOCATE_MEMORY_FAILED;
    } else {
      int64_t got = 0;
      while (OB_SUCC(ret) && got < st.st_size) {
        const ssize_t n = ::read(fd, data + got, st.st_size - got);
        if (n <= 0) {
          ret = OB_IO_ERROR;
        } else {
          got += n;
        }
      }
      size = st.st_size;
    }
    if (fd >= 0) {
      ::close(fd);
    }
  }
  return ret;
}

// Open a parquet file and read its footer metadata.
int fs_parquet_meta(ObIAllocator &alloc, const ObString &path,
                    common::ObIArray<ParquetColumn> &cols,
                    common::ObIArray<ParquetChunk> &chunks,
                    char *&file_data, int64_t &file_size, int64_t &num_rows)
{
  int ret = OB_SUCCESS;
  if (OB_FAIL(fs_read_whole_file(alloc, path, file_data, file_size))) {
  } else if (file_size < 12 || 0 != MEMCMP(file_data, "PAR1", 4)
             || 0 != MEMCMP(file_data + file_size - 4, "PAR1", 4)) {
    ret = OB_INVALID_DATA;
    LOG_WARN("not a parquet file", K(ret), K(file_size));
  } else {
    uint32_t footer_len = 0;
    MEMCPY(&footer_len, file_data + file_size - 8, 4);
    if (static_cast<int64_t>(footer_len) + 8 > file_size) {
      ret = OB_INVALID_DATA;
    } else {
      ParquetThrift t;
      t.p_ = file_data + file_size - 8 - footer_len;
      t.end_ = file_data + file_size - 8;
      if (OB_FAIL(pt_parse_file_meta(t, cols, chunks, num_rows))) {
        LOG_WARN("fail to parse parquet footer", K(ret));
      }
    }
  }
  return ret;
}

int fs_parquet_physical_to_type(const int32_t physical, ObFileColumnType &type)
{
  int ret = OB_SUCCESS;
  switch (physical) {
    case 0: type = ObFileColumnType::BOOLEAN; break;
    case 1: type = ObFileColumnType::BIGINT; break;
    case 2: type = ObFileColumnType::BIGINT; break;
    case 4: type = ObFileColumnType::DOUBLE; break;
    case 5: type = ObFileColumnType::DOUBLE; break;
    case 6: type = ObFileColumnType::VARCHAR; break;
    default:
      ret = OB_NOT_SUPPORTED;
      LOG_USER_ERROR(OB_NOT_SUPPORTED, "this parquet column type");
      break;
  }
  return ret;
}

// Snappy raw block format: a varint uncompressed length followed by a sequence
// of literal and copy elements.
int fs_snappy_uncompress(const char *in, const int64_t in_len,
                         char *out, const int64_t out_cap, int64_t &out_len)
{
  int ret = OB_SUCCESS;
  int64_t ip = 0;
  int64_t op = 0;
  uint64_t want = 0;
  int shift = 0;
  while (ip < in_len) {
    const uint8_t b = static_cast<uint8_t>(in[ip++]);
    want |= static_cast<uint64_t>(b & 0x7f) << shift;
    if (0 == (b & 0x80)) {
      break;
    }
    shift += 7;
  }
  if (static_cast<int64_t>(want) > out_cap) {
    ret = OB_INVALID_DATA;
  }
  while (OB_SUCC(ret) && ip < in_len && op < static_cast<int64_t>(want)) {
    const uint8_t tag = static_cast<uint8_t>(in[ip++]);
    const int kind = tag & 0x03;
    if (0 == kind) {
      int64_t len = tag >> 2;
      if (len >= 60) {
        const int extra = static_cast<int>(len - 59);
        if (ip + extra > in_len) {
          ret = OB_INVALID_DATA;
        } else {
          len = 0;
          for (int i = 0; i < extra; ++i) {
            len |= static_cast<int64_t>(static_cast<uint8_t>(in[ip++])) << (8 * i);
          }
          len += 1;
        }
      } else {
        len += 1;
      }
      if (OB_SUCC(ret)) {
        if (ip + len > in_len || op + len > out_cap) {
          ret = OB_INVALID_DATA;
        } else {
          MEMCPY(out + op, in + ip, len);
          ip += len;
          op += len;
        }
      }
    } else {
      int64_t len = 0;
      int64_t offset = 0;
      if (1 == kind) {
        len = ((tag >> 2) & 0x07) + 4;
        if (ip >= in_len) {
          ret = OB_INVALID_DATA;
        } else {
          offset = ((static_cast<int64_t>(tag >> 5)) << 8)
                   | static_cast<uint8_t>(in[ip++]);
        }
      } else if (2 == kind) {
        len = (tag >> 2) + 1;
        if (ip + 2 > in_len) {
          ret = OB_INVALID_DATA;
        } else {
          offset = static_cast<uint8_t>(in[ip]) | (static_cast<int64_t>(static_cast<uint8_t>(in[ip + 1])) << 8);
          ip += 2;
        }
      } else {
        len = (tag >> 2) + 1;
        if (ip + 4 > in_len) {
          ret = OB_INVALID_DATA;
        } else {
          offset = static_cast<uint8_t>(in[ip])
                   | (static_cast<int64_t>(static_cast<uint8_t>(in[ip + 1])) << 8)
                   | (static_cast<int64_t>(static_cast<uint8_t>(in[ip + 2])) << 16)
                   | (static_cast<int64_t>(static_cast<uint8_t>(in[ip + 3])) << 24);
          ip += 4;
        }
      }
      if (OB_SUCC(ret)) {
        if (offset <= 0 || offset > op || op + len > out_cap) {
          ret = OB_INVALID_DATA;
        } else {
          for (int64_t i = 0; i < len; ++i) {
            out[op] = out[op - offset];
            ++op;
          }
        }
      }
    }
  }
  if (OB_SUCC(ret)) {
    out_len = op;
  }
  return ret;
}

int fs_bit_width(const uint32_t max_value, int &width)
{
  int ret = OB_SUCCESS;
  width = 0;
  uint32_t v = max_value;
  while (v > 0) {
    ++width;
    v >>= 1;
  }
  return ret;
}

} // anonymous namespace

int ObFileScanUtils::infer_parquet_schema(ObIAllocator &alloc, const ObString &path,
                                          ObFileTableMeta &meta)
{
  int ret = OB_SUCCESS;
  ObSEArray<ParquetColumn, 16> cols;
  ObSEArray<ParquetChunk, 16> chunks;
  char *data = NULL;
  int64_t size = 0;
  int64_t num_rows = 0;
  meta.columns_.reset();
  meta.row_count_ = 0;
  meta.has_header_ = false;
  meta.file_path_ = path;
  if (OB_FAIL(fs_parquet_meta(alloc, path, cols, chunks, data, size, num_rows))) {
  } else {
    for (int64_t i = 0; OB_SUCC(ret) && i < cols.count(); ++i) {
      ObFileColumnDef col;
      col.name_ = cols.at(i).name_;
      col.nullable_ = (1 == cols.at(i).repetition_);
      if (OB_FAIL(fs_parquet_physical_to_type(cols.at(i).physical_type_, col.type_))) {
      } else if (OB_FAIL(meta.columns_.push_back(col))) {
      }
    }
    meta.row_count_ = num_rows;
  }
  return ret;
}

struct ObParquetReader::State
{
  State()
      : col_count_(0), row_count_(0), row_idx_(0), cells_() {}
  int64_t col_count_;
  int64_t row_count_;
  int64_t row_idx_;
  common::ObSEArray<common::ObString, 64> cells_;
  common::ObSEArray<ParquetColumn, 16> cols_;
};

ObParquetReader::ObParquetReader() : state_(NULL), opened_(false) {}

ObParquetReader::~ObParquetReader() { close(); }

void ObParquetReader::close()
{
  opened_ = false;
  state_ = NULL;
}

int ObParquetReader::open(ObIAllocator &alloc, const ObString &path,
                          const ObSEArray<ObFileColumnDef, 16> &columns)
{
  int ret = OB_SUCCESS;
  ObSEArray<ParquetChunk, 16> chunks;
  char *data = NULL;
  int64_t size = 0;
  int64_t num_rows = 0;
  int64_t total_rows = 0;
  State *state = NULL;
  if (opened_) {
    ret = OB_INIT_TWICE;
  } else if (OB_ISNULL(state = static_cast<State *>(alloc.alloc(sizeof(State))))) {
    ret = OB_ALLOCATE_MEMORY_FAILED;
  } else {
    state = new (state) State();
    state_ = state;
    state->col_count_ = columns.count();
    if (OB_FAIL(fs_parquet_meta(alloc, path, state->cols_, chunks, data, size, num_rows))) {
    } else if (OB_UNLIKELY(state->cols_.count() != state->col_count_)) {
      ret = OB_INVALID_DATA;
      LOG_WARN("parquet column count mismatch", K(ret), K(state->cols_.count()), K(state->col_count_));
    } else {
      if (OB_UNLIKELY(chunks.count() != state->col_count_)) {
        ret = OB_NOT_SUPPORTED;
        LOG_USER_ERROR(OB_NOT_SUPPORTED, "a parquet file with several row groups");
      } else {
        total_rows = num_rows;
        for (int64_t i = 0; OB_SUCC(ret) && i < total_rows * state->col_count_; ++i) {
          if (OB_FAIL(state->cells_.push_back(ObString()))) {
          }
        }
        state->row_count_ = total_rows;
      }
    }
  }
  if (OB_SUCC(ret)) {
    // Decode each column chunk into the row-major cell array. The footer lists
    // chunks column by column, so the destination index has to be strided.
    const int64_t rows_per_group = num_rows;
    int64_t row_base = 0;
    for (int64_t c = 0; OB_SUCC(ret) && c < chunks.count(); ++c) {
      ParquetThrift t;
      t.p_ = data + chunks.at(c).data_page_offset_;
      t.end_ = data + size;
      uint8_t page_type = 0;
      (void)page_type;
      int64_t last = 0;
      bool stop = false;
      int64_t page_num_values = 0;
      int32_t page_encoding = 0;
      int32_t uncompressed_size = 0;
      int32_t compressed_size = 0;
      int32_t rep_len = 0;
      int32_t def_len = 0;
      bool values_compressed = true;
      while (OB_SUCC(ret) && !stop) {
        int64_t id = 0;
        uint8_t ft = 0;
        if (OB_FAIL(pt_field(t, last, stop, id, ft))) {
        } else if (stop) {
        } else if (1 == id && 5 == ft) {
          int32_t v = 0;
          if (OB_FAIL(pt_read_i32(t, v))) {
          } else {
            page_type = static_cast<uint8_t>(v);
          }
        } else if (2 == id && 5 == ft) {
          if (OB_FAIL(pt_read_i32(t, uncompressed_size))) {
          }
        } else if (3 == id && 5 == ft) {
          if (OB_FAIL(pt_read_i32(t, compressed_size))) {
          }
        } else if (5 == id && 12 == ft) {
          int64_t dlast = 0;
          bool dstop = false;
          while (OB_SUCC(ret) && !dstop) {
            int64_t did = 0;
            uint8_t dt = 0;
            if (OB_FAIL(pt_field(t, dlast, dstop, did, dt))) {
            } else if (dstop) {
            } else if (1 == did && 5 == dt) {
              int32_t v = 0;
              if (OB_FAIL(pt_read_i32(t, v))) {
              } else {
                page_num_values = v;
              }
            } else if (2 == did && 5 == dt) {
              if (OB_FAIL(pt_read_i32(t, page_encoding))) {
              }
            } else if (OB_FAIL(pt_skip(t, dt))) {
            }
          }
        } else if (8 == id && 12 == ft) {
          // DataPageHeaderV2: 1 num_values, 4 encoding, 5 def_len, 6 rep_len, 7 is_compressed
          int64_t dlast = 0;
          bool dstop = false;
          while (OB_SUCC(ret) && !dstop) {
            int64_t did = 0;
            uint8_t dt = 0;
            if (OB_FAIL(pt_field(t, dlast, dstop, did, dt))) {
            } else if (dstop) {
            } else if (1 == did && 5 == dt) {
              int32_t v = 0;
              if (OB_FAIL(pt_read_i32(t, v))) {
              } else {
                page_num_values = v;
              }
            } else if (4 == did && 5 == dt) {
              if (OB_FAIL(pt_read_i32(t, page_encoding))) {
              }
            } else if (5 == did && 5 == dt) {
              if (OB_FAIL(pt_read_i32(t, def_len))) {
              }
            } else if (6 == did && 5 == dt) {
              if (OB_FAIL(pt_read_i32(t, rep_len))) {
              }
            } else if (7 == did && (1 == dt || 2 == dt)) {
              values_compressed = (1 == dt);
            } else if (OB_FAIL(pt_skip(t, dt))) {
            }
          }
        } else if (OB_FAIL(pt_skip(t, ft))) {
        }
      }
      // A v2 data page keeps the repetition and definition levels uncompressed at
      // the front and compresses only the values, while a v1 page compresses
      // everything. Rebuild the v1 layout (a length prefixed level block followed
      // by the values) so the one decoder below serves both page versions.
      char *page = NULL;
      int64_t page_len = 0;
      const int32_t codec = chunks.at(c).codec_;
      if (OB_FAIL(ret)) {
      } else if (0 != page_type && 3 != page_type) {
        ret = OB_NOT_SUPPORTED;
        LOG_USER_ERROR(OB_NOT_SUPPORTED, "this parquet page type");
      } else if (0 != page_encoding) {
        ret = OB_NOT_SUPPORTED;
        LOG_USER_ERROR(OB_NOT_SUPPORTED, "dictionary encoded parquet data");
      } else if (t.p_ + compressed_size > data + size) {
        ret = OB_INVALID_DATA;
      } else if (3 != page_type) {
        // v1: the whole page is compressed
        char *whole = NULL;
        if (OB_ISNULL(whole = static_cast<char *>(alloc.alloc(uncompressed_size > 0 ? uncompressed_size : 1)))) {
          ret = OB_ALLOCATE_MEMORY_FAILED;
        } else if (0 == codec) {
          if (compressed_size > uncompressed_size) {
            ret = OB_INVALID_DATA;
          } else {
            MEMCPY(whole, t.p_, compressed_size);
            page_len = compressed_size;
          }
        } else if (1 == codec) {
          if (OB_FAIL(fs_snappy_uncompress(t.p_, compressed_size, whole, uncompressed_size, page_len))) {
            LOG_WARN("fail to snappy decompress", K(ret));
          }
        } else {
          ret = OB_NOT_SUPPORTED;
          LOG_USER_ERROR(OB_NOT_SUPPORTED, "this parquet compression");
        }
        page = whole;
      } else {
        // v2: levels are raw, only the values may be compressed
        if (rep_len + def_len > compressed_size || rep_len + def_len > uncompressed_size) {
          ret = OB_INVALID_DATA;
        } else {
          const char *val_in = t.p_ + rep_len + def_len;
          const int64_t val_in_len = compressed_size - rep_len - def_len;
          const int64_t val_out_len = uncompressed_size - rep_len - def_len;
          const bool has_def = (1 == state->cols_.at(c).repetition_);
          const int64_t head = has_def ? (4 + def_len) : 0;
          char *whole = NULL;
          if (OB_ISNULL(whole = static_cast<char *>(alloc.alloc(head + (val_out_len > 0 ? val_out_len : 1))))) {
            ret = OB_ALLOCATE_MEMORY_FAILED;
          } else {
            if (has_def) {
              uint32_t lvl = static_cast<uint32_t>(def_len);
              MEMCPY(whole, &lvl, 4);
              if (def_len > 0) {
                MEMCPY(whole + 4, t.p_ + rep_len, def_len);
              }
            }
            char *val_out = whole + head;
            int64_t got = 0;
            if (values_compressed && 1 == codec) {
              if (OB_FAIL(fs_snappy_uncompress(val_in, val_in_len, val_out, val_out_len, got))) {
                LOG_WARN("fail to snappy decompress", K(ret));
              }
            } else if (0 == codec || !values_compressed) {
              if (val_in_len > val_out_len) {
                ret = OB_INVALID_DATA;
              } else {
                MEMCPY(val_out, val_in, val_in_len);
                got = val_in_len;
              }
            } else {
              ret = OB_NOT_SUPPORTED;
              LOG_USER_ERROR(OB_NOT_SUPPORTED, "this parquet compression");
            }
            page = whole;
            page_len = head + got;
          }
        }
      }
      // definition levels come first when the column is optional
      int64_t dp = 0;
      ObSEArray<uint8_t, 256> def_levels;
      if (OB_SUCC(ret) && 1 == state->cols_.at(c).repetition_) {
        if (dp + 4 > page_len) {
          ret = OB_INVALID_DATA;
        } else {
          uint32_t lvl_len = 0;
          MEMCPY(&lvl_len, page + dp, 4);
          dp += 4;
          int width = 0;
          (void)fs_bit_width(1, width);
          const char *lvl = page + dp;
          const int64_t lvl_end = dp + lvl_len;
          int64_t out_i = 0;
          int64_t lp = 0;
          while (OB_SUCC(ret) && out_i < page_num_values && lp < lvl_len) {
            uint64_t header = 0;
            int shift = 0;
            while (lp < lvl_len) {
              const uint8_t b = static_cast<uint8_t>(lvl[lp++]);
              header |= static_cast<uint64_t>(b & 0x7f) << shift;
              if (0 == (b & 0x80)) {
                break;
              }
              shift += 7;
            }
            if (0 != (header & 1)) {
              const int64_t groups = header >> 1;
              const int64_t bits = groups * 8 * width;
              const int64_t nbytes = (bits + 7) / 8;
              for (int64_t g = 0; OB_SUCC(ret) && g < groups * 8 && out_i < page_num_values; ++g) {
                const int64_t bit_off = g * width;
                uint8_t v = 0;
                for (int w = 0; w < width; ++w) {
                  const int64_t abs_bit = bit_off + w;
                  const uint8_t byte = static_cast<uint8_t>(lvl[abs_bit / 8]);
                  v |= ((byte >> (abs_bit % 8)) & 1) << w;
                }
                if (OB_FAIL(def_levels.push_back(v))) {
                }
                ++out_i;
              }
              lp += nbytes;
            } else {
              const int64_t run = header >> 1;
              const int64_t nbytes = (width + 7) / 8;
              uint8_t v = 0;
              for (int w = 0; w < nbytes && lp < lvl_end; ++w) {
                v |= static_cast<uint8_t>(lvl[lp++]) << (8 * w);
              }
              for (int64_t g = 0; OB_SUCC(ret) && g < run && out_i < page_num_values; ++g) {
                if (OB_FAIL(def_levels.push_back(v))) {
                }
                ++out_i;
              }
            }
          }
          dp = lvl_end;
        }
      }
      if (OB_FAIL(ret)) {
      }
      // plain values
      const int32_t physical = state->cols_.at(c).physical_type_;
      for (int64_t i = 0; OB_SUCC(ret) && i < page_num_values; ++i) {
        bool is_null = false;
        if (1 == state->cols_.at(c).repetition_) {
          is_null = (i >= def_levels.count() || 0 == def_levels.at(i));
        }
        ObString text;
        if (is_null) {
          text = ObString();
        } else if (0 == physical) {  // boolean
          if (dp >= page_len) { ret = OB_INVALID_DATA; }
          else {
            const uint8_t byte = static_cast<uint8_t>(page[dp + i / 8]);
            const bool b = (0 != ((byte >> (i % 8)) & 1));
            text = ObString::make_string(b ? "1" : "0");
          }
        } else if (1 == physical || 2 == physical || 4 == physical || 5 == physical) {
          // The text outlives this loop, so it must be copied into the allocator;
          // a stack buffer would leave the stored value pointing at dead memory.
          const int sz = (1 == physical) ? 4 : ((2 == physical) ? 8 : ((4 == physical) ? 4 : 8));
          if (dp + sz > page_len) {
            ret = OB_INVALID_DATA;
          } else {
            char *buf = static_cast<char *>(alloc.alloc(32));
            if (OB_ISNULL(buf)) {
              ret = OB_ALLOCATE_MEMORY_FAILED;
            } else {
              int n = 0;
              if (1 == physical) {
                int32_t v = 0;
                MEMCPY(&v, page + dp, 4);
                n = ::snprintf(buf, 32, "%d", static_cast<int>(v));
              } else if (2 == physical) {
                int64_t v = 0;
                MEMCPY(&v, page + dp, 8);
                n = ::snprintf(buf, 32, "%ld", static_cast<long>(v));
              } else {
                double d = 0.0;
                if (4 == physical) { float f = 0.0f; MEMCPY(&f, page + dp, 4); d = f; }
                else { MEMCPY(&d, page + dp, 8); }
                n = ::snprintf(buf, 32, "%.17g", d);
              }
              dp += sz;
              text.assign_ptr(buf, static_cast<ObString::obstr_size_t>(n));
            }
          }
        } else {  // byte_array
          if (dp + 4 > page_len) { ret = OB_INVALID_DATA; }
          else {
            uint32_t n = 0;
            MEMCPY(&n, page + dp, 4);
            dp += 4;
            if (dp + n > page_len) { ret = OB_INVALID_DATA; }
            else {
              text.assign_ptr(page + dp, static_cast<ObString::obstr_size_t>(n));
              dp += n;
            }
          }
        }
        if (OB_SUCC(ret)) {
          const int64_t idx = (row_base + i) * state->col_count_ + c;
          if (idx >= state->cells_.count()) {
            ret = OB_INVALID_DATA;
          } else {
            state->cells_.at(idx) = text;
          }
        }
      }
      row_base += rows_per_group;
    }
  }
  if (OB_SUCC(ret)) {
    opened_ = true;
  }
  return ret;
}

int ObParquetReader::get_next_row(ObIAllocator &alloc, ObFileRow &row)
{
  int ret = OB_SUCCESS;
  row.reset();
  if (!opened_ || OB_ISNULL(state_)) {
    ret = OB_NOT_INIT;
  } else if (state_->row_idx_ >= state_->row_count_) {
    ret = OB_ITER_END;
  } else {
    for (int64_t c = 0; OB_SUCC(ret) && c < state_->col_count_; ++c) {
      ObFileRowValue value;
      const ObString &text = state_->cells_.at(state_->row_idx_ * state_->col_count_ + c);
      if (text.empty()) {
        value.is_null_ = true;
      } else {
        value.text_ = text;
      }
      if (OB_FAIL(row.push_back(value))) {
      }
    }
    if (OB_SUCC(ret)) {
      ++state_->row_idx_;
    }
  }
  UNUSED(alloc);
  return ret;
}

} // namespace sql
} // namespace oceanbase
