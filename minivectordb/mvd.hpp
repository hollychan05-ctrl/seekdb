// Shared declarations for MiniVectorDB: SQL AST, values, storage and engine.
#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "json.hpp"

namespace mvd {

// ------------------------------------------------------------------ SQL model

enum class VType { BIGINT, VARCHAR, VECTOR };

struct ColumnDef {
  std::string name;
  VType type = VType::BIGINT;
  int64_t len = 0;  // VARCHAR character cap, or VECTOR dimension
  bool primary = false;
};

struct Literal {
  enum Kind { INT, STR, VEC } kind = INT;
  int64_t i = 0;
  std::string s;
  std::vector<float> v;
};

struct WhereCond {
  std::string column;
  std::string op;  // = < <= > >=
  Literal value;
};

struct OrderItem {
  std::string column;
  bool desc = false;
};

struct SelectItem {
  enum Kind { STAR, COLUMN, L2DIST } kind = COLUMN;
  std::string column;
  Literal vec;  // L2DIST probe vector
  std::string alias;
};

struct Assign {
  std::string column;
  Literal value;
};

struct Stmt {
  enum Kind {
    CREATE_TABLE,
    CREATE_VECTOR_INDEX,
    DROP_TABLE,
    INSERT,
    SELECT,
    UPDATE,
    DELETE_ROW,
    BEGIN,
    COMMIT,
    ROLLBACK,
    NOOP,
    UNSUPPORTED
  } kind = UNSUPPORTED;

  std::string name;       // table / index name
  std::string table;      // target table for INSERT/SELECT/UPDATE/DELETE/index
  std::string column;     // index vector column / drop target
  bool if_exists = false;
  std::vector<ColumnDef> columns;
  std::vector<std::string> insert_columns;              // optional column list
  std::vector<std::vector<Literal>> insert_rows;
  std::vector<SelectItem> select_items;
  std::vector<WhereCond> where;
  std::vector<OrderItem> order_by;
  std::vector<Assign> assigns;
  bool has_limit = false;
  int64_t limit = 0;
  int64_t offset = 0;
};

// ---------------------------------------------------------------- SQL parsing

// Parses a single statement.  Returns false and sets err on a syntax error.
bool parseSql(const std::string& sql, Stmt& out, std::string& err);

// ------------------------------------------------------------------- storage

// 32 KiB keeps slot offsets inside the u16 slot directory while still holding
// a row with a long VARCHAR, so a wide row cannot make a statement fail.
constexpr uint32_t kPageSize = 32768;
constexpr uint16_t kSlotFree = 0xFFFF;

struct Rid {
  uint32_t page = 0;
  uint16_t slot = 0;
  bool operator==(const Rid& o) const { return page == o.page && slot == o.slot; }
};

struct Page {
  bool loaded = false;
  bool dirty = false;
  std::vector<uint8_t> bytes;
  uint16_t nslots() const {
    return static_cast<uint16_t>(bytes[0] | (bytes[1] << 8));
  }
  void setNslots(uint16_t v) {
    bytes[0] = static_cast<uint8_t>(v & 0xFF);
    bytes[1] = static_cast<uint8_t>(v >> 8);
  }
  uint16_t freeEnd() const { return static_cast<uint16_t>(bytes[4] | (bytes[5] << 8)); }
  void setFreeEnd(uint16_t v) {
    bytes[4] = static_cast<uint8_t>(v & 0xFF);
    bytes[5] = static_cast<uint8_t>(v >> 8);
  }
  void slotAt(uint16_t idx, uint16_t& off, uint16_t& len) const {
    size_t p = 8 + static_cast<size_t>(idx) * 4;
    off = static_cast<uint16_t>(bytes[p] | (bytes[p + 1] << 8));
    len = static_cast<uint16_t>(bytes[p + 2] | (bytes[p + 3] << 8));
  }
  void setSlot(uint16_t idx, uint16_t off, uint16_t len) {
    size_t p = 8 + static_cast<size_t>(idx) * 4;
    bytes[p] = static_cast<uint8_t>(off & 0xFF);
    bytes[p + 1] = static_cast<uint8_t>(off >> 8);
    bytes[p + 2] = static_cast<uint8_t>(len & 0xFF);
    bytes[p + 3] = static_cast<uint8_t>(len >> 8);
  }
  void init() {
    bytes.assign(kPageSize, 0);
    setNslots(0);
    setFreeEnd(kPageSize);
  }
};

struct Table {
  std::string name;
  uint32_t oid = 0;
  std::vector<ColumnDef> columns;
  int pk_index = -1;
  int vec_index = -1;
  bool has_vector_index = false;
  std::string vector_index_name;
  std::unordered_map<int64_t, Rid> pk;
  std::vector<uint32_t> page_free;  // per page: contiguous free bytes
  uint32_t npages = 0;
  std::string heap_path;
};

// --------------------------------------------------------------- engine API

struct ExecResult {
  bool ok = false;
  bool has_rows = false;
  std::vector<std::string> columns;
  std::vector<std::vector<JValue>> rows;
  int64_t affected_rows = 0;
  bool affected_rows_set = false;
  std::string error_code;
  std::string error_message;
};

class Database {
 public:
  Database();
  ~Database();
  Database(const Database&) = delete;
  Database& operator=(const Database&) = delete;

  // Opens <dir>, replays the log and prepares /data.  Returns a non-empty
  // message on failure.
  std::string open(const std::string& dir);
  void close();

  ExecResult execute(const std::string& sql, const std::string& search_mode);

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace mvd
