// Storage and execution engine for MiniVectorDB.
//
// Layout under the data directory:
//   catalog.json  checkpoint of table definitions (written at clean shutdown
//                 and right after recovery)
//   t<oid>.dat    slotted-page heap file for one table
//   wal.log       redo log: page images plus a commit marker per transaction
//
// Durability model: a page modified by a transaction stays pinned in the page
// cache until that transaction commits, so an uncommitted page never reaches
// the heap file.  On commit the changed page images and a commit marker are
// appended to wal.log and fsynced; the heap write can then happen lazily.
// Recovery replays only committed page images, which makes it idempotent.
#include "mvd.hpp"

#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstring>
#include <list>
#include <map>
#include <set>
#include <tuple>
#include <unordered_set>

namespace mvd {
namespace {

// -------------------------------------------------------------------- helpers

uint32_t crcTable[256];
bool crcReady = false;

void initCrc() {
  if (crcReady) return;
  for (uint32_t i = 0; i < 256; ++i) {
    uint32_t c = i;
    for (int k = 0; k < 8; ++k) c = (c & 1) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
    crcTable[i] = c;
  }
  crcReady = true;
}

uint32_t crc32Of(const void* data, size_t n) {
  initCrc();
  const uint8_t* p = static_cast<const uint8_t*>(data);
  uint32_t c = 0xFFFFFFFFu;
  for (size_t i = 0; i < n; ++i) c = crcTable[(c ^ p[i]) & 0xFF] ^ (c >> 8);
  return c ^ 0xFFFFFFFFu;
}

void putU16(std::string& s, uint16_t v) {
  s.push_back(static_cast<char>(v & 0xFF));
  s.push_back(static_cast<char>(v >> 8));
}
void putU32(std::string& s, uint32_t v) {
  for (int i = 0; i < 4; ++i) s.push_back(static_cast<char>((v >> (8 * i)) & 0xFF));
}
void putU64(std::string& s, uint64_t v) {
  for (int i = 0; i < 8; ++i) s.push_back(static_cast<char>((v >> (8 * i)) & 0xFF));
}
uint16_t getU16(const uint8_t* p) { return static_cast<uint16_t>(p[0] | (p[1] << 8)); }
uint32_t getU32(const uint8_t* p) {
  return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
         (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24);
}
uint64_t getU64(const uint8_t* p) {
  uint64_t v = 0;
  for (int i = 0; i < 8; ++i) v |= static_cast<uint64_t>(p[i]) << (8 * i);
  return v;
}

bool writeFull(int fd, const void* buf, size_t n) {
  const uint8_t* p = static_cast<const uint8_t*>(buf);
  while (n > 0) {
    ssize_t w = ::write(fd, p, n);
    if (w <= 0) return false;
    p += w;
    n -= static_cast<size_t>(w);
  }
  return true;
}

bool pwriteFull(int fd, const void* buf, size_t n, off_t off) {
  const uint8_t* p = static_cast<const uint8_t*>(buf);
  while (n > 0) {
    ssize_t w = ::pwrite(fd, p, n, off);
    if (w <= 0) return false;
    p += w;
    n -= static_cast<size_t>(w);
    off += w;
  }
  return true;
}

bool readFullAt(int fd, void* buf, size_t n, off_t off) {
  uint8_t* p = static_cast<uint8_t*>(buf);
  while (n > 0) {
    ssize_t r = ::pread(fd, p, n, off);
    if (r <= 0) return false;
    p += r;
    n -= static_cast<size_t>(r);
    off += r;
  }
  return true;
}

int64_t fileSize(int fd) {
  struct stat st;
  if (::fstat(fd, &st) != 0) return -1;
  return static_cast<int64_t>(st.st_size);
}

void fsyncDir(const std::string& dir) {
  int fd = ::open(dir.c_str(), O_RDONLY);
  if (fd >= 0) {
    ::fsync(fd);
    ::close(fd);
  }
}

std::string upper(const std::string& s) {
  std::string o = s;
  for (char& c : o) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
  return o;
}

const char* typeName(VType t) {
  switch (t) {
    case VType::BIGINT: return "bigint";
    case VType::VARCHAR: return "varchar";
    case VType::VECTOR: return "vector";
  }
  return "bigint";
}
VType typeFromName(const std::string& n) {
  if (n == "varchar") return VType::VARCHAR;
  if (n == "vector") return VType::VECTOR;
  return VType::BIGINT;
}

// Unicode code-point count of a UTF-8 string (VARCHAR length is measured in
// characters, not bytes).
int64_t utf8Length(const std::string& s) {
  int64_t n = 0;
  for (size_t i = 0; i < s.size();) {
    unsigned char c = static_cast<unsigned char>(s[i]);
    size_t step = 1;
    if ((c & 0x80) == 0) step = 1;
    else if ((c & 0xE0) == 0xC0) step = 2;
    else if ((c & 0xF0) == 0xE0) step = 3;
    else if ((c & 0xF8) == 0xF0) step = 4;
    i += step;
    ++n;
  }
  return n;
}

struct Value {
  VType type = VType::BIGINT;
  int64_t i = 0;
  std::string s;
  std::vector<float> v;
};

std::string encodeRow(const Table& t, const std::vector<Value>& vals) {
  std::string out;
  for (size_t c = 0; c < t.columns.size(); ++c) {
    const Value& val = vals[c];
    switch (t.columns[c].type) {
      case VType::BIGINT: putU64(out, static_cast<uint64_t>(val.i)); break;
      case VType::VARCHAR: {
        putU32(out, static_cast<uint32_t>(val.s.size()));
        out += val.s;
        break;
      }
      case VType::VECTOR: {
        putU32(out, static_cast<uint32_t>(val.v.size()));
        for (float f : val.v) {
          uint32_t bits;
          std::memcpy(&bits, &f, 4);
          putU32(out, bits);
        }
        break;
      }
    }
  }
  return out;
}

bool decodeRow(const Table& t, const uint8_t* data, uint16_t len, std::vector<Value>& out) {
  out.clear();
  out.reserve(t.columns.size());
  size_t pos = 0;
  for (const ColumnDef& col : t.columns) {
    Value v;
    v.type = col.type;
    if (col.type == VType::BIGINT) {
      if (pos + 8 > len) return false;
      v.i = static_cast<int64_t>(getU64(data + pos));
      pos += 8;
    } else if (col.type == VType::VARCHAR) {
      if (pos + 4 > len) return false;
      uint32_t n = getU32(data + pos);
      pos += 4;
      if (pos + n > len) return false;
      v.s.assign(reinterpret_cast<const char*>(data + pos), n);
      pos += n;
    } else {
      if (pos + 4 > len) return false;
      uint32_t n = getU32(data + pos);
      pos += 4;
      if (pos + static_cast<size_t>(n) * 4 > len) return false;
      v.v.reserve(n);
      for (uint32_t k = 0; k < n; ++k) {
        uint32_t bits = getU32(data + pos);
        pos += 4;
        float f;
        std::memcpy(&f, &bits, 4);
        v.v.push_back(f);
      }
    }
    out.push_back(std::move(v));
  }
  return true;
}

// Parses "[1, 2, 3]" (whitespace tolerant).  Rejects NaN/Infinity and values
// that are not representable as a finite float32.
bool parseVectorText(const std::string& text, std::vector<float>& out, std::string& err) {
  out.clear();
  size_t i = 0;
  while (i < text.size() && std::isspace(static_cast<unsigned char>(text[i]))) ++i;
  if (i >= text.size() || text[i] != '[') {
    err = "vector literal must start with '['";
    return false;
  }
  ++i;
  for (;;) {
    while (i < text.size() && std::isspace(static_cast<unsigned char>(text[i]))) ++i;
    if (i < text.size() && text[i] == ']') {
      ++i;
      break;
    }
    if (i >= text.size()) {
      err = "unterminated vector literal";
      return false;
    }
    size_t start = i;
    while (i < text.size() && text[i] != ',' && text[i] != ']') ++i;
    std::string num = text.substr(start, i - start);
    size_t b = num.find_first_not_of(" \t\r\n");
    size_t e = num.find_last_not_of(" \t\r\n");
    if (b == std::string::npos) {
      err = "empty vector element";
      return false;
    }
    num = num.substr(b, e - b + 1);
    double d;
    try {
      size_t used = 0;
      d = std::stod(num, &used);
      if (used != num.size()) {
        err = "invalid vector element: " + num;
        return false;
      }
    } catch (...) {
      err = "invalid vector element: " + num;
      return false;
    }
    if (!std::isfinite(d) || std::fabs(d) > 3.4028234663852886e38) {
      err = "vector element is not a finite float32: " + num;
      return false;
    }
    out.push_back(static_cast<float>(d));
    while (i < text.size() && std::isspace(static_cast<unsigned char>(text[i]))) ++i;
    if (i < text.size() && text[i] == ',') {
      ++i;
      continue;
    }
    if (i < text.size() && text[i] == ']') {
      ++i;
      break;
    }
    err = "invalid vector literal";
    return false;
  }
  while (i < text.size()) {
    if (!std::isspace(static_cast<unsigned char>(text[i]))) {
      err = "trailing characters in vector literal";
      return false;
    }
    ++i;
  }
  return true;
}

struct SortKey {
  int kind = 0;  // 0 = int, 1 = double, 2 = string
  int64_t i = 0;
  double d = 0;
  std::string s;
};

int compareKeys(const SortKey& a, const SortKey& b) {
  if (a.kind != b.kind) {
    if (a.kind == 0 && b.kind == 1) return static_cast<double>(a.i) < b.d ? -1 : (static_cast<double>(a.i) > b.d ? 1 : 0);
    if (a.kind == 1 && b.kind == 0) return a.d < static_cast<double>(b.i) ? -1 : (a.d > static_cast<double>(b.i) ? 1 : 0);
    return a.kind < b.kind ? -1 : 1;
  }
  switch (a.kind) {
    case 0: return a.i < b.i ? -1 : (a.i > b.i ? 1 : 0);
    case 1: return a.d < b.d ? -1 : (a.d > b.d ? 1 : 0);
    default: return a.s.compare(b.s) < 0 ? -1 : (a.s == b.s ? 0 : 1);
  }
}

}  // namespace

// =============================================================== Impl

struct Database::Impl {
  std::string dir;
  std::map<std::string, std::unique_ptr<Table>> tables;
  std::map<uint32_t, Table*> by_oid;
  std::map<uint32_t, int> heap_fds;
  int wal_fd = -1;
  uint32_t next_oid = 1;
  uint32_t next_txn = 1;
  bool closed = false;

