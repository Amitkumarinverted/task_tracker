// Inverted TaskFlow Server - accounts: passwords, sessions, the signed-in user and the first administrator (accounts module)
#include "server.h"

#include <filesystem>
#include <fstream>
#include "crypto.h"

namespace fs = std::filesystem;

namespace tf {

// ============================================================== auth
std::string App::password_problem(const std::string& pw) {
  if (pw.size() < 8) return "Password must be at least 8 characters";
  if (pw.size() > 128) return "Password is too long";
  bool letter = false, digit = false;
  for (unsigned char c : pw) {
    if (isalpha(c)) letter = true;
    if (isdigit(c)) digit = true;
  }
  if (!letter || !digit) return "Password must contain both letters and numbers";
  return "";
}

std::string App::hash_password(const std::string& pw, std::string& salt_hex, int& iters) {
  iters = 120000;
  auto salt = secure_random(16);
  salt_hex = to_hex(salt);
  return to_hex(pbkdf2_sha256(pw, salt, (uint32_t)iters, 32));
}

bool App::verify_password(const std::string& pw, const json& u) {
  if (!jhas(u, "pw_hash")) {
    pbkdf2_sha256(pw, secure_random(16), 120000, 32);  // keep timing similar for unknown users
    return false;
  }
  auto calc = to_hex(pbkdf2_sha256(pw, from_hex(jstr(u, "pw_salt")), (uint32_t)jint(u, "pw_iter", 120000), 32));
  return constant_time_equal(calc, jstr(u, "pw_hash"));
}

UserCtx App::load_ctx(int64_t user_id) {
  json u = db.one("SELECT id,name,email,global_role,status,scope_ver,must_change_password FROM users WHERE id=?", {user_id});
  if (u.is_null()) throw ApiError(401, "Unknown user");
  UserCtx c;
  c.id = user_id;
  c.name = jstr(u, "name");
  c.email = jstr(u, "email");
  c.role = jstr(u, "global_role");
  c.status = jstr(u, "status");
  c.scope_ver = jint(u, "scope_ver", 1);
  c.must_change_password = jint(u, "must_change_password") != 0;
  for (auto& m : db.query("SELECT m.dept_id, m.role, d.is_management FROM memberships m JOIN departments d ON d.id=m.dept_id "
                          "WHERE m.user_id=? AND m.active=1 AND d.active=1", {user_id})) {
    c.units[m["dept_id"].get<int64_t>()] = m["role"].get<std::string>();
    if (jint(m, "is_management")) c.management = true;
  }
  if (c.role == "MANAGEMENT") c.management = true;
  return c;
}

bool App::load_user_by_token(const std::string& token, UserCtx& out) {
  if (token.size() != 64) return false;
  std::string h = sha256_hex(token);
  json s = db.one("SELECT user_id, expires_at, last_seen FROM sessions WHERE token_hash=?", {h});
  if (s.is_null()) return false;
  if (jstr(s, "expires_at") < now_iso()) {
    db.exec("DELETE FROM sessions WHERE token_hash=?", {h});
    return false;
  }
  out = load_ctx(jint(s, "user_id"));
  if (out.status != "ACTIVE") return false;
  if (jstr(s, "last_seen") < iso_after_seconds(-60)) db.exec("UPDATE sessions SET last_seen=? WHERE token_hash=?", {now_iso(), h});
  return true;
}

void App::bootstrap_admin() {
  if (jint(db.one("SELECT COUNT(*) AS n FROM users"), "n") > 0) return;
  std::string pw = cfg.admin_password;
  bool generated = false;
  if (pw.empty()) {
    static const char* alpha = "ABCDEFGHJKLMNPQRSTUVWXYZabcdefghijkmnopqrstuvwxyz23456789";
    auto rnd = secure_random(14);
    for (auto b : rnd) pw += alpha[b % 57];
    pw += "7";
    generated = true;
  }
  std::string salt;
  int it;
  std::string h = hash_password(pw, salt, it);
  int64_t mgmt = management_unit();
  int64_t id = db.insert(
      "INSERT INTO users(email,name,designation,pw_salt,pw_hash,pw_iter,must_change_password,status,global_role,created_at,seq) "
      "VALUES(?,?,?,?,?,?,?,?,?,?,?)",
      {cfg.admin_email, "System Administrator", "IT Administrator", salt, h, it, generated ? 1 : 0, "ACTIVE", "ADMIN",
       now_iso(), next_seq()});
  (void)mgmt;
  (void)id;
  log_line("  First administrator account: " + cfg.admin_email);
  if (generated) {
    std::string path = (fs::path(cfg.db_path).parent_path() / "initial_admin_password.txt").string();
    std::ofstream f(path, std::ios::trunc);
    f << "Inverted TaskFlow - first administrator\n" << "E-mail   : " << cfg.admin_email << "\n"
      << "Password : " << pw << "\n" << "You will be asked to change it at first sign-in. Delete this file afterwards.\n";
    f.close();
#ifndef _WIN32
    fs::permissions(path, fs::perms::owner_read | fs::perms::owner_write, fs::perm_options::replace);
#endif
    log_line("  Generated a random admin password - written to " + path);
  }
}

}  // namespace tf
