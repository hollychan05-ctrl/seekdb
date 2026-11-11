// MiniVectorDB entry point: JSON-Lines over stdin/stdout, as required by the
// 2026-competition-round2 judge.  Startup recovers /data and emits
// {"type":"ready"}; then every line is one request and produces exactly one
// response line.  Diagnostics go to stderr only.
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <string>

#include "json.hpp"
#include "mvd.hpp"

namespace {

void emitLine(const std::string& text) {
  std::fwrite(text.data(), 1, text.size(), stdout);
  std::fputc('\n', stdout);
  std::fflush(stdout);
}

std::string dataDirFromArgs(int argc, char** argv) {
  std::string dir = "/data";
  const char* env = std::getenv("VECTORDB_DATA_DIR");
  if (env != nullptr && *env != '\0') dir = env;
  for (int i = 1; i < argc; ++i) {
    if (std::strcmp(argv[i], "--data-dir") == 0 && i + 1 < argc) {
      dir = argv[i + 1];
      ++i;
    } else if (std::strncmp(argv[i], "--data-dir=", 11) == 0) {
      dir = argv[i] + 11;
    }
  }
  return dir;
}

}  // namespace

int main(int argc, char** argv) {
  std::signal(SIGPIPE, SIG_IGN);
  const std::string dir = dataDirFromArgs(argc, argv);

  mvd::Database db;
  std::string openErr = db.open(dir);
  if (!openErr.empty()) {
    std::fprintf(stderr, "recovery failed: %s\n", openErr.c_str());
    return 1;
  }
  emitLine("{\"type\":\"ready\"}");

  std::string line;
  while (std::getline(std::cin, line)) {
    if (line.empty()) continue;
    mvd::JValue request;
    try {
      request = mvd::parseJson(line);
    } catch (const std::exception& e) {
      std::fprintf(stderr, "bad request line: %s\n", e.what());
      emitLine("{\"ok\":false,\"error\":{\"code\":\"bad_request\",\"message\":\"invalid JSON request\"}}");
      continue;
    }
    if (!request.isObj()) {
      emitLine("{\"ok\":false,\"error\":{\"code\":\"bad_request\",\"message\":\"request must be an object\"}}");
      continue;
    }
    mvd::JValue id = mvd::JValue::makeInt(0);
    if (const mvd::JValue* pid = request.find("id")) id = *pid;

    std::string op = "execute";
    if (const mvd::JValue* pop = request.find("op"); pop != nullptr && pop->isStr()) op = pop->s;

    mvd::JValue response = mvd::JValue::makeObj();
    response.obj.emplace_back("id", id);

    if (op == "shutdown") {
      db.close();
      response.obj.emplace_back("ok", mvd::JValue::makeBool(true));
      emitLine(mvd::dump(response));
      std::fflush(stdout);
      return 0;
    }

    if (op != "execute") {
      response.obj.emplace_back("ok", mvd::JValue::makeBool(false));
      mvd::JValue err = mvd::JValue::makeObj();
      err.obj.emplace_back("code", mvd::JValue::makeStr("unsupported_op"));
      err.obj.emplace_back("message", mvd::JValue::makeStr("unsupported operation"));
      response.obj.emplace_back("error", std::move(err));
      emitLine(mvd::dump(response));
      continue;
    }

    std::string sql;
    if (const mvd::JValue* psql = request.find("sql"); psql != nullptr && psql->isStr()) sql = psql->s;
    std::string mode = "exact";
    if (const mvd::JValue* pm = request.find("search_mode"); pm != nullptr && pm->isStr()) mode = pm->s;

    mvd::ExecResult result;
    try {
      result = db.execute(sql, mode);
    } catch (const std::exception& e) {
      result.ok = false;
      result.error_code = "internal_error";
      result.error_message = e.what();
    } catch (...) {
      result.ok = false;
      result.error_code = "internal_error";
      result.error_message = "unknown error";
    }

    if (!result.ok) {
      response.obj.emplace_back("ok", mvd::JValue::makeBool(false));
      mvd::JValue err = mvd::JValue::makeObj();
      err.obj.emplace_back("code", mvd::JValue::makeStr(
                                         result.error_code.empty() ? "execution_error" : result.error_code));
      err.obj.emplace_back("message", mvd::JValue::makeStr(result.error_message));
      response.obj.emplace_back("error", std::move(err));
      emitLine(mvd::dump(response));
      continue;
    }

    response.obj.emplace_back("ok", mvd::JValue::makeBool(true));
    if (result.has_rows) {
      mvd::JValue cols = mvd::JValue::makeArr();
      for (const std::string& c : result.columns) cols.arr.push_back(mvd::JValue::makeStr(c));
      response.obj.emplace_back("columns", std::move(cols));
      mvd::JValue rows = mvd::JValue::makeArr();
      for (auto& row : result.rows) {
        mvd::JValue jr = mvd::JValue::makeArr();
        for (auto& cell : row) jr.arr.push_back(std::move(cell));
        rows.arr.push_back(std::move(jr));
      }
      response.obj.emplace_back("rows", std::move(rows));
    } else if (result.affected_rows_set) {
      response.obj.emplace_back("affected_rows", mvd::JValue::makeInt(result.affected_rows));
    }
    emitLine(mvd::dump(response));
  }

  db.close();
  return 0;
}
