// Inverted TaskFlow Server - administration endpoints (accounts, departments, memberships, audit, settings, backup)
#include "api.h"

namespace tf::api {

// ================================================================== administration
void Api::install_admin() {
  auto admin_only = [](UserCtx* u) { require(u->is_admin(), 403, "Administrator only"); };

  route("GET", "/admin/users", true, [this, admin_only](const Req&, const json&, UserCtx* u) {
    admin_only(u);
    return json{{"users", app.db.query("SELECT id,email,name,phone,designation,status,global_role,requested_dept_id,"
                                       "decision_note,created_at,last_login FROM users ORDER BY status, name")}};
  });

  route("POST", R"(/admin/users/(\d+)/approve)", true, [this, admin_only](const Req& r, const json& b, UserCtx* u) {
    admin_only(u);
    int64_t id = id_arg(r);
    json usr = user_row(id);
    require(jstr(usr, "status") == "PENDING" || jstr(usr, "status") == "REJECTED", 400, "This account is not waiting for approval");
    std::string role = upper(jstr(b, "global_role", "STAFF"));
    require(role == "STAFF" || role == "MANAGEMENT" || role == "ADMIN", 400, "Invalid role");
    int64_t dept = jint(b, "dept_id");
    std::string drole = upper(jstr(b, "dept_role", "MEMBER"));
    require(drole == "MEMBER" || drole == "HEAD" || drole == "DEPUTY", 400, "Invalid department role");
    require(role == "ADMIN" || dept, 400, "Choose the department this person works in");
    if (dept) unit_row(dept);
    Tx tx(app.db);
    app.db.exec("UPDATE users SET status='ACTIVE', global_role=?, decision_note='', seq=? WHERE id=?", {role, app.next_seq(), id});
    if (dept)
      app.db.exec("INSERT INTO memberships(user_id,dept_id,role,active,seq) VALUES(?,?,?,1,?) "
                  "ON CONFLICT(user_id,dept_id) DO UPDATE SET role=excluded.role, active=1, seq=excluded.seq",
                  {id, dept, drole, app.next_seq()});
    app.bump_scope(id);
    app.notify(id, "ACCOUNT_APPROVED", "Welcome to Inverted TaskFlow", "Your account has been approved - you can sign in now.", 0, 0, true);
    app.audit(u->id, "approve_user", "user", id, role + (dept ? " " + app.unit_label(dept) + " " + drole : ""));
    tx.commit();
    return json{{"ok", true}};
  });

  route("POST", R"(/admin/users/(\d+)/reject)", true, [this, admin_only](const Req& r, const json& b, UserCtx* u) {
    admin_only(u);
    int64_t id = id_arg(r);
    json usr = user_row(id);
    require(jstr(usr, "status") == "PENDING", 400, "This account is not waiting for approval");
    std::string note = req_text(b, "note", "Reason", 500);
    Tx tx(app.db);
    app.db.exec("UPDATE users SET status='REJECTED', decision_note=?, seq=? WHERE id=?", {note, app.next_seq(), id});
    if (app.cfg.email_enabled)
      app.db.insert("INSERT INTO email_outbox(to_email,subject,body,created_at) VALUES(?,?,?,?)",
                    {usr["email"], "[TaskFlow] Registration not approved", note, now_iso()});
    app.audit(u->id, "reject_user", "user", id, note);
    tx.commit();
    return json{{"ok", true}};
  });

  route("POST", R"(/admin/users/(\d+)/(disable|enable))", true, [this, admin_only](const Req& r, const json&, UserCtx* u) {
    admin_only(u);
    int64_t id = id_arg(r);
    bool disable = r.matches[2].str() == "disable";
    require(id != u->id, 400, "You cannot disable your own account");
    json usr = user_row(id);
    require(jstr(usr, "status") == (disable ? "ACTIVE" : "DISABLED"), 400, disable ? "Account is not active" : "Account is not disabled");
    Tx tx(app.db);
    app.db.exec("UPDATE users SET status=?, seq=? WHERE id=?", {disable ? "DISABLED" : "ACTIVE", app.next_seq(), id});
    if (disable) app.db.exec("DELETE FROM sessions WHERE user_id=?", {id});
    app.audit(u->id, disable ? "disable_user" : "enable_user", "user", id);
    tx.commit();
    return json{{"ok", true}};
  });

  route("POST", R"(/admin/users/(\d+)/role)", true, [this, admin_only](const Req& r, const json& b, UserCtx* u) {
    admin_only(u);
    int64_t id = id_arg(r);
    user_row(id);
    std::string role = upper(jstr(b, "global_role"));
    require(role == "STAFF" || role == "MANAGEMENT" || role == "ADMIN", 400, "Invalid role");
    require(!(id == u->id && role != "ADMIN"), 400, "You cannot remove your own administrator role");
    Tx tx(app.db);
    app.db.exec("UPDATE users SET global_role=?, seq=? WHERE id=?", {role, app.next_seq(), id});
    app.bump_scope(id);
    app.audit(u->id, "set_role", "user", id, role);
    tx.commit();
    return json{{"ok", true}};
  });

  route("PATCH", R"(/admin/users/(\d+))", true, [this, admin_only](const Req& r, const json& b, UserCtx* u) {
    admin_only(u);
    int64_t id = id_arg(r);
    json usr = user_row(id);
    std::string name = jhas(b, "name") ? req_text(b, "name", "Name", 100) : jstr(usr, "name");
    std::string desig = jhas(b, "designation") ? opt_text(b, "designation", 80, "Designation") : jstr(usr, "designation");
    std::string phone = jhas(b, "phone") ? opt_text(b, "phone", 30, "Phone") : jstr(usr, "phone");
    app.db.exec("UPDATE users SET name=?, designation=?, phone=?, seq=? WHERE id=?", {name, desig, phone, app.next_seq(), id});
    app.audit(u->id, "edit_user", "user", id, b.dump());
    return json{{"ok", true}};
  });

  route("POST", R"(/admin/users/(\d+)/reset-password)", true, [this, admin_only](const Req& r, const json& b, UserCtx* u) {
    admin_only(u);
    int64_t id = id_arg(r);
    {
      std::lock_guard<std::recursive_mutex> lock(app.mu);
      user_row(id);
    }
    std::string pw = jstr(b, "password");
    std::string prob = App::password_problem(pw);
    require(prob.empty(), 400, prob);
    std::string salt; int it;
    std::string h = app.hash_password(pw, salt, it);  // slow: outside the lock
    std::lock_guard<std::recursive_mutex> lock(app.mu);
    Tx tx(app.db);
    app.db.exec("UPDATE users SET pw_salt=?,pw_hash=?,pw_iter=?,failed_logins=0,locked_until='',must_change_password=? WHERE id=?",
                {salt, h, it, id == u->id ? 0 : 1, id});
    if (id != u->id) app.db.exec("DELETE FROM sessions WHERE user_id=?", {id});
    app.audit(u->id, "reset_password", "user", id);
    tx.commit();
    return json{{"ok", true}};
  }, true);

  route("POST", "/admin/departments", true, [this, admin_only](const Req&, const json& b, UserCtx* u) {
    admin_only(u);
    std::string code = upper(req_text(b, "code", "Code", 12));
    std::string name = req_text(b, "name", "Name", 80);
    for (char c : code) require(isalnum((unsigned char)c), 400, "Code may contain letters and digits only");
    require(app.db.one("SELECT id FROM departments WHERE code=?", {code}).is_null(), 409, "That code is already used");
    int64_t parent = jint(b, "parent_id");
    if (!parent)
      require(app.db.one("SELECT id FROM projects WHERE code=?", {code + "R"}).is_null(), 409,
              "That code is already used by the project " + code + "R - choose another code");
    if (parent) {
      json p = unit_row(parent);
      require(!jint(p, "parent_id"), 400, "Sub-departments can only be created under a main department");
    }
    Tx tx(app.db);
    int64_t id = app.db.insert("INSERT INTO departments(code,name,parent_id,is_management,sort,active,seq) VALUES(?,?,?,0,?,1,?)",
                               {code, name, parent ? json(parent) : json(), jint(b, "sort", 50), app.next_seq()});
    if (!parent)
      app.db.insert("INSERT INTO projects(code,name,description,owner_id,dept_id,status,is_routine,created_at,seq) "
                    "VALUES(?,?,?,?,?,'ACTIVE',1,?,?)",
                    {code + "R", name + " - Routine", "Day-to-day work of the department", u->id, id, now_iso(), app.next_seq()});
    app.audit(u->id, parent ? "create_subdepartment" : "create_department", "department", id, code);
    tx.commit();
    return json{{"ok", true}, {"id", id}};
  });

  route("PATCH", R"(/admin/departments/(\d+))", true, [this, admin_only](const Req& r, const json& b, UserCtx* u) {
    admin_only(u);
    int64_t id = id_arg(r);
    json d = app.db.one("SELECT * FROM departments WHERE id=?", {id});
    require(!d.is_null(), 404, "Department not found");
    std::string name = jhas(b, "name") ? req_text(b, "name", "Name", 80) : jstr(d, "name");
    int64_t act = jhas(b, "active") ? (jint(b, "active") ? 1 : 0) : jint(d, "active");
    if (!act && jint(d, "is_management")) throw ApiError(400, "The Management department cannot be deactivated");
    if (!act && jint(d, "active")) {
      int64_t subs = jint(app.db.one("SELECT COUNT(*) AS n FROM departments WHERE parent_id=? AND active=1", {id}), "n");
      require(subs == 0, 400, "This department still has " + std::to_string(subs) +
                                  " active sub-department(s) - deactivate or move them first");
      int64_t open = jint(app.db.one("SELECT COUNT(*) AS n FROM tasks WHERE dept_id=? AND status IN "
                                     "('TODO','IN_PROGRESS','ON_HOLD','IN_REVIEW')", {id}), "n");
      require(open == 0, 400, "This department still has " + std::to_string(open) + " open task(s) - reassign or close them first");
      int64_t reqs = jint(app.db.one("SELECT COUNT(*) AS n FROM work_requests WHERE target_dept_id=? AND status='PENDING'", {id}), "n");
      require(reqs == 0, 400, "This department still has " + std::to_string(reqs) + " request(s) waiting for a decision - decide them first");
    }
    Tx tx(app.db);
    app.db.exec("UPDATE departments SET name=?, active=?, sort=?, seq=? WHERE id=?",
                {name, act, jint(b, "sort", jint(d, "sort")), app.next_seq(), id});
    for (auto& m : app.db.query("SELECT user_id FROM memberships WHERE dept_id=?", {id})) app.bump_scope(jint(m, "user_id"));
    if (act != jint(d, "active")) app.bump_scope_around(id, true);  // parents and sub-units see differently now
    app.audit(u->id, "update_department", "department", id, b.dump());
    tx.commit();
    return json{{"ok", true}};
  });

  route("POST", "/admin/memberships", true, [this, admin_only](const Req&, const json& b, UserCtx* u) {
    admin_only(u);
    int64_t uid = jint(b, "user_id"), dept = jint(b, "dept_id");
    std::string role = upper(jstr(b, "role", "MEMBER"));
    require(role == "MEMBER" || role == "HEAD" || role == "DEPUTY", 400, "Invalid department role");
    require(jstr(user_row(uid), "status") == "ACTIVE", 400, "Only active accounts can be added to departments");
    unit_row(dept);
    std::string old_role = app.role_in(uid, dept);
    Tx tx(app.db);
    app.db.exec("INSERT INTO memberships(user_id,dept_id,role,active,seq) VALUES(?,?,?,1,?) "
                "ON CONFLICT(user_id,dept_id) DO UPDATE SET role=excluded.role, active=1, seq=excluded.seq",
                {uid, dept, role, app.next_seq()});
    app.bump_scope(uid);
    app.bump_scope_around(dept, role != "MEMBER" || old_role == "HEAD" || old_role == "DEPUTY");  // leadership changed?
    app.audit(u->id, "set_membership", "user", uid, app.unit_label(dept) + " " + role);
    tx.commit();
    return json{{"ok", true}};
  });

  route("DELETE", R"(/admin/memberships/(\d+))", true, [this, admin_only](const Req& r, const json&, UserCtx* u) {
    admin_only(u);
    json m = app.db.one("SELECT * FROM memberships WHERE id=?", {id_arg(r)});
    require(!m.is_null(), 404, "Membership not found");
    int64_t open = jint(app.db.one("SELECT COUNT(*) AS n FROM tasks WHERE assignee_id=? AND dept_id=? AND status IN "
                                   "('TODO','IN_PROGRESS','ON_HOLD','IN_REVIEW')", {m["user_id"], m["dept_id"]}), "n");
    require(open == 0, 400, app.name_of(jint(m, "user_id")) + " still has " + std::to_string(open) +
                                " open task(s) in this department - reassign them first");
    Tx tx(app.db);
    app.db.exec("UPDATE memberships SET active=0, seq=? WHERE id=?", {app.next_seq(), m["id"]});
    app.bump_scope(jint(m, "user_id"));
    app.bump_scope_around(jint(m, "dept_id"), jstr(m, "role") != "MEMBER");
    app.audit(u->id, "remove_membership", "user", jint(m, "user_id"), app.unit_label(jint(m, "dept_id")));
    tx.commit();
    return json{{"ok", true}};
  });

  route("GET", "/admin/audit", true, [this, admin_only](const Req& r, const json&, UserCtx* u) {
    admin_only(u);
    int64_t limit = 300;
    try { if (r.has_param("limit")) limit = std::stoll(r.get_param_value("limit")); } catch (...) {}
    if (limit < 1 || limit > 2000) limit = 300;
    return json{{"audit", app.db.query("SELECT a.*, COALESCE(u.name,'') AS actor_name FROM audit a "
                                       "LEFT JOIN users u ON u.id=a.actor_id ORDER BY a.id DESC LIMIT ?", {limit})}};
  });

  route("PATCH", "/admin/company", true, [this, admin_only](const Req&, const json& b, UserCtx* u) {
    admin_only(u);
    if (jhas(b, "name")) app.db.exec("UPDATE meta SET value=? WHERE key='company_name'", {req_text(b, "name", "Name", 100)});
    if (jhas(b, "tagline")) app.db.exec("UPDATE meta SET value=? WHERE key='company_tagline'", {opt_text(b, "tagline", 120, "Tagline")});
    app.audit(u->id, "update_company", "company", 0, b.dump());
    return json{{"ok", true}};
  });

  route("GET", "/admin/system", true, [this, admin_only](const Req&, const json&, UserCtx* u) {
    admin_only(u);
    json c = app.db.one("SELECT (SELECT COUNT(*) FROM users WHERE status='ACTIVE') AS users, "
                        "(SELECT COUNT(*) FROM tasks) AS tasks, (SELECT COUNT(*) FROM projects) AS projects, "
                        "(SELECT COUNT(*) FROM email_outbox WHERE sent_at='' AND attempts<6) AS mail_pending, "
                        "(SELECT COUNT(*) FROM email_outbox WHERE sent_at='' AND attempts>=6) AS mail_failed, "
                        "(SELECT COUNT(*) FROM sessions WHERE expires_at>?) AS sessions", {now_iso()});
    c["server_version"] = TF_VERSION;
    c["email_enabled"] = app.cfg.email_enabled;
    c["tls"] = !app.cfg.tls_cert.empty();
    c["live_connections"] = app.waiters.load();
    c["backups_this_run"] = app.backups_done.load();
    c["backup_dir"] = app.cfg.backup_dir;
    // desktop app versions in use (sessions seen in the last 7 days) - shows who still has to update
    json versions = json::object();
    for (auto& r : app.db.query("SELECT client FROM sessions WHERE expires_at>? AND last_seen>?", {now_iso(), iso_after_seconds(-7LL * 86400)})) {
      std::string cl = jstr(r, "client"), v = "other";
      if (cl.rfind("Desktop ", 0) == 0) {
        size_t sp = cl.find(' ', 8);
        v = cl.substr(8, sp == std::string::npos ? std::string::npos : sp - 8);
      }
      versions[v] = versions.value(v, 0) + 1;
    }
    c["client_versions"] = versions;
    c["min_client_version"] = app.cfg.min_client_version;
    return c;
  });

  route("POST", "/admin/backup", true, [this, admin_only](const Req&, const json&, UserCtx* u) {
    admin_only(u);
    std::string path, err;
    require(app.run_backup(&path, &err), 500, "Backup failed: " + err);
    app.audit(u->id, "backup", "system", 0, path);
    return json{{"ok", true}, {"path", path}};
  });
}

}  // namespace tf::api
