// Inverted TaskFlow Server - core and accounts endpoints (health, company, sign-in, sign-up, password, me, notifications)
#include "api.h"

namespace tf::api {

// ------------------------------------------------------------------ routes
void Api::install() {
  // ================= public =================
  route("GET", "/health", false, [](const Req&, const json&, UserCtx*) {
    return json{{"status", "ok"}, {"version", TF_VERSION}, {"time", now_iso()}};
  }, true);  // never waits for the database lock: monitoring stays responsive
  route("GET", "/company", false, [this](const Req&, const json&, UserCtx*) {
    return json{{"name", jstr(app.db.one("SELECT value FROM meta WHERE key='company_name'"), "value")},
                {"tagline", jstr(app.db.one("SELECT value FROM meta WHERE key='company_tagline'"), "value")},
                {"server_version", TF_VERSION}, {"api_version", TF_API_VERSION},
                {"min_client_version", app.cfg.min_client_version}};
  });
  route("GET", "/departments/public", false, [this](const Req&, const json&, UserCtx*) {
    return json{{"departments", app.db.query("SELECT id, code, name, parent_id FROM departments WHERE active=1 ORDER BY sort, name")}};
  });

  route("POST", "/auth/register", false, [this](const Req& req, const json& b, UserCtx*) {
    require(allow_registration(client_ip(req)), 429, "Too many sign-up requests from this computer - try again later");
    std::string name = req_text(b, "name", "Name", 100);
    std::string email = trim(jstr(b, "email"));
    require(email.size() >= 5 && email.size() <= 120 && email.find('@') != std::string::npos &&
                email.find(' ') == std::string::npos && email.find('.', email.find('@')) != std::string::npos,
            400, "Enter a valid e-mail address");
    std::string pw = jstr(b, "password");
    std::string prob = App::password_problem(pw);
    require(prob.empty(), 400, prob);
    int64_t dept = jint(b, "dept_id");
    std::string salt; int it;
    std::string h = app.hash_password(pw, salt, it);  // slow on purpose - done before taking the lock
    std::lock_guard<std::recursive_mutex> lock(app.mu);
    if (dept) unit_row(dept);
    json existing = app.db.one("SELECT id,status FROM users WHERE email=?", {email});
    Tx tx(app.db);
    int64_t id;
    if (!existing.is_null()) {
      require(jstr(existing, "status") == "REJECTED", 409, "An account with this e-mail already exists");
      id = jint(existing, "id");
      app.db.exec("UPDATE users SET name=?,phone=?,designation=?,pw_salt=?,pw_hash=?,pw_iter=?,status='PENDING',"
                  "requested_dept_id=?,decision_note='',seq=? WHERE id=?",
                  {name, opt_text(b, "phone", 30, "Phone"), opt_text(b, "designation", 80, "Designation"), salt, h, it,
                   dept ? json(dept) : json(), app.next_seq(), id});
    } else {
      id = app.db.insert("INSERT INTO users(email,name,phone,designation,pw_salt,pw_hash,pw_iter,status,global_role,"
                         "requested_dept_id,created_at,seq) VALUES(?,?,?,?,?,?,?,'PENDING','STAFF',?,?,?)",
                         {email, name, opt_text(b, "phone", 30, "Phone"), opt_text(b, "designation", 80, "Designation"),
                          salt, h, it, dept ? json(dept) : json(), now_iso(), app.next_seq()});
    }
    for (auto& a : app.db.query("SELECT id FROM users WHERE global_role='ADMIN' AND status='ACTIVE'"))
      app.notify(jint(a, "id"), "ACCOUNT_PENDING", "New account waiting for approval", name + " <" + email + ">", 0, 0, true);
    app.audit(id, "register", "user", id, email);
    tx.commit();
    return json{{"ok", true}, {"message", "Account requested. You can sign in once the administrator approves it."}};
  }, true);

  route("POST", "/auth/login", false, [this](const Req& req, const json& b, UserCtx*) {
    const std::string ip = client_ip(req);
    require(!login_blocked(ip), 429, "Too many failed sign-in attempts from this computer - wait 10 minutes");
    std::string email = trim(jstr(b, "email"));
    std::string pw = jstr(b, "password");
    require(!email.empty() && email.size() <= 120 && pw.size() <= 128, 401, "Wrong e-mail or password");
    json u;
    {
      std::lock_guard<std::recursive_mutex> lock(app.mu);
      u = app.db.one("SELECT * FROM users WHERE email=?", {email});
    }
    if (u.is_null()) {
      app.verify_password(pw, json::object());
      login_failed(ip);
      throw ApiError(401, "Wrong e-mail or password");
    }
    require(jstr(u, "locked_until") < now_iso(), 423, "Too many failed attempts - this account is locked for 15 minutes");
    bool pw_ok = app.verify_password(pw, u);  // slow on purpose - outside the lock
    std::lock_guard<std::recursive_mutex> lock(app.mu);
    if (!pw_ok) {
      u = app.db.one("SELECT * FROM users WHERE id=?", {u["id"]});
      login_failed(ip);
      int64_t fails = jint(u, "failed_logins") + 1;
      std::string until = fails >= 5 ? iso_after_seconds(15 * 60) : "";
      app.db.exec("UPDATE users SET failed_logins=?, locked_until=? WHERE id=?", {fails >= 5 ? 0 : fails, until, u["id"]});
      if (fails >= 5) app.audit(jint(u, "id"), "account_locked", "user", jint(u, "id"), ip);
      throw ApiError(401, "Wrong e-mail or password");
    }
    std::string st = jstr(u, "status");
    require(st != "PENDING", 403, "Your account is waiting for administrator approval");
    require(st != "REJECTED", 403, "Your registration was not approved" +
                                       (jstr(u, "decision_note").empty() ? std::string() : ": " + jstr(u, "decision_note")));
    require(st == "ACTIVE", 403, "Your account is disabled - contact the administrator");
    bool remember = jint(b, "remember") != 0;
    std::string token = to_hex(secure_random(32));
    std::string exp = iso_after_seconds(remember ? (long long)app.cfg.remember_days * 86400 : (long long)app.cfg.session_hours * 3600);
    Tx tx(app.db);
    app.db.exec("UPDATE users SET failed_logins=0, locked_until='', last_login=? WHERE id=?", {now_iso(), u["id"]});
    app.db.insert("INSERT INTO sessions(token_hash,user_id,created_at,expires_at,last_seen,client,ip) VALUES(?,?,?,?,?,?,?)",
                  {sha256_hex(token), u["id"], now_iso(), exp, now_iso(), opt_text(b, "client", 120, "Client"), ip});
    app.db.exec("DELETE FROM sessions WHERE expires_at < ?", {now_iso()});
    app.audit(jint(u, "id"), "login", "user", jint(u, "id"), ip);
    tx.commit();
    UserCtx c = app.load_ctx(jint(u, "id"));
    return json{{"token", token}, {"expires_at", exp}, {"user", me_payload(c)}};
  }, true);

  // ================= session =================
  route("POST", "/auth/logout", true, [this](const Req& req, const json&, UserCtx*) {
    app.db.exec("DELETE FROM sessions WHERE token_hash=?", {sha256_hex(req.get_header_value("Authorization").substr(7))});
    return json{{"ok", true}};
  });
  route("POST", "/auth/password", true, [this](const Req& req, const json& b, UserCtx* u) {
    json row;
    {
      std::lock_guard<std::recursive_mutex> lock(app.mu);
      row = user_row(u->id);
    }
    // password hashing takes ~0.1 s each: done without holding the lock, so nobody else waits for it
    require(app.verify_password(jstr(b, "old_password"), row), 400, "Current password is incorrect");
    std::string np = jstr(b, "new_password");
    std::string prob = App::password_problem(np);
    require(prob.empty(), 400, prob);
    require(np != jstr(b, "old_password"), 400, "Choose a password different from the current one");
    std::string salt; int it;
    std::string h = app.hash_password(np, salt, it);
    std::lock_guard<std::recursive_mutex> lock(app.mu);
    Tx tx(app.db);
    app.db.exec("UPDATE users SET pw_salt=?,pw_hash=?,pw_iter=?,must_change_password=0 WHERE id=?", {salt, h, it, u->id});
    // sign out every other session of this user
    app.db.exec("DELETE FROM sessions WHERE user_id=? AND token_hash<>?",
                {u->id, sha256_hex(req.get_header_value("Authorization").substr(7))});
    app.audit(u->id, "password_change", "user", u->id);
    tx.commit();
    return json{{"ok", true}};  // other sessions were removed: their instant-update connections end on the next wake-up
  }, true);
  route("GET", "/me", true, [this](const Req&, const json&, UserCtx* u) { return me_payload(*u); });

  install_admin();
  install_projects();
  install_tasks();
  install_requests();

  route("POST", "/notifications/read", true, [this](const Req&, const json& b, UserCtx* u) {
    Tx tx(app.db);
    int64_t s = app.next_seq();
    if (jint(b, "all")) {
      app.db.exec("UPDATE notifications SET is_read=1, seq=? WHERE user_id=? AND is_read=0", {s, u->id});
    } else if (b.contains("ids") && b["ids"].is_array()) {
      int n = 0;
      for (auto& id : b["ids"]) {
        if (++n > 500) break;
        app.db.exec("UPDATE notifications SET is_read=1, seq=? WHERE id=? AND user_id=? AND is_read=0", {s, id, u->id});
      }
    }
    tx.commit();
    return json{{"ok", true}};
  });

  // instant updates: registered without the generic wrapper (it waits without holding the lock)
  srv.Get("/api/v1/sync", [this](const Req& req, httplib::Response& res) { sync_handler(req, res); });
}

}  // namespace tf::api

namespace tf {

void register_routes(httplib::Server& srv, App& app) {
  static api::Api* instance = nullptr;
  instance = new api::Api(srv, app);  // lives for the life of the process
  instance->install();
}

}  // namespace tf
