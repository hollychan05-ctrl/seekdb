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

} // namespace sql
} // namespace oceanbase
