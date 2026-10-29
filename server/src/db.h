// TaskFlow Server - thin SQLite wrapper returning nlohmann::json rows
#pragma once
#include <stdexcept>
#include <string>

#include "json.hpp"

struct sqlite3;

namespace tf {

using json = nlohmann::json;

class Db {
 public:
  explicit Db(const std::string& path);
  ~Db();
  Db(const Db&) = delete;
  Db& operator=(const Db&) = delete;

  // Run a statement (or several, no params) without results.
  void exec_script(const std::string& sql);
  // Run a parameterised statement; returns number of changed rows.
  int exec(const std::string& sql, const json& params = json::array());
  // Run a parameterised INSERT; returns last insert rowid.
  int64_t insert(const std::string& sql, const json& params = json::array());
  // Query rows -> json array of objects keyed by column name.
  json query(const std::string& sql, const json& params = json::array());
  // First row or null.
  json one(const std::string& sql, const json& params = json::array());
  // Single scalar value of first column of first row (null if none).
  json scalar(const std::string& sql, const json& params = json::array());

  // Consistent online copy of the whole database (safe while the server is running).
  bool backup_to(const std::string& path, std::string& err);
  // PRAGMA quick_check / integrity_check result ("ok" when healthy).
  std::string check(bool full);

  void begin();
  void commit();
  void rollback();

 private:
  sqlite3* db_ = nullptr;
};

// RAII transaction: rolls back unless commit() called.
class Tx {
 public:
  explicit Tx(Db& db) : db_(db) { db_.begin(); }
  ~Tx() {
    if (!done_) {
      try { db_.rollback(); } catch (...) {}
    }
  }
  void commit() { db_.commit(); done_ = true; }

 private:
  Db& db_;
  bool done_ = false;
};

}  // namespace tf
