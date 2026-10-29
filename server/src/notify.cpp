// Inverted TaskFlow Server - notifications, e-mail outbox, task history and audit log (notifications + audit modules)
#include "server.h"

#include <string>

namespace tf {

void App::audit(int64_t actor, const std::string& action, const std::string& entity, int64_t id, const std::string& detail) {
  db.insert("INSERT INTO audit(at,actor_id,action,entity,entity_id,detail) VALUES(?,?,?,?,?,?)",
            {now_iso(), actor, action, entity, id, detail.substr(0, 2000)});
}

void App::notify(int64_t user_id, const std::string& type, const std::string& title, const std::string& body,
                 int64_t task_id, int64_t request_id, bool email) {
  if (user_id <= 0) return;
  json u = db.one("SELECT email, name, status FROM users WHERE id=?", {user_id});
  if (u.is_null() || jstr(u, "status") != "ACTIVE") return;
  db.insert("INSERT INTO notifications(user_id,type,title,body,task_id,request_id,created_at,seq) VALUES(?,?,?,?,?,?,?,?)",
            {user_id, type, title, body.substr(0, 500), task_id ? json(task_id) : json(),
             request_id ? json(request_id) : json(), now_iso(), next_seq()});
  if (email && cfg.email_enabled)
    db.insert("INSERT INTO email_outbox(to_email,subject,body,created_at) VALUES(?,?,?,?)",
              {u["email"], "[TaskFlow] " + title,
               "Dear " + jstr(u, "name") + ",\r\n\r\n" + title + "\r\n" + body +
                   "\r\n\r\nOpen Inverted TaskFlow to take action.\r\n\r\n-- Inverted TaskFlow (automatic message)",
               now_iso()});
}

void App::notify_all(const std::set<int64_t>& users, int64_t except, const std::string& type, const std::string& title,
                     const std::string& body, int64_t task_id, int64_t request_id, bool email) {
  for (auto u : users)
    if (u != except) notify(u, type, title, body, task_id, request_id, email);
}

void App::add_event(int64_t task_id, int64_t actor, const std::string& body) {
  db.insert("INSERT INTO comments(task_id,author_id,kind,body,created_at,seq) VALUES(?,?,?,?,?,?)",
            {task_id, actor, "EVENT", body.substr(0, 4000), now_iso(), next_seq()});
}

void App::bump_scope(int64_t user_id) {
  db.exec("UPDATE users SET scope_ver=scope_ver+1, seq=? WHERE id=?", {next_seq(), user_id});
}

void App::bump_scope_around(int64_t unit, bool leadership) {
  std::set<int64_t> lead_units = {unit}, member_units;
  for (auto a : ancestors(unit)) lead_units.insert(a);
  if (int64_t mg = management_unit()) lead_units.insert(mg);
  if (leadership) {
    std::vector<int64_t> frontier = {unit};
    member_units.insert(unit);
    for (int g = 0; !frontier.empty() && g < 8; g++) {
      std::vector<int64_t> next;
      for (auto f : frontier)
        for (auto& r : db.query("SELECT id FROM departments WHERE parent_id=?", {f}))
          if (member_units.insert(jint(r, "id")).second) next.push_back(jint(r, "id"));
      frontier = next;
    }
  }
  std::set<int64_t> users;
  for (auto x : lead_units)
    for (auto& r : db.query("SELECT user_id FROM memberships WHERE dept_id=? AND active=1 AND role IN ('HEAD','DEPUTY')", {x}))
      users.insert(jint(r, "user_id"));
  for (auto x : member_units)
    for (auto& r : db.query("SELECT user_id FROM memberships WHERE dept_id=? AND active=1", {x})) users.insert(jint(r, "user_id"));
  for (auto u : users) bump_scope(u);
}

std::string App::name_of(int64_t user_id) {
  json r = db.one("SELECT name FROM users WHERE id=?", {user_id});
  return r.is_null() ? std::string("someone") : jstr(r, "name");
}

}  // namespace tf