  // page cache
  struct CacheEntry {
    Page page;
    std::list<uint64_t>::iterator lru;
  };
  std::map<uint64_t, CacheEntry> cache;
  std::list<uint64_t> lru;
  size_t cache_bytes = 0;
  size_t cache_limit = 96u * 1024 * 1024;

  // transaction state
  bool txn_active = false;
  uint32_t txn_id = 0;
  std::set<uint64_t> txn_pages;
  // Pre-transaction image of every page this transaction modified.  Dirty
  // committed pages may not have reached the heap file yet, so rollback must
  // restore the in-memory content instead of re-reading the file.
  std::map<uint64_t, std::vector<uint8_t>> txn_before;
  std::vector<std::pair<uint32_t, uint32_t>> txn_new_pages;  // (oid, page)
  struct Undo {
    enum Kind { PK_SET, PK_ERASE, TABLE_ADD, TABLE_REMOVE, VINDEX_SET } kind;
    std::string table;
    int64_t key = 0;
    Rid rid;
    std::unique_ptr<Table> saved;  // for TABLE_REMOVE
    bool flag = false;
  };
  std::vector<Undo> undo;

  ~Impl() { closeHeapFds(); }

  void closeHeapFds() {
    for (auto& kv : heap_fds) {
      if (kv.second >= 0) ::close(kv.second);
    }
    heap_fds.clear();
    if (wal_fd >= 0) {
      ::close(wal_fd);
      wal_fd = -1;
    }
  }

  std::string path(const std::string& name) const { return dir + "/" + name; }

  int heapFd(const Table& t) {
    auto it = heap_fds.find(t.oid);
    if (it != heap_fds.end()) return it->second;
    int fd = ::open(t.heap_path.c_str(), O_RDWR | O_CREAT, 0644);
    heap_fds[t.oid] = fd;
    return fd;
  }

  static uint64_t pageKey(uint32_t oid, uint32_t page) {
    return (static_cast<uint64_t>(oid) << 32) | page;
  }

  void touchLru(uint64_t key) {
    auto it = cache.find(key);
    if (it == cache.end()) return;
    lru.erase(it->second.lru);
    lru.push_back(key);
    it->second.lru = std::prev(lru.end());
  }

  void evictIfNeeded() {
    while (cache_bytes > cache_limit && !lru.empty()) {
      uint64_t victim = 0xFFFFFFFFFFFFFFFFull;
      for (auto it = lru.begin(); it != lru.end(); ++it) {
        if (txn_pages.count(*it) == 0) {
          victim = *it;
          break;
        }
      }
      if (victim == 0xFFFFFFFFFFFFFFFFull) break;  // everything pinned
      auto it = cache.find(victim);
      if (it == cache.end()) {
        lru.erase(std::find(lru.begin(), lru.end(), victim));
        continue;
      }
      if (it->second.page.dirty) flushPage(it->first, it->second.page);
      cache_bytes -= it->second.page.bytes.size();
      lru.erase(it->second.lru);
      cache.erase(it);
    }
  }

  bool flushPage(uint64_t key, Page& page) {
    uint32_t oid = static_cast<uint32_t>(key >> 32);
    uint32_t pno = static_cast<uint32_t>(key & 0xFFFFFFFFu);
    auto tit = by_oid.find(oid);
    if (tit == by_oid.end()) return true;
    int fd = heapFd(*tit->second);
    if (fd < 0) return false;
    if (!pwriteFull(fd, page.bytes.data(), page.bytes.size(),
                    static_cast<off_t>(pno) * kPageSize)) {
      return false;
    }
    page.dirty = false;
    return true;
  }

  void flushAllDirty() {
    for (auto& kv : cache) {
      if (kv.second.page.dirty) flushPage(kv.first, kv.second.page);
    }
    for (auto& kv : heap_fds) {
      if (kv.second >= 0) ::fsync(kv.second);
    }
  }

  Page& loadPage(Table& t, uint32_t pno, bool create) {
    uint64_t key = pageKey(t.oid, pno);
    auto it = cache.find(key);
    if (it != cache.end()) {
      touchLru(key);
      return it->second.page;
    }
    CacheEntry entry;
    entry.page.bytes.assign(kPageSize, 0);
    entry.page.init();
    int fd = heapFd(t);
    if (fd >= 0) {
      int64_t size = fileSize(fd);
      off_t off = static_cast<off_t>(pno) * kPageSize;
      if (size > off) {
        size_t want = static_cast<size_t>(std::min<int64_t>(kPageSize, size - off));
        if (!readFullAt(fd, entry.page.bytes.data(), want, off)) {
          entry.page.init();
        }
      } else if (!create) {
        entry.page.init();
      }
    }
    entry.page.loaded = true;
    lru.push_back(key);
    entry.lru = std::prev(lru.end());
    cache_bytes += entry.page.bytes.size();
    auto res = cache.emplace(key, std::move(entry));
    evictIfNeeded();
    return res.first->second.page;
  }

