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

#pragma once

#include "lib/allocator/ob_allocator.h"
#include "lib/container/ob_se_array.h"
#include "lib/string/ob_string.h"
#include "lib/string/ob_string_buffer.h"

namespace oceanbase
{
namespace sql
{

// The column type a file scan exposes. The set is deliberately the one the file
// table functions can infer from text: the value order is also the widening
// order used when two rows disagree about the type of a column.
enum class ObFileColumnType : int64_t
{
  INVALID = 0,
  NULLTYPE = 1,
  BOOLEAN = 2,
  BIGINT = 3,
  DOUBLE = 4,
  DATE = 5,
  DATETIME = 6,
  VARCHAR = 7
};

// The uppercase name of a file column type, as reported by "describe select".
const char *ob_file_column_type_name(const ObFileColumnType type);

// One column of a scanned file.
struct ObFileColumnDef
{
  ObFileColumnDef()
      : name_(), type_(ObFileColumnType::INVALID), nullable_(false)
  {}
  common::ObString name_;
  ObFileColumnType type_;
  // true when at least one row had no value for this column
  bool nullable_;
  TO_STRING_KV(K_(name), K_(type), K_(nullable));
};

// The result of looking at a whole file before it is scanned.
struct ObFileTableMeta
{
  ObFileTableMeta()
      : file_path_(), columns_(), row_count_(0), has_header_(true), is_glob_(false)
  {}
  common::ObString file_path_;
  common::ObSEArray<ObFileColumnDef, 16> columns_;
  // rows seen while inferring: used for the plan cost, never for the result
  int64_t row_count_;
  bool has_header_;
  bool is_glob_;
  TO_STRING_KV(K_(file_path), K_(row_count), K_(has_header), K_(is_glob), K(columns_.count()));
};

// One row of a file, as text. A null field keeps null_ = true and an empty text.
struct ObFileRowValue
{
  ObFileRowValue() : text_(), is_null_(false) {}
  common::ObString text_;
  bool is_null_;
};

typedef common::ObSEArray<ObFileRowValue, 16> ObFileRow;

// Which of the three optional named arguments a file table function was called
// with. sample_size is accepted only with the value -1, which means "look at the
// whole file"; any other value is reported as unsupported by the caller.
struct ObFileTableArgs
{
  ObFileTableArgs()
      : has_header_(false), header_(true), has_sample_size_(false), sample_size_(-1),
        has_format_(false), format_(), has_delimiter_(false), delimiter_(','),
        has_nullstr_(false), nullstr_()
  {}
  bool has_header_;
  bool header_;
  bool has_sample_size_;
  int64_t sample_size_;
  bool has_format_;
  common::ObString format_;
  bool has_delimiter_;
  char delimiter_;
  bool has_nullstr_;
  common::ObString nullstr_;
};

// Reading a local file as a table.
//
// Nothing here knows about plans or operators: the resolver calls infer_*() to
// learn the columns of the file, and the file scan operator calls scan_*() to
// iterate the rows, so the two can never disagree about the schema.
class ObFileScanUtils
{
public:
  // Guess the format of a path from its extension. Returns INVALID for a path
  // that no reader handles.
  static int detect_format(const common::ObString &path,
                           common::ObString &format); // "csv" / "json" / "parquet"

  // List the regular files matching a glob pattern. Each match is appended as a
  // full path; the result is sorted so the scan order is deterministic.
  static int glob_files(common::ObIAllocator &alloc,
                        const common::ObString &pattern,
                        common::ObIArray<common::ObString> &files);

  // Read a whole csv file and derive its column names and types.
  static int infer_csv_schema(common::ObIAllocator &alloc,
                              const common::ObString &path,
                              const ObFileTableArgs &args,
                              ObFileTableMeta &meta);

  // Read a whole newline delimited json file and derive the union of the object
  // fields. A field missing from some line makes the column nullable.
  static int infer_json_schema(common::ObIAllocator &alloc,
                               const common::ObString &path,
                               ObFileTableMeta &meta);

  // Turn a text field into a typed value, using the type the schema inference
  // decided for the column. Used by both readers.
  static int convert_field(common::ObIAllocator &alloc,
                           const ObFileColumnType type,
                           const common::ObString &text,
                           bool &is_null,
                           int64_t &int_value,
                           double &double_value,
                           common::ObString &str_value);

  // Type helpers used by both the inference and the conversion
  static ObFileColumnType widen(ObFileColumnType left, ObFileColumnType right);
  static ObFileColumnType type_of_text(const common::ObString &text);
  static int parse_date(const common::ObString &text, int32_t &days, bool &ok);
  static int parse_datetime(const common::ObString &text, int64_t &usec, bool &ok);
  static int parse_bool(const common::ObString &text, bool &value, bool &ok);
  static int parse_bigint(const common::ObString &text, int64_t &value, bool &ok);
  static int parse_double(const common::ObString &text, double &value, bool &ok);

  // Make "name" unique inside the column list by appending "__<n>", which is what
  // a duplicated csv header becomes (the second "id" is named "id__2").
  static int unique_column_name(common::ObIAllocator &alloc,
                                common::ObIArray<ObFileColumnDef> &columns,
                                common::ObString &name);
};

// A csv file opened for reading. Fields may contain the delimiter, quotes
// (escaped by doubling) and line breaks, so the reader works on the byte stream
// instead of on lines.
class ObFileCsvReader
{
public:
  ObFileCsvReader();
  ~ObFileCsvReader();

  int open(common::ObIAllocator &alloc,
           const common::ObString &path,
           const ObFileTableArgs &args,
           const common::ObSEArray<ObFileColumnDef, 16> &columns);

  // Append the next row to values_. Returns OB_ITER_END at the end of the file.
  // Every call reuses allocator_ memory, so a caller that needs the text to
  // outlive the call must copy it.
  int get_next_row(common::ObIAllocator &alloc, ObFileRow &row);

  void close();

private:
  int refill();
  int read_field(common::ObIAllocator &alloc, ObFileRowValue &value, bool &row_end);
  int read_line_into_buffer();

  int fd_;
  bool is_inited_;
  bool eof_;
  char delimiter_;
  common::ObString nullstr_;
  common::ObString buffer_;   // unparsed tail of the file
  int64_t pos_;               // read position inside buffer_
  char *io_buf_;              // fixed size read buffer
};

// A newline delimited json file opened for reading. Every line is parsed into an
// object and its fields are mapped onto the inferred columns by name.
class ObFileJsonReader
{
public:
  ObFileJsonReader();
  ~ObFileJsonReader();

  int open(common::ObIAllocator &alloc,
           const common::ObString &path,
           const common::ObSEArray<ObFileColumnDef, 16> &columns);
  int get_next_row(common::ObIAllocator &alloc, ObFileRow &row);
  void close();

private:
  int fd_;
  bool is_inited_;
  bool eof_;
  common::ObString buffer_;
  int64_t pos_;
  char *io_buf_;
};

} // namespace sql
} // namespace oceanbase
