// TaskFlow Server - thin SQLite wrapper
#include "db.h"

#include "sqlite3.h"

namespace tf {
namespace {

struct Stmt {
  sqlite3_stmt* s = nullptr;
  ~Stmt() { if (s) sqlite3_finalize(s); }
};

void ensure(sqlite3* db, int rc, const char* what) {
  if (rc != SQLITE_OK && rc != SQLITE_ROW && rc != SQLITE_DONE)
    throw std::runtime_error(std::string(what) + ": " + sqlite3_errmsg(db));
}

void prepare(sqlite3* db, const std::string& sql, const json& params, Stmt& st) {
  ensure(db, sqlite3_prepare_v2(db, sql.c_str(), (int)sql.size(), &st.s, nullptr), "prepare");
  int i = 1;
  for (const auto& p : params) {
    int rc;
    if (p.is_null()) rc = sqlite3_bind_null(st.s, i);
    else if (p.is_boolean()) rc = sqlite3_bind_int64(st.s, i, p.get<bool>() ? 1 : 0);
    else if (p.is_number_integer()) rc = sqlite3_bind_int64(st.s, i, p.get<int64_t>());
    else if (p.is_number_float()) rc = sqlite3_bind_double(st.s, i, p.get<double>());
    else if (p.is_string()) {
      const std::string& v = p.get_ref<const std::string&>();
      rc = sqlite3_bind_text(st.s, i, v.c_str(), (int)v.size(), SQLITE_TRANSIENT);
    } else {
      std::string v = p.dump();
      rc = sqlite3_bind_text(st.s, i, v.c_str(), (int)v.size(), SQLITE_TRANSIENT);
    }
    ensure(db, rc, "bind");
    i++;
  }
}

}  // namespace

Db::Db(const std::string& path) {
  if (sqlite3_open(path.c_str(), &db_) != SQLITE_OK) {
    std::string msg = db_ ? sqlite3_errmsg(db_) : "out of memory";
    throw std::runtime_error("cannot open database '" + path + "': " + msg);
  }
  sqlite3_busy_timeout(db_, 5000);
  exec_script("PRAGMA journal_mode=WAL; PRAGMA synchronous=NORMAL; PRAGMA foreign_keys=ON;");
}

Db::~Db() {
  if (db_) sqlite3_close(db_);
}

void Db::exec_script(const std::string& sql) {
  char* err = nullptr;
  if (sqlite3_exec(db_, sql.c_str(), nullptr, nullptr, &err) != SQLITE_OK) {
    std::string msg = err ? err : "unknown";
    sqlite3_free(err);
    throw std::runtime_error("sql: " + msg);
  }
}

int Db::exec(const std::string& sql, const json& params) {
  Stmt st;
  prepare(db_, sql, params, st);
  int rc;
  while ((rc = sqlite3_step(st.s)) == SQLITE_ROW) {}
  ensure(db_, rc, "step");
  return sqlite3_changes(db_);
}

int64_t Db::insert(const std::string& sql, const json& params) {
  exec(sql, params);
  return sqlite3_last_insert_rowid(db_);
}

json Db::query(const std::string& sql, const json& params) {
  Stmt st;
  prepare(db_, sql, params, st);
  json rows = json::array();
  int rc;
  int ncol = sqlite3_column_count(st.s);
  while ((rc = sqlite3_step(st.s)) == SQLITE_ROW) {
    json row = json::object();
    for (int c = 0; c < ncol; c++) {
      const char* name = sqlite3_column_name(st.s, c);
      switch (sqlite3_column_type(st.s, c)) {
        case SQLITE_INTEGER: row[name] = (int64_t)sqlite3_column_int64(st.s, c); break;
        case SQLITE_FLOAT: row[name] = sqlite3_column_double(st.s, c); break;
        case SQLITE_NULL: row[name] = nullptr; break;
        default: {
          const unsigned char* t = sqlite3_column_text(st.s, c);
          row[name] = t ? std::string(reinterpret_cast<const char*>(t), sqlite3_column_bytes(st.s, c)) : "";
        }
      }
    }
    rows.push_back(std::move(row));
  }
  ensure(db_, rc, "step");
  return rows;
}

json Db::one(const std::string& sql, const json& params) {
  json r = query(sql, params);
  return r.empty() ? json() : r[0];
}

json Db::scalar(const std::string& sql, const json& params) {
  json r = one(sql, params);
  if (r.is_null() || r.empty()) return json();
  return r.begin().value();
}

bool Db::backup_to(const std::string& path, std::string& err) {
  sqlite3* dst = nullptr;
  if (sqlite3_open(path.c_str(), &dst) != SQLITE_OK) {
    err = dst ? sqlite3_errmsg(dst) : "cannot create backup file";
    if (dst) sqlite3_close(dst);
    return false;
  }
  sqlite3_backup* b = sqlite3_backup_init(dst, "main", db_, "main");
  if (!b) {
    err = sqlite3_errmsg(dst);
    sqlite3_close(dst);
    return false;
  }
  int rc = sqlite3_backup_step(b, -1);
  sqlite3_backup_finish(b);
  if (rc != SQLITE_DONE) {
    err = sqlite3_errstr(rc);
    sqlite3_close(dst);
    return false;
  }
  rc = sqlite3_errcode(dst);
  sqlite3_close(dst);
  if (rc != SQLITE_OK) {
    err = sqlite3_errstr(rc);
    return false;
  }
  return true;
}

std::string Db::check(bool full) {
  json r = query(full ? "PRAGMA integrity_check" : "PRAGMA quick_check");
  std::string out;
  for (auto& row : r) out += row.begin().value().get<std::string>() + (r.size() > 1 ? "; " : "");
  return out;
}

void Db::begin() { exec_script("BEGIN IMMEDIATE"); }
void Db::commit() { exec_script("COMMIT"); }
void Db::rollback() { exec_script("ROLLBACK"); }

}  // namespace tf