  // Must be called *before* changing a page's bytes.
  void noteModified(uint64_t key, Page& page) {
    page.dirty = true;
    if (txn_active && txn_pages.insert(key).second) {
      txn_before[key] = page.bytes;
    }
  }

  // ------------------------------------------------------------ WAL

  bool walAppend(const std::string& payload) {
    std::string rec;
    putU32(rec, static_cast<uint32_t>(payload.size()));
    putU32(rec, crc32Of(payload.data(), payload.size()));
    rec += payload;
    return writeFull(wal_fd, rec.data(), rec.size());
  }

  bool walPage(uint32_t txn, uint32_t oid, uint32_t page, const std::vector<uint8_t>& image) {
    std::string p;
    p.push_back(1);
    putU32(p, txn);
    putU32(p, oid);
    putU32(p, page);
    putU32(p, static_cast<uint32_t>(image.size()));
    p.append(reinterpret_cast<const char*>(image.data()), image.size());
    return walAppend(p);
  }
  bool walCommit(uint32_t txn) {
    std::string p;
    p.push_back(2);
    putU32(p, txn);
    return walAppend(p);
  }
  bool walCreateTable(uint32_t txn, const Table& t) {
    std::string p;
    p.push_back(3);
    putU32(p, txn);
    putU32(p, t.oid);
    putU32(p, static_cast<uint32_t>(t.name.size()));
    p += t.name;
    putU32(p, static_cast<uint32_t>(t.columns.size()));
    for (const ColumnDef& c : t.columns) {
      putU32(p, static_cast<uint32_t>(c.name.size()));
      p += c.name;
      putU32(p, static_cast<uint32_t>(c.type));
      putU64(p, static_cast<uint64_t>(c.len));
      p.push_back(c.primary ? 1 : 0);
    }
    return walAppend(p);
  }
  bool walDropTable(uint32_t txn, const std::string& name) {
    std::string p;
    p.push_back(4);
    putU32(p, txn);
    putU32(p, static_cast<uint32_t>(name.size()));
    p += name;
    return walAppend(p);
  }

  // -------------------------------------------------- statement execution

  Table* findTable(const std::string& name) {
    for (auto& kv : tables) {
      if (kv.first == name) return kv.second.get();
    }
    return nullptr;
  }

  static void setError(ExecResult& r, const std::string& code, const std::string& msg) {
    r.ok = false;
    r.error_code = code;
    r.error_message = msg;
  }

  // Converts a literal to a column value, validating it.
  bool literalToValue(const ColumnDef& col, const Literal& lit, Value& out, std::string& err) {
    out.type = col.type;
    switch (col.type) {
      case VType::BIGINT: {
        if (lit.kind == Literal::INT) {
          out.i = lit.i;
          return true;
        }
        if (lit.kind == Literal::STR) {
          try {
            size_t used = 0;
            long long v = std::stoll(lit.s, &used);
            if (used == lit.s.size()) {
              out.i = v;
              return true;
            }
          } catch (...) {
          }
        }
        err = "cannot convert value to BIGINT";
        return false;
      }
      case VType::VARCHAR: {
        if (lit.kind == Literal::STR) {
          out.s = lit.s;
        } else if (lit.kind == Literal::INT) {
          out.s = std::to_string(lit.i);
        } else {
          err = "cannot convert vector value to VARCHAR";
          return false;
        }
        if (col.len > 0 && utf8Length(out.s) > col.len) {
          err = "value too long for VARCHAR(" + std::to_string(col.len) + ")";
          return false;
        }
        return true;
      }
      case VType::VECTOR: {
        std::vector<float> vec;
        if (lit.kind == Literal::VEC) {
          vec = lit.v;
        } else if (lit.kind == Literal::STR) {
          if (!parseVectorText(lit.s, vec, err)) return false;
        } else {
          err = "cannot convert integer to VECTOR";
          return false;
        }
        if (static_cast<int64_t>(vec.size()) != col.len) {
          err = "vector dimension mismatch: expected " + std::to_string(col.len) + ", got " +
                std::to_string(vec.size());
          return false;
        }
        out.v = std::move(vec);
        return true;
      }
    }
    err = "unsupported column type";
    return false;
  }

  // Iterates live rows until fn returns false.  fn must not touch the page
  // cache: the page reference stays valid only for the duration of the call.
  template <typename Fn>
  void scanUntil(Table& t, Fn&& fn) {
    for (uint32_t p = 0; p < t.npages; ++p) {
      Page& page = loadPage(t, p, false);
      uint16_t ns = page.nslots();
      for (uint16_t s = 0; s < ns; ++s) {
        uint16_t off = 0, len = 0;
        page.slotAt(s, off, len);
        if (len == kSlotFree || len == 0) continue;
        if (!fn(Rid{p, s}, page.bytes.data() + off, len)) return;
      }
    }
  }

  // Iterates every live row of a table.
  template <typename Fn>
  void scan(Table& t, Fn&& fn) {
    scanUntil(t, [&](Rid rid, const uint8_t* data, uint16_t len) {
      fn(rid, data, len);
      return true;
    });
  }

  bool insertRow(Table& t, const std::vector<Value>& vals, Rid& rid) {
    std::string rec = encodeRow(t, vals);
    if (rec.size() + 8 + 4 > kPageSize) return false;
    if (t.npages == 0) t.npages = 1;
    uint32_t pno = t.npages - 1;
    size_t need = rec.size();
    {
      Page& probe = loadPage(t, pno, true);
      size_t dirEnd = 8 + static_cast<size_t>(probe.nslots()) * 4;
      if (probe.freeEnd() < dirEnd + need + 4) {
        pno = t.npages;
        t.npages += 1;
      }
    }
    Page& page = loadPage(t, pno, true);
    uint16_t ns = page.nslots();
    size_t dirEnd = 8 + static_cast<size_t>(ns) * 4;
    if (page.freeEnd() < dirEnd + need + 4) {
      if (pno + 1 == t.npages) t.npages = pno;
      return false;
    }
    uint16_t off = static_cast<uint16_t>(page.freeEnd() - need);
    std::memcpy(page.bytes.data() + off, rec.data(), need);
    // reuse a deleted slot directory entry when possible
    uint16_t slotIdx = ns;
    bool reused = false;
    for (uint16_t s = 0; s < ns; ++s) {
      uint16_t so = 0, sl = 0;
      page.slotAt(s, so, sl);
      if (sl == kSlotFree) {
        slotIdx = s;
        reused = true;
        break;
      }
    }
    noteModified(pageKey(t.oid, pno), page);
    if (!reused) page.setNslots(static_cast<uint16_t>(ns + 1));
    page.setSlot(slotIdx, off, static_cast<uint16_t>(need));
    page.setFreeEnd(off);
    rid = Rid{pno, slotIdx};
    return true;
  }

  void markDeleted(Table& t, Rid rid) {
    Page& page = loadPage(t, rid.page, false);
    noteModified(pageKey(t.oid, rid.page), page);
    page.setSlot(rid.slot, 0, kSlotFree);
  }

  bool readRow(Table& t, Rid rid, std::vector<Value>& vals) {
    if (rid.page >= t.npages) return false;
    Page& page = loadPage(t, rid.page, false);
    if (rid.slot >= page.nslots()) return false;
    uint16_t off = 0, len = 0;
    page.slotAt(rid.slot, off, len);
    if (len == kSlotFree || len == 0) return false;
    return decodeRow(t, page.bytes.data() + off, len, vals);
  }

  bool rowMatches(Table& t, const std::vector<Value>& vals, const std::vector<WhereCond>& where) {
    for (const WhereCond& c : where) {
      int idx = -1;
      for (size_t k = 0; k < t.columns.size(); ++k) {
        if (t.columns[k].name == c.column) {
          idx = static_cast<int>(k);
          break;
        }
      }
      if (idx < 0) return false;
      const Value& v = vals[idx];
      if (v.type == VType::VARCHAR) {
        std::string rhs = c.value.kind == Literal::STR ? c.value.s
                            : (c.value.kind == Literal::INT ? std::to_string(c.value.i) : std::string());
        int cmp = v.s.compare(rhs);
        if (!(c.op == "=" ? cmp == 0
                          : c.op == "<" ? cmp < 0
                          : c.op == "<=" ? cmp <= 0
                          : c.op == ">" ? cmp > 0
                          : c.op == ">=" ? cmp >= 0
                          : c.op == "!=" ? cmp != 0
                                         : false)) {
          return false;
        }
      } else {
        int64_t lhs = v.i;
        int64_t rhs = c.value.kind == Literal::INT ? c.value.i : 0;
        if (c.value.kind == Literal::STR) {
          try {
            rhs = std::stoll(c.value.s);
          } catch (...) {
            return false;
          }
        }
        if (!(c.op == "=" ? lhs == rhs
                          : c.op == "<" ? lhs < rhs
                          : c.op == "<=" ? lhs <= rhs
                          : c.op == ">" ? lhs > rhs
                          : c.op == ">=" ? lhs >= rhs
                          : c.op == "!=" ? lhs != rhs
                                         : false)) {
          return false;
        }
      }
    }
    return true;
  }

  // ------------------------------------------------------- transaction

  void txnBegin() {
    if (!txn_active) {
      txn_active = true;
      txn_id = next_txn++;
      txn_pages.clear();
      txn_before.clear();
      txn_new_pages.clear();
      undo.clear();
    }
  }

  void txnCommit() {
    if (!txn_active) return;
    if (txn_pages.empty() && undo.empty()) {
      // read-only statement: nothing to make durable, skip the fsync
      txn_active = false;
      txn_before.clear();
      return;
    }
    // redo the pages touched by this transaction, then the commit marker
    for (uint64_t key : txn_pages) {
      auto it = cache.find(key);
      if (it == cache.end()) {
        // page was modified then dropped; it is already on disk (pre-txn), so
        // nothing to log.  This cannot normally happen because pages stay
        // pinned, but is harmless.
        continue;
      }
      walPage(txn_id, static_cast<uint32_t>(key >> 32), static_cast<uint32_t>(key & 0xFFFFFFFFu),
              it->second.page.bytes);
    }
    walCommit(txn_id);
    ::fsync(wal_fd);
    txn_active = false;
    txn_pages.clear();
    txn_before.clear();
    txn_new_pages.clear();
    undo.clear();
  }

  void txnRollback() {
    if (!txn_active) return;
    // undo auxiliary structures
    for (auto it = undo.rbegin(); it != undo.rend(); ++it) {
      Undo& u = *it;
      Table* t = findTable(u.table);
      switch (u.kind) {
        case Undo::PK_SET:
          if (t) t->pk[u.key] = u.rid;
          break;
        case Undo::PK_ERASE:
          if (t) t->pk.erase(u.key);
          break;
        case Undo::TABLE_ADD:
          if (t) dropFromCatalog(t);
          break;
        case Undo::TABLE_REMOVE:
          if (u.saved) restoreTable(std::move(u.saved));
          break;
        case Undo::VINDEX_SET:
          if (t) {
            t->has_vector_index = u.flag;
            t->vector_index_name.clear();
          }
          break;
      }
    }
    // restore the pre-transaction content of every page this transaction
    // touched; pages that did not exist before are simply discarded
    for (uint64_t key : txn_pages) {
      auto before = txn_before.find(key);
      auto it = cache.find(key);
      if (it == cache.end()) continue;
      if (before != txn_before.end()) {
        it->second.page.bytes = before->second;
        it->second.page.dirty = true;
      } else {
        cache_bytes -= it->second.page.bytes.size();
        lru.erase(it->second.lru);
        cache.erase(it);
      }
    }
    // free pages appended by this transaction
    for (auto& np : txn_new_pages) {
      Table* t = by_oid.count(np.first) ? by_oid[np.first] : nullptr;
      if (t != nullptr && t->npages > np.second) {
        t->npages = np.second;
      }
    }
    txn_active = false;
    txn_pages.clear();
    txn_before.clear();
    txn_new_pages.clear();
    undo.clear();
  }

  // Persists the table definitions.  Called at every checkpoint: both on a
  // clean shutdown and right after recovery, because recovery truncates the
  // log that carried the DDL records -- losing it here would drop tables if the
  // process is killed again before the next clean shutdown.
  void writeCatalog() {
    JValue root = JValue::makeObj();
    JValue tabs = JValue::makeArr();
    for (auto& kv : tables) {
      Table* t = kv.second.get();
      JValue tv = JValue::makeObj();
      tv.set("name", JValue::makeStr(t->name));
      tv.set("oid", JValue::makeInt(t->oid));
      JValue cols = JValue::makeArr();
      for (const ColumnDef& c : t->columns) {
        JValue cv = JValue::makeObj();
        cv.set("name", JValue::makeStr(c.name));
        cv.set("type", JValue::makeStr(typeName(c.type)));
        cv.set("len", JValue::makeInt(c.len));
        cv.set("pk", JValue::makeBool(c.primary));
        cols.arr.push_back(std::move(cv));
      }
      tv.set("columns", std::move(cols));
      tv.set("has_vector_index", JValue::makeBool(t->has_vector_index));
      tv.set("vector_index", JValue::makeStr(t->vector_index_name));
      tabs.arr.push_back(std::move(tv));
    }
    root.set("tables", std::move(tabs));
    root.set("next_oid", JValue::makeInt(next_oid));
    std::string text = dump(root);
    std::string tmp = path("catalog.tmp");
    FILE* f = std::fopen(tmp.c_str(), "wb");
    if (f == nullptr) return;
    std::fwrite(text.data(), 1, text.size(), f);
    std::fflush(f);
    ::fsync(fileno(f));
    std::fclose(f);
    std::rename(tmp.c_str(), path("catalog.json").c_str());
    fsyncDir(dir);
  }

  void dropFromCatalog(Table* t) {
    if (t == nullptr) return;
    by_oid.erase(t->oid);
    heap_fds.erase(t->oid);
    for (auto it = tables.begin(); it != tables.end(); ++it) {
      if (it->second.get() == t) {
        tables.erase(it);
        break;
      }
    }
  }

  void restoreTable(std::unique_ptr<Table> t) {
    Table* raw = t.get();
    by_oid[raw->oid] = raw;
    tables[raw->name] = std::move(t);
  }

  void recordPkSet(Table& t, int64_t key) {
    auto it = t.pk.find(key);
    Undo u;
    u.table = t.name;
    u.key = key;
    if (it == t.pk.end()) {
      u.kind = Undo::PK_ERASE;
    } else {
      u.kind = Undo::PK_SET;
      u.rid = it->second;
    }
    undo.push_back(std::move(u));
  }
};

// =============================================================== Database

Database::Database() : impl_(new Impl()) {}

Database::~Database() {
  if (impl_) close();
}

std::string Database::open(const std::string& directory) {
  Impl& d = *impl_;
  d.dir = directory;
  ::mkdir(d.dir.c_str(), 0755);

  // ---- load checkpoint catalog
  {
    std::string cat;
    FILE* f = std::fopen(d.path("catalog.json").c_str(), "rb");
    if (f) {
      char buf[65536];
      size_t n;
      while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0) cat.append(buf, n);
      std::fclose(f);
    }
    if (!cat.empty()) {
      try {
        JValue root = parseJson(cat);
        if (const JValue* tabs = root.find("tables")) {
          for (const JValue& tv : tabs->arr) {
            auto t = std::unique_ptr<Table>(new Table());
            const JValue* name = tv.find("name");
            const JValue* oid = tv.find("oid");
            t->name = name ? name->s : "";
            t->oid = oid ? static_cast<uint32_t>(oid->i) : 0;
            if (const JValue* cols = tv.find("columns")) {
              for (const JValue& cv : cols->arr) {
                ColumnDef c;
                const JValue* cn = cv.find("name");
                const JValue* ct = cv.find("type");
                const JValue* cl = cv.find("len");
                const JValue* cp = cv.find("pk");
                c.name = cn ? cn->s : "";
                c.type = typeFromName(ct ? ct->s : "bigint");
                c.len = cl ? cl->i : 0;
                c.primary = cp && cp->kind == JValue::BOOL && cp->b;
                t->columns.push_back(c);
              }
            }
            const JValue* hvi = tv.find("has_vector_index");
            t->has_vector_index = hvi && hvi->kind == JValue::BOOL && hvi->b;
            if (const JValue* vin = tv.find("vector_index")) t->vector_index_name = vin->s;
            for (size_t k = 0; k < t->columns.size(); ++k) {
              if (t->columns[k].primary) t->pk_index = static_cast<int>(k);
              if (t->columns[k].type == VType::VECTOR) t->vec_index = static_cast<int>(k);
            }
            t->heap_path = d.path("t" + std::to_string(t->oid) + ".dat");
            Table* raw = t.get();
            d.by_oid[raw->oid] = raw;
            d.tables[raw->name] = std::move(t);
          }
        }
        if (const JValue* no = root.find("next_oid")) d.next_oid = static_cast<uint32_t>(no->i);
      } catch (...) {
        // a corrupt checkpoint is fatal only if there is no log to rebuild from
      }
    }
  }

  // ---- open heaps, size them, rebuild primary-key maps
  for (auto& kv : d.tables) {
    Table* t = kv.second.get();
    int fd = d.heapFd(*t);
    int64_t size = fd >= 0 ? fileSize(fd) : 0;
    t->npages = static_cast<uint32_t>((static_cast<uint64_t>(size) + kPageSize - 1) / kPageSize);
    if (t->oid >= d.next_oid) d.next_oid = t->oid + 1;
  }

  // ---- replay the write-ahead log
  d.wal_fd = ::open(d.path("wal.log").c_str(), O_RDWR | O_CREAT, 0644);
  if (d.wal_fd < 0) return "cannot open wal.log";
  {
    int64_t size = fileSize(d.wal_fd);
    std::vector<uint8_t> data(static_cast<size_t>(std::max<int64_t>(size, 0)));
    if (!data.empty() && !readFullAt(d.wal_fd, data.data(), data.size(), 0)) {
      data.clear();
    }
    // pending page images per transaction, plus ordered DDL
    struct PendingTxn {
      std::vector<std::tuple<uint32_t, uint32_t, std::vector<uint8_t>>> pages;
      std::vector<std::unique_ptr<Table>> creates;
      std::vector<std::string> drops;
    };
    std::map<uint32_t, PendingTxn> pending;
    size_t pos = 0;
    bool clean = true;
    while (pos + 8 <= data.size() && clean) {
      uint32_t plen = getU32(data.data() + pos);
      uint32_t pcrc = getU32(data.data() + pos + 4);
      if (pos + 8 + plen > data.size()) break;  // torn tail
      const uint8_t* payload = data.data() + pos + 8;
      if (crc32Of(payload, plen) != pcrc) break;
      pos += 8 + plen;
      if (plen < 5) continue;
      uint8_t type = payload[0];
      uint32_t txn = getU32(payload + 1);
      if (type == 1) {
        if (plen < 17) continue;
        uint32_t oid = getU32(payload + 5);
        uint32_t pno = getU32(payload + 9);
        uint32_t ilen = getU32(payload + 13);
        if (plen < 17 + ilen) continue;
        std::vector<uint8_t> image(payload + 17, payload + 17 + ilen);
        pending[txn].pages.emplace_back(oid, pno, std::move(image));
      } else if (type == 2) {
        auto it = pending.find(txn);
        if (it != pending.end()) {
          for (auto& c : it->second.creates) {
            Table* raw = c.get();
            if (d.tables.count(raw->name) == 0) {
              raw->heap_path = d.path("t" + std::to_string(raw->oid) + ".dat");
              d.by_oid[raw->oid] = raw;
              if (raw->oid >= d.next_oid) d.next_oid = raw->oid + 1;
              d.tables[raw->name] = std::move(c);
            }
          }
          for (const std::string& name : it->second.drops) {
            Table* t = d.findTable(name);
            if (t) {
              int fd = d.heapFd(*t);
              if (fd >= 0) ::close(fd);
              d.heap_fds.erase(t->oid);
              std::string hp = t->heap_path;
              d.dropFromCatalog(t);
              ::unlink(hp.c_str());
            }
          }
          for (auto& pg : it->second.pages) {
            uint32_t oid = std::get<0>(pg);
            uint32_t pno = std::get<1>(pg);
            std::vector<uint8_t>& image = std::get<2>(pg);
            auto tit = d.by_oid.find(oid);
            if (tit == d.by_oid.end()) continue;
            Table* t = tit->second;
            int fd = d.heapFd(*t);
            if (fd < 0) continue;
            ::pwrite(fd, image.data(), image.size(), static_cast<off_t>(pno) * kPageSize);
            if (pno + 1 > t->npages) t->npages = pno + 1;
          }
          pending.erase(it);
        }
      } else if (type == 3) {
        if (plen < 13) continue;
        auto t = std::unique_ptr<Table>(new Table());
        size_t off = 9;
        t->oid = getU32(payload + 5);
        uint32_t nlen = getU32(payload + off);
        off += 4;
        if (off + nlen > plen) continue;
        t->name.assign(reinterpret_cast<const char*>(payload + off), nlen);
        off += nlen;
        uint32_t ncols = getU32(payload + off);
        off += 4;
        bool ok = true;
        for (uint32_t k = 0; k < ncols && ok; ++k) {
          ColumnDef c;
          uint32_t cn = getU32(payload + off);
          off += 4;
          if (off + cn + 13 > plen) {
            ok = false;
            break;
          }
          c.name.assign(reinterpret_cast<const char*>(payload + off), cn);
          off += cn;
          c.type = static_cast<VType>(getU32(payload + off));
          off += 4;
          c.len = static_cast<int64_t>(getU64(payload + off));
          off += 8;
          c.primary = payload[off++] != 0;
          t->columns.push_back(c);
        }
        if (!ok) continue;
        for (size_t k = 0; k < t->columns.size(); ++k) {
          if (t->columns[k].primary) t->pk_index = static_cast<int>(k);
          if (t->columns[k].type == VType::VECTOR) t->vec_index = static_cast<int>(k);
        }
        pending[txn].creates.push_back(std::move(t));
      } else if (type == 4) {
        uint32_t nlen = getU32(payload + 5);
        if (plen < 9 + nlen) continue;
        pending[txn].drops.emplace_back(reinterpret_cast<const char*>(payload + 9), nlen);
      } else if (type == 5) {
        // vector index: not reconstructible-critical, ignore on replay
      }
    }
    (void)clean;
  }

  // After replay the heaps already hold every committed change: checkpoint now.
  // The catalog must reach the disk BEFORE the log is truncated -- if we die in
  // between, the next start reloads the new catalog and simply replays the log
  // again (the images are idempotent), whereas truncating first would throw away
  // DDL records the catalog does not have yet.
  for (auto& kv : d.heap_fds) {
    if (kv.second >= 0) ::fsync(kv.second);
  }
  d.writeCatalog();
  if (::ftruncate(d.wal_fd, 0) != 0) {
  }
  ::fsync(d.wal_fd);
  ::lseek(d.wal_fd, 0, SEEK_SET);

  // rebuild primary key maps from the heaps
  for (auto& kv : d.tables) {
    Table* t = kv.second.get();
    t->pk.clear();
    if (t->pk_index < 0) continue;
    d.scan(*t, [&](Rid rid, const uint8_t* data, uint16_t len) {
      std::vector<Value> vals;
      if (decodeRow(*t, data, len, vals) && t->pk_index < static_cast<int>(vals.size())) {
        t->pk[vals[t->pk_index].i] = rid;
      }
    });
  }
  return std::string();
}

void Database::close() {
  if (!impl_) return;
  Impl& d = *impl_;
  if (d.closed) return;
  d.closed = true;
  if (d.txn_active) d.txnRollback();
  d.flushAllDirty();
  // checkpoint: persist table definitions, then drop the log
  d.writeCatalog();
  if (d.wal_fd >= 0) {
    ::ftruncate(d.wal_fd, 0);
    ::fsync(d.wal_fd);
  }
  d.closeHeapFds();
  d.cache.clear();
  d.lru.clear();
  d.cache_bytes = 0;
  d.tables.clear();
  d.by_oid.clear();
}

// --------------------------------------------------------------- execute

ExecResult Database::execute(const std::string& sql, const std::string& search_mode) {
  (void)search_mode;
  Impl& d = *impl_;
  ExecResult res;
  Stmt stmt;
  std::string err;
  if (!parseSql(sql, stmt, err)) {
    Impl::setError(res, "syntax_error", err.empty() ? "syntax error" : err);
    return res;
  }
  if (stmt.kind == Stmt::NOOP) {
    res.ok = true;
    return res;
  }
  if (stmt.kind == Stmt::UNSUPPORTED) {
    Impl::setError(res, "unsupported_sql", "statement is not supported");
    return res;
  }
  if (stmt.kind == Stmt::BEGIN) {
    d.txnBegin();
    res.ok = true;
    return res;
  }
  if (stmt.kind == Stmt::COMMIT) {
    d.txnCommit();
    res.ok = true;
    return res;
  }
  if (stmt.kind == Stmt::ROLLBACK) {
    d.txnRollback();
    res.ok = true;
    return res;
  }

  bool implicit = !d.txn_active;
  if (implicit) d.txnBegin();

  bool failed = false;

  if (stmt.kind == Stmt::CREATE_TABLE) {
    if (d.findTable(stmt.name) != nullptr) {
      Impl::setError(res, "table_exists", "table already exists: " + stmt.name);
      failed = true;
    } else if (stmt.columns.empty()) {
      Impl::setError(res, "syntax_error", "table needs at least one column");
      failed = true;
    } else {
      int pkCount = 0;
      int vecCount = 0;
      std::unordered_set<std::string> names;
      for (const ColumnDef& c : stmt.columns) {
        if (!names.insert(c.name).second) {
          Impl::setError(res, "duplicate_column", "duplicate column: " + c.name);
          failed = true;
          break;
        }
        if (c.primary) {
          ++pkCount;
          if (c.type != VType::BIGINT) {
            Impl::setError(res, "unsupported_sql", "PRIMARY KEY must be BIGINT");
            failed = true;
            break;
          }
        }
        if (c.type == VType::VECTOR) ++vecCount;
      }
      if (!failed && (pkCount != 1 || vecCount > 1)) {
        Impl::setError(res, "unsupported_sql",
                       "exactly one BIGINT primary key and at most one VECTOR column are required");
        failed = true;
      }
      if (!failed) {
        auto t = std::unique_ptr<Table>(new Table());
        t->name = stmt.name;
        t->oid = d.next_oid++;
        t->columns = stmt.columns;
        for (size_t k = 0; k < t->columns.size(); ++k) {
          if (t->columns[k].primary) t->pk_index = static_cast<int>(k);
          if (t->columns[k].type == VType::VECTOR) t->vec_index = static_cast<int>(k);
        }
        t->heap_path = d.path("t" + std::to_string(t->oid) + ".dat");
        Table* raw = t.get();
        d.by_oid[raw->oid] = raw;
        d.tables[raw->name] = std::move(t);
        Impl::Undo u;
        u.kind = Impl::Undo::TABLE_ADD;
        u.table = raw->name;
        d.undo.push_back(std::move(u));
        d.walCreateTable(d.txn_id, *raw);
      }
    }
  } else if (stmt.kind == Stmt::DROP_TABLE) {
    Table* t = d.findTable(stmt.name);
    if (t == nullptr) {
      if (stmt.if_exists) {
        res.ok = true;
      } else {
        Impl::setError(res, "no_such_table", "table does not exist: " + stmt.name);
        failed = true;
      }
    } else {
      Impl::Undo u;
      u.kind = Impl::Undo::TABLE_REMOVE;
      u.table = t->name;
      for (auto it = d.tables.begin(); it != d.tables.end(); ++it) {
        if (it->second.get() == t) {
          u.saved = std::move(it->second);
          d.tables.erase(it);
          break;
        }
      }
      d.by_oid.erase(t->oid);
      d.undo.push_back(std::move(u));
      d.walDropTable(d.txn_id, stmt.name);
    }
  } else if (stmt.kind == Stmt::CREATE_VECTOR_INDEX) {
    Table* t = d.findTable(stmt.table);
    if (t == nullptr) {
      Impl::setError(res, "no_such_table", "table does not exist: " + stmt.table);
      failed = true;
    } else {
      int ci = -1;
      for (size_t k = 0; k < t->columns.size(); ++k) {
        if (t->columns[k].name == stmt.column) ci = static_cast<int>(k);
      }
      if (ci < 0 || t->columns[ci].type != VType::VECTOR) {
        Impl::setError(res, "no_such_column", "vector column does not exist: " + stmt.column);
        failed = true;
      } else {
        Impl::Undo u;
        u.kind = Impl::Undo::VINDEX_SET;
        u.table = t->name;
        u.flag = t->has_vector_index;
        d.undo.push_back(std::move(u));
        t->has_vector_index = true;
        t->vector_index_name = stmt.name;
      }
    }
  } else if (stmt.kind == Stmt::DROP_INDEX) {
    Table* t = nullptr;
    if (!stmt.table.empty()) {
      t = d.findTable(stmt.table);
    } else {
      for (auto& kv : d.tables) {
        if (kv.second->has_vector_index && kv.second->vector_index_name == stmt.name) {
          t = kv.second.get();
          break;
        }
      }
      if (t == nullptr) t = d.findTable(stmt.name);
    }
    if (t == nullptr) {
      Impl::setError(res, "no_such_table", "no table owns index: " + stmt.name);
      failed = true;
    } else {
      Impl::Undo u;
      u.kind = Impl::Undo::VINDEX_SET;
      u.table = t->name;
      u.flag = t->has_vector_index;
      d.undo.push_back(std::move(u));
      t->has_vector_index = false;
      t->vector_index_name.clear();
    }
  } else if (stmt.kind == Stmt::INSERT) {
    Table* t = d.findTable(stmt.table);
    if (t == nullptr) {
      Impl::setError(res, "no_such_table", "table does not exist: " + stmt.table);
      failed = true;
    } else {
      // Resolve the column order once.
      std::vector<int> order;
      if (stmt.insert_columns.empty()) {
        for (size_t k = 0; k < t->columns.size(); ++k) order.push_back(static_cast<int>(k));
      } else {
        for (const std::string& cn : stmt.insert_columns) {
          int idx = -1;
          for (size_t k = 0; k < t->columns.size(); ++k) {
            if (t->columns[k].name == cn) idx = static_cast<int>(k);
          }
          if (idx < 0) {
            Impl::setError(res, "no_such_column", "unknown column: " + cn);
            failed = true;
            break;
          }
          order.push_back(idx);
        }
      }
      if (!failed && order.size() != t->columns.size()) {
        Impl::setError(res, "syntax_error", "column count does not match value count");
        failed = true;
      }
      std::vector<std::vector<Value>> rows;
      if (!failed) {
        std::unordered_set<int64_t> seen;
        for (const std::vector<Literal>& lrow : stmt.insert_rows) {
          if (lrow.size() != order.size()) {
            Impl::setError(res, "syntax_error", "column count does not match value count");
            failed = true;
            break;
          }
          std::vector<Value> vals(t->columns.size());
          for (size_t k = 0; k < lrow.size(); ++k) {
            std::string cerr;
            if (!d.literalToValue(t->columns[order[k]], lrow[k], vals[order[k]], cerr)) {
              Impl::setError(res, "invalid_value", cerr);
              failed = true;
              break;
            }
          }
          if (failed) break;
          if (t->pk_index >= 0) {
            int64_t pk = vals[t->pk_index].i;
            if (!seen.insert(pk).second || t->pk.count(pk) != 0) {
              Impl::setError(res, "duplicate_key", "duplicate primary key: " + std::to_string(pk));
              failed = true;
              break;
            }
          }
          rows.push_back(std::move(vals));
        }
      }
      if (!failed) {
        int64_t inserted = 0;
        for (auto& vals : rows) {
          Rid rid;
          uint32_t before = t->npages;
          if (!d.insertRow(*t, vals, rid)) {
            Impl::setError(res, "invalid_value", "row does not fit in a page");
            failed = true;
            break;
          }
          if (t->npages > before) {
            for (uint32_t p = before; p < t->npages; ++p) d.txn_new_pages.emplace_back(t->oid, p);
          }
          if (t->pk_index >= 0) {
            std::unordered_map<int64_t, Rid>::iterator it = t->pk.find(vals[t->pk_index].i);
            Impl::Undo u;
            u.kind = it == t->pk.end() ? Impl::Undo::PK_ERASE : Impl::Undo::PK_SET;
            u.table = t->name;
            u.key = vals[t->pk_index].i;
            if (it != t->pk.end()) u.rid = it->second;
            d.undo.push_back(std::move(u));
            t->pk[vals[t->pk_index].i] = rid;
          }
          ++inserted;
        }
        if (!failed) {
          res.affected_rows = inserted;
          res.affected_rows_set = true;
        }
      }
    }
  } else if (stmt.kind == Stmt::DELETE_ROW) {
    Table* t = d.findTable(stmt.table);
    if (t == nullptr) {
      Impl::setError(res, "no_such_table", "table does not exist: " + stmt.table);
      failed = true;
    } else {
      std::vector<std::pair<int64_t, Rid>> victims;
      d.scan(*t, [&](Rid rid, const uint8_t* data, uint16_t len) {
        std::vector<Value> vals;
        if (!decodeRow(*t, data, len, vals)) return;
        if (d.rowMatches(*t, vals, stmt.where)) {
          victims.emplace_back(t->pk_index >= 0 ? vals[t->pk_index].i : 0, rid);
        }
      });
      for (auto& v : victims) {
        d.markDeleted(*t, v.second);
        if (t->pk_index >= 0) {
          auto it = t->pk.find(v.first);
          if (it != t->pk.end()) {
            Impl::Undo u;
            u.kind = Impl::Undo::PK_SET;
            u.table = t->name;
            u.key = v.first;
            u.rid = it->second;
            d.undo.push_back(std::move(u));
            t->pk.erase(it);
          }
        }
      }
      res.affected_rows = static_cast<int64_t>(victims.size());
      res.affected_rows_set = true;
    }
  } else if (stmt.kind == Stmt::UPDATE) {
    Table* t = d.findTable(stmt.table);
    if (t == nullptr) {
      Impl::setError(res, "no_such_table", "table does not exist: " + stmt.table);
      failed = true;
    } else {
      std::vector<int> assignIdx;
      for (const Assign& a : stmt.assigns) {
        int idx = -1;
        for (size_t k = 0; k < t->columns.size(); ++k) {
          if (t->columns[k].name == a.column) idx = static_cast<int>(k);
        }
        if (idx < 0) {
          Impl::setError(res, "no_such_column", "unknown column: " + a.column);
          failed = true;
          break;
        }
        if (t->columns[idx].primary) {
          Impl::setError(res, "unsupported_sql", "primary key cannot be updated");
          failed = true;
          break;
        }
        assignIdx.push_back(idx);
      }
      std::vector<std::pair<Rid, std::vector<Value>>> jobs;
      if (!failed) {
        d.scan(*t, [&](Rid rid, const uint8_t* data, uint16_t len) {
          std::vector<Value> vals;
          if (!decodeRow(*t, data, len, vals)) return;
          if (!d.rowMatches(*t, vals, stmt.where)) return;
          std::vector<Value> updated = vals;
          for (size_t k = 0; k < stmt.assigns.size(); ++k) {
            std::string cerr;
            if (!d.literalToValue(t->columns[assignIdx[k]], stmt.assigns[k].value,
                                  updated[assignIdx[k]], cerr)) {
              Impl::setError(res, "invalid_value", cerr);
              failed = true;
              return;
            }
          }
          jobs.emplace_back(rid, std::move(updated));
        });
      }
      if (!failed) {
        int64_t count = 0;
        for (auto& job : jobs) {
          Rid newRid;
          uint32_t before = t->npages;
          if (!d.insertRow(*t, job.second, newRid)) {
            Impl::setError(res, "invalid_value", "row does not fit in a page");
            failed = true;
            break;
          }
          if (t->npages > before) {
            for (uint32_t p = before; p < t->npages; ++p) d.txn_new_pages.emplace_back(t->oid, p);
          }
          if (t->pk_index >= 0) {
            int64_t pk = job.second[t->pk_index].i;
            auto it = t->pk.find(pk);
            if (it != t->pk.end()) {
              Impl::Undo u;
              u.kind = Impl::Undo::PK_SET;
              u.table = t->name;
              u.key = pk;
              u.rid = it->second;
              d.undo.push_back(std::move(u));
            }
            t->pk[pk] = newRid;
          }
          d.markDeleted(*t, job.first);
          ++count;
        }
        if (!failed) {
          res.affected_rows = count;
          res.affected_rows_set = true;
        }
      }
    }
  } else if (stmt.kind == Stmt::SELECT) {
    Table* t = d.findTable(stmt.table);
    if (t == nullptr) {
      Impl::setError(res, "no_such_table", "table does not exist: " + stmt.table);
      failed = true;
    } else {
      // expand select items
      struct OutItem {
        int kind;  // 0 = column, 1 = l2
        int col;
        std::vector<float> probe;
        std::string name;
      };
      std::vector<OutItem> outItems;
      bool badColumn = false;
      for (const SelectItem& si : stmt.select_items) {
        if (si.kind == SelectItem::STAR) {
          for (size_t k = 0; k < t->columns.size(); ++k) {
            outItems.push_back(OutItem{0, static_cast<int>(k), {}, t->columns[k].name});
          }
        } else if (si.kind == SelectItem::COLUMN) {
          int idx = -1;
          for (size_t k = 0; k < t->columns.size(); ++k) {
            if (t->columns[k].name == si.column) idx = static_cast<int>(k);
          }
          if (idx < 0) {
            Impl::setError(res, "no_such_column", "unknown column: " + si.column);
            failed = true;
            badColumn = true;
            break;
          }
          outItems.push_back(OutItem{0, idx, {}, si.alias});
        } else {
          int idx = -1;
          for (size_t k = 0; k < t->columns.size(); ++k) {
            if (t->columns[k].name == si.column) idx = static_cast<int>(k);
          }
          if (idx < 0 || t->columns[idx].type != VType::VECTOR) {
            Impl::setError(res, "no_such_column", "unknown vector column: " + si.column);
            failed = true;
            badColumn = true;
            break;
          }
          std::vector<float> probe;
          std::string perr;
          if (si.vec.kind == Literal::VEC) {
            probe = si.vec.v;
          } else if (si.vec.kind == Literal::STR) {
            if (!parseVectorText(si.vec.s, probe, perr)) {
              Impl::setError(res, "invalid_vector", perr);
              failed = true;
              badColumn = true;
              break;
            }
          } else {
            Impl::setError(res, "invalid_vector", "invalid probe vector");
            failed = true;
            badColumn = true;
            break;
          }
          if (static_cast<int64_t>(probe.size()) != t->columns[idx].len) {
            Impl::setError(res, "invalid_vector", "probe vector dimension mismatch");
            failed = true;
            badColumn = true;
            break;
          }
          outItems.push_back(OutItem{1, idx, std::move(probe), si.alias});
        }
      }
      (void)badColumn;
      // Resolve ORDER BY: an output alias/column first, then any table column
      // (which does not have to appear in the select list).
      struct OrderRef {
        bool desc;
        int itemIndex;  // >= 0 when the key comes from a projected item
        int colIndex;   // >= 0 when the key comes straight from a table column
      };
      std::vector<OrderRef> orderRefs;
      if (!failed) {
        for (const OrderItem& oi : stmt.order_by) {
          int found = -1;
          for (size_t k = 0; k < outItems.size(); ++k) {
            if (outItems[k].name == oi.column) {
              found = static_cast<int>(k);
              break;
            }
          }
          if (found < 0) {
            for (size_t k = 0; k < outItems.size(); ++k) {
              if (outItems[k].kind == 0 && t->columns[outItems[k].col].name == oi.column) {
                found = static_cast<int>(k);
                break;
              }
            }
          }
          if (found >= 0) {
            orderRefs.push_back(OrderRef{oi.desc, found, -1});
            continue;
          }
          int ci = -1;
          for (size_t k = 0; k < t->columns.size(); ++k) {
            if (t->columns[k].name == oi.column) ci = static_cast<int>(k);
          }
          if (ci < 0) {
            Impl::setError(res, "no_such_column", "unknown ORDER BY column: " + oi.column);
            failed = true;
            break;
          }
          orderRefs.push_back(OrderRef{oi.desc, -1, ci});
        }
      }
      if (!failed) {
        struct RowOut {
          std::vector<JValue> cells;
          std::vector<SortKey> keys;
          std::vector<SortKey> orderKeys;
          int64_t pk = 0;
        };
        // A LIMIT must not force the whole table into memory: keep at most
        // offset+limit candidates, and stop the scan early when there is no
        // ORDER BY (which rows come back is unspecified then anyway).
        const bool has_order = !orderRefs.empty();
        const size_t cap = stmt.has_limit
                               ? static_cast<size_t>(std::max<int64_t>(0, stmt.offset)) +
                                     static_cast<size_t>(std::max<int64_t>(0, stmt.limit))
                               : 0;
        const bool bounded = stmt.has_limit;
        const bool early_stop = bounded && !has_order;
        std::vector<RowOut> out;
        if (bounded) out.reserve(std::min<size_t>(cap, 4096));

        auto compareOrder = [&](const RowOut& a, const RowOut& b) -> int {
          for (size_t i = 0; i < orderRefs.size(); ++i) {
            int c = compareKeys(a.orderKeys[i], b.orderKeys[i]);
            if (orderRefs[i].desc) c = -c;
            if (c != 0) return c;
          }
          if (a.pk != b.pk) return a.pk < b.pk ? -1 : 1;
          return 0;
        };

        d.scanUntil(*t, [&](Rid /*rid*/, const uint8_t* data, uint16_t len) {
          std::vector<Value> vals;
          if (!decodeRow(*t, data, len, vals)) return true;
          if (!d.rowMatches(*t, vals, stmt.where)) return true;
          RowOut ro;
          for (const OutItem& item : outItems) {
            JValue cell;
            SortKey key;
            if (item.kind == 1) {
              double sum = 0;
              const std::vector<float>& v = vals[item.col].v;
              size_t n = std::min(v.size(), item.probe.size());
              for (size_t k = 0; k < n; ++k) {
                double diff = static_cast<double>(v[k]) - static_cast<double>(item.probe[k]);
                sum += diff * diff;
              }
              cell = JValue::makeDouble(std::sqrt(sum));
              key.kind = 1;
              key.d = std::sqrt(sum);
            } else {
              const Value& v = vals[item.col];
              if (v.type == VType::BIGINT) {
                cell = JValue::makeInt(v.i);
                key.kind = 0;
                key.i = v.i;
              } else if (v.type == VType::VARCHAR) {
                cell = JValue::makeStr(v.s);
                key.kind = 2;
                key.s = v.s;
              } else {
                JValue arr = JValue::makeArr();
                for (float f : v.v) arr.arr.push_back(JValue::makeDouble(static_cast<double>(f)));
                cell = std::move(arr);
                key.kind = 2;
              }
            }
            ro.cells.push_back(std::move(cell));
            ro.keys.push_back(std::move(key));
          }
          for (const OrderRef& ref : orderRefs) {
            if (ref.itemIndex >= 0) {
              ro.orderKeys.push_back(ro.keys[ref.itemIndex]);
            } else {
              const Value& v = vals[ref.colIndex];
              SortKey k;
              if (v.type == VType::BIGINT) {
                k.kind = 0;
                k.i = v.i;
              } else if (v.type == VType::VARCHAR) {
                k.kind = 2;
                k.s = v.s;
              } else {
                k.kind = 2;
              }
              ro.orderKeys.push_back(std::move(k));
            }
          }
          if (t->pk_index >= 0) ro.pk = vals[t->pk_index].i;
          out.push_back(std::move(ro));
          if (bounded && out.size() > cap) {
            if (has_order) {
              size_t worst = 0;  // the candidate that sorts last
              for (size_t i = 1; i < out.size(); ++i) {
                if (compareOrder(out[worst], out[i]) < 0) worst = i;
              }
              out.erase(out.begin() + worst);
            } else {
              out.pop_back();
            }
          }
          return !(early_stop && out.size() >= cap);
        });
        if (has_order && out.size() > 1) {
          std::stable_sort(out.begin(), out.end(),
                           [&](const RowOut& a, const RowOut& b) { return compareOrder(a, b) < 0; });
        }
        if (stmt.has_limit) {
          size_t begin = std::min<size_t>(
              static_cast<size_t>(std::max<int64_t>(0, stmt.offset)), out.size());
          size_t end = std::min<size_t>(
              begin + static_cast<size_t>(std::max<int64_t>(0, stmt.limit)), out.size());
          std::vector<RowOut> sliced(out.begin() + begin, out.begin() + end);
          out = std::move(sliced);
        }
        for (const OutItem& item : outItems) res.columns.push_back(item.name);
        for (RowOut& ro : out) res.rows.push_back(std::move(ro.cells));
        res.has_rows = true;
      }
    }
  }

  res.ok = !failed;

  if (!failed) {
    if (implicit) d.txnCommit();
  } else {
    if (implicit) {
      d.txnRollback();
    }
  }
  return res;
}

}  // namespace mvd
