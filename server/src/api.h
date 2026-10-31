// Inverted TaskFlow Server - HTTP API: router, request checks, rate limits and the Api class.
// The endpoints are grouped by the architecture's server modules:
//   api_core.cpp      health, company, sign-in / sign-up, password, me, notifications (accounts, core)
//   api_admin.cpp     accounts approval, departments and memberships, audit, settings, backup (org, audit)
//   api_projects.cpp  projects
//   api_tasks.cpp     tasks, sub-tasks, life cycle, comments, extensions (tasks, collaboration, extensions)
//   api_requests.cpp  proposals, help requests, cross-team requests (work_requests)
//   api_sync.cpp      instant updates: GET /sync (sync)
#pragma once
#include <algorithm>
#include <chrono>
#include <functional>
#include <map>
#include "crypto.h"
#include "httplib.h"
#include "server.h"

namespace tf::api {


using Req = httplib::Request;
using Handler = std::function<json(const Req&, const json& body, UserCtx* u)>;
using Clock = std::chrono::steady_clock;

inline const std::set<std::string> kPriorities = {"LOW", "MEDIUM", "HIGH", "CRITICAL"};
inline const std::set<std::string> kOpen = {"TODO", "IN_PROGRESS", "ON_HOLD"};

inline std::string trim(const std::string& s) {
  size_t a = s.find_first_not_of(" \t\r\n");
  if (a == std::string::npos) return "";
  size_t b = s.find_last_not_of(" \t\r\n");
  return s.substr(a, b - a + 1);
}
inline std::string upper(std::string s) {
  for (auto& c : s) c = (char)toupper((unsigned char)c);
  return s;
}
inline void require(bool cond, int status, const std::string& msg) {
  if (!cond) throw ApiError(status, msg);
}
// strips control characters (except newlines/tabs) from user text
inline std::string clean(const std::string& s) {
  std::string o;
  o.reserve(s.size());
  for (unsigned char c : s)
    if (c >= 32 || c == '\n' || c == '\t') o += (char)c;
  return o;
}
inline std::string req_text(const json& b, const char* k, const char* label, size_t max_len = 200) {
  std::string v = clean(trim(jstr(b, k)));
  require(!v.empty(), 400, std::string(label) + " is required");
  require(v.size() <= max_len, 400, std::string(label) + " is too long (max " + std::to_string(max_len) + " characters)");
  return v;
}
inline std::string opt_text(const json& b, const char* k, size_t max_len, const char* label) {
  std::string v = clean(trim(jstr(b, k)));
  require(v.size() <= max_len, 400, std::string(label) + " is too long (max " + std::to_string(max_len) + " characters)");
  return v;
}
inline std::string opt_date(const json& b, const char* k, const char* label) {
  std::string v = trim(jstr(b, k));
  if (v.empty()) return v;
  require(valid_date(v), 400, std::string(label) + " must be a valid date");
  return v;
}
inline std::string req_date(const json& b, const char* k, const char* label) {
  std::string v = opt_date(b, k, label);
  require(!v.empty(), 400, std::string(label) + " is required");
  return v;
}
inline std::string priority_of(const json& b, const std::string& def = "MEDIUM") {
  std::string p = upper(trim(jstr(b, "priority", def)));
  if (p.empty()) p = def;
  require(kPriorities.count(p) > 0, 400, "Priority must be Low, Medium, High or Critical");
  return p;
}
inline std::string valid_link(const json& b, const char* k) {
  std::string v = trim(jstr(b, k));
  if (v.empty()) return v;
  require(v.size() <= 600, 400, "Link is too long");
  require(v.rfind("https://", 0) == 0 || v.rfind("http://", 0) == 0, 400,
          "Report link must start with http:// or https://");
  for (unsigned char c : v) require(c > 32 && c != '"' && c != '<' && c != '>' && c != '\\', 400, "Report link contains invalid characters");
  return v;
}

// The caller's address. Behind a local TLS reverse proxy (Caddy/IIS/nginx on the same machine) every request comes
// from 127.0.0.1, so the address the proxy reports in X-Forwarded-For is used instead.
inline std::string client_ip(const Req& req) {
  const std::string& a = req.remote_addr;
  if ((a == "127.0.0.1" || a == "::1" || a == "::ffff:127.0.0.1") && req.has_header("X-Forwarded-For")) {
    std::string x = req.get_header_value("X-Forwarded-For");
    auto c = x.rfind(',');
    std::string ip = trim(c == std::string::npos ? x : x.substr(c + 1));
    if (!ip.empty() && ip.size() < 64) return ip;
  }
  return a;
}

struct Bucket {
  double tokens = 1e9;
  Clock::time_point last = Clock::now();
  int login_fails = 0;
  Clock::time_point fail_window = Clock::now();
  int registrations = 0;
  Clock::time_point reg_window = Clock::now();
};

class Api {
 public:
  Api(httplib::Server& s, App& a) : srv(s), app(a) {}
  httplib::Server& srv;
  App& app;
  std::mutex rl_mu;
  std::map<std::string, Bucket> buckets;
  std::mutex waits_mu;
  std::map<std::string, int> waits_by_session;  // held instant-update requests per session (max 2)
  std::map<int64_t, int> waits_by_user;          // ... and per person over all their sessions (max 4)

  // ---- per-IP limits: 30 requests/s sustained (burst 60), 20 failed logins per 10 minutes
  bool allow_request(const std::string& ip) {
    std::lock_guard<std::mutex> l(rl_mu);
    if (buckets.size() > 20000) buckets.clear();
    Bucket& b = buckets[ip];
    auto now = Clock::now();
    double dt = std::chrono::duration<double>(now - b.last).count();
    b.last = now;
    const double rate = app.cfg.rate_limit, burst = 2.0 * rate;
    b.tokens = std::min(burst, b.tokens + dt * rate);
    if (b.tokens < 1.0) return false;
    b.tokens -= 1.0;
    return true;
  }
  bool login_blocked(const std::string& ip) {
    std::lock_guard<std::mutex> l(rl_mu);
    Bucket& b = buckets[ip];
    if (Clock::now() - b.fail_window > std::chrono::minutes(10)) { b.login_fails = 0; b.fail_window = Clock::now(); }
    return b.login_fails >= 20;
  }
  void login_failed(const std::string& ip) {
    std::lock_guard<std::mutex> l(rl_mu);
    buckets[ip].login_fails++;
  }
  bool allow_registration(const std::string& ip) {  // 5 sign-up requests per hour per computer
    std::lock_guard<std::mutex> l(rl_mu);
    Bucket& b = buckets[ip];
    if (Clock::now() - b.reg_window > std::chrono::hours(1)) { b.registrations = 0; b.reg_window = Clock::now(); }
    return ++b.registrations <= 5;
  }

  static void send(httplib::Response& res, int status, const json& out) {
    res.status = status;
    res.set_content(out.dump(), "application/json; charset=utf-8");
  }

  // Handlers normally run with the database lock held. `self_locking` handlers (public sign-in / sign-up) take the
  // lock themselves so that slow password hashing never blocks other users.
  void route(const std::string& method, const std::string& pattern, bool auth, Handler h, bool self_locking = false) {
    auto wrapped = [this, auth, h, pattern, self_locking, method](const Req& req, httplib::Response& res) {
      if (!allow_request(client_ip(req))) return send(res, 429, {{"error", "Too many requests - slow down"}, {"code", "RATE_LIMIT"}});
      json out;
      int status = 200;
      bool notify = false;
      try {
        std::unique_lock<std::recursive_mutex> lock(app.mu, std::defer_lock);
        if (!self_locking) lock.lock();
        int64_t seq_before = self_locking ? 0 : app.current_seq();
        json body = json::object();
        if (!req.body.empty()) {
          // the API never needs deep JSON: refuse nesting beyond 8 levels before anything walks the document
          json::parser_callback_t depth_guard = [](int depth, json::parse_event_t, json&) {
            if (depth > 8) throw std::runtime_error("too deep");
            return true;
          };
          try { body = json::parse(req.body, depth_guard); } catch (...) { throw ApiError(400, "Request body is not valid JSON"); }
          if (!body.is_object()) throw ApiError(400, "Request body must be a JSON object");
        }
        UserCtx u;
        UserCtx* up = nullptr;
        if (auth) {
          std::unique_lock<std::recursive_mutex> auth_lock(app.mu, std::defer_lock);
          if (self_locking) auth_lock.lock();  // self-locking handlers still read the session under the lock
          std::string a = req.get_header_value("Authorization");
          std::string token = a.rfind("Bearer ", 0) == 0 ? a.substr(7) : "";
          if (!app.load_user_by_token(token, u)) throw ApiError(401, "Session expired - please sign in again", "SESSION_EXPIRED");
          if (u.must_change_password && pattern != "/auth/password" && pattern != "/auth/logout" && pattern != "/me")
            throw ApiError(403, "Please change your password first", "PASSWORD_CHANGE_REQUIRED");
          up = &u;
        }
        out = h(req, body, up);
        if (out.is_null()) out = json{{"ok", true}};
        notify = self_locking ? method != "GET" : app.current_seq() != seq_before;  // a health check never wakes anyone
      } catch (const ApiError& e) {
        status = e.status;
        out = json{{"error", e.what()}};
        if (!e.code.empty()) out["code"] = e.code;
      } catch (const json::exception& e) {
        status = 400;
        out = json{{"error", std::string("Invalid field: ") + e.what()}};
      } catch (const std::exception& e) {
        status = 500;
        out = json{{"error", "Server error - the administrator can see details in the server log"}};
        log_line("  ! " + req.method + " " + req.path + " -> " + e.what());
      }
      if (notify) app.changed();  // wake instant-update connections after the commit
      send(res, status, out);
    };
    const std::string full = "/api/v1" + pattern;
    if (method == "GET") srv.Get(full, wrapped);
    else if (method == "POST") srv.Post(full, wrapped);
    else if (method == "PATCH") srv.Patch(full, wrapped);
    else if (method == "DELETE") srv.Delete(full, wrapped);
  }

  static int64_t id_arg(const Req& r, int i = 1) { return std::stoll(r.matches[i].str()); }

  // ------------------------------------------------------------------ shared helpers
  json me_payload(const UserCtx& u) {
    json uj = app.db.one("SELECT id,name,email,phone,designation,global_role,status,scope_ver,must_change_password "
                         "FROM users WHERE id=?", {u.id});
    uj["units"] = app.db.query(
        "SELECT m.dept_id, m.role, d.code, d.name, d.parent_id, d.is_management FROM memberships m "
        "JOIN departments d ON d.id=m.dept_id WHERE m.user_id=? AND m.active=1 AND d.active=1 ORDER BY d.name", {u.id});
    uj["management"] = u.management;
    return uj;
  }
  json user_row(int64_t id) {
    json r = app.db.one("SELECT * FROM users WHERE id=?", {id});
    require(!r.is_null(), 404, "User not found");
    return r;
  }
  json project_row(int64_t id) {
    json p = app.db.one("SELECT * FROM projects WHERE id=?", {id});
    require(!p.is_null(), 404, "Project not found");
    return p;
  }
  json unit_row(int64_t id) {
    json d = app.db.one("SELECT * FROM departments WHERE id=? AND active=1", {id});
    require(!d.is_null(), 404, "Department not found");
    return d;
  }
  bool is_active_user(int64_t id) {
    json r = app.db.one("SELECT status FROM users WHERE id=?", {id});
    return !r.is_null() && jstr(r, "status") == "ACTIVE";
  }
  std::vector<int64_t> units_of(int64_t user) {
    std::vector<int64_t> v;
    // deepest units first (sub-departments before departments)
    for (auto& r : app.db.query("SELECT m.dept_id FROM memberships m JOIN departments d ON d.id=m.dept_id "
                                "WHERE m.user_id=? AND m.active=1 AND d.active=1 ORDER BY (d.parent_id IS NULL), d.id", {user}))
      v.push_back(r["dept_id"].get<int64_t>());
    return v;
  }
  // Unit in which `assignee` will do work handed out by `actor`.
  int64_t pick_unit(const UserCtx& actor, int64_t assignee, int64_t requested) {
    if (requested) {
      require(!app.role_in(assignee, requested).empty(), 400, app.name_of(assignee) + " is not a member of " + app.unit_label(requested));
      return requested;
    }
    auto us = units_of(assignee);
    require(!us.empty(), 400, app.name_of(assignee) + " is not in any department yet");
    if (assignee != actor.id)
      for (auto u : us)
        if (app.has_authority(actor, assignee, u)) return u;
    return us.front();
  }
  void check_project_open(const json& p) {
    require(jstr(p, "status") != "CLOSED", 400, "Project " + jstr(p, "code") + " is closed");
  }
  bool is_self_subtask(const json& t) {
    if (!jint(t, "parent_id")) return false;
    json p = app.db.one("SELECT assignee_id FROM tasks WHERE id=?", {t["parent_id"]});
    return !p.is_null() && jint(p, "assignee_id") == jint(t, "assignee_id");
  }
  int open_subtasks(int64_t task) {
    return (int)jint(app.db.one("SELECT COUNT(*) AS n FROM tasks WHERE parent_id=? AND status IN "
                                "('TODO','IN_PROGRESS','ON_HOLD','IN_REVIEW')", {task}), "n");
  }
  std::string label(const json& t) { return jstr(t, "key") + "  " + jstr(t, "title"); }
  // the department a unit belongs to (the unit itself when it is a department)
  int64_t top_unit(int64_t unit) {
    int64_t top = unit;
    for (auto a : app.ancestors(unit)) top = a;
    return top;
  }
  std::set<int64_t> request_approvers(const json& q) {
    std::string type = jstr(q, "type");
    if (type == "CROSS_TEAM") {
      auto l = app.leaders(jint(q, "target_dept_id"));
      if (l.empty())
        for (auto a : app.ancestors(jint(q, "target_dept_id"))) {
          l = app.leaders(a);
          if (!l.empty()) break;
        }
      return l;
    }
    return app.managers_of(jint(q, "requested_by"), jint(q, "target_dept_id"));
  }
  bool can_decide_request(const UserCtx& u, const json& q) {
    if (jstr(q, "type") == "CROSS_TEAM") {
      int64_t t = jint(q, "target_dept_id");
      if (u.leads(t)) return true;
      for (auto a : app.ancestors(t)) if (u.leads(a)) return true;
      return false;
    }
    return app.has_authority(u, jint(q, "requested_by"), jint(q, "target_dept_id"));
  }

  // ------------------------------------------------------------------ routes
  void install();

  // ================================================================== administration
  void install_admin();

  // ================================================================== projects
  void install_projects();

  // ================================================================== tasks
  void install_tasks();

  json create_task(const json& b, const UserCtx& u);

  json task_action(const json& t, const std::string& act, const json& b, const UserCtx& u);

  // follow-ups once a task is DONE: tell the requester, refresh the parent, nudge the parent owner
  // a finished or cancelled task leaves nothing to decide: pending extensions and help requests on it are closed
  void close_pending_items(int64_t task_id, const std::string& why, const UserCtx& u);

  void after_done(const json& t, const UserCtx& u);

  // ---- v1.1: people helping on a task, dependencies, milestones, repeating tasks, mentions
  std::string recur_of(const json& b, const std::string& def);
  std::vector<int64_t> id_list(const json& b, const char* key, size_t max, const std::string& what);
  // replaces the list of people who also work on the task (the actor needs authority over each new one)
  void set_helpers(const json& t, std::vector<int64_t> want, const UserCtx& u);
  // replaces what the task waits for (same project, visible to the actor, no circles)
  void set_deps(const json& t, const std::vector<int64_t>& want, const UserCtx& u);
  bool waits_for(int64_t from, int64_t target);  // does `from` (directly or indirectly) wait for `target`?
  void repeat_task(const json& t, const UserCtx& u);           // a repeating task creates its next occurrence when done
  void tell_waiting_tasks(const json& done, const UserCtx& u);
  // dependency rows as this person may see them: the far end of a task they cannot open becomes 0 ("hidden"),
  // with only whether it is finished (so "waits for 1 more" still adds up)
  json visible_deps(const json& rows, const UserCtx& u);  // tasks waiting for this one: "you can start"
  std::set<int64_t> mentioned_in(const std::string& body, const json& t);

  // ================================================================== work requests
  void install_requests();

  json create_request(const json& b, const UserCtx& u);

  json approve_request(const json& q, const json& b, const UserCtx& u);

  json reject_request(const json& q, const json& b, const UserCtx& u);

  json withdraw_request(const json& q, const UserCtx& u);

  // ================================================================== sync (instant updates)
  // GET /sync?cursor=N&scope=V&wait=S  - returns what changed since N that the caller may see.
  // With wait>0 the request is held open (up to 25 s) until something relevant changes.
  void sync_handler(const Req& req, httplib::Response& res);

  json build_delta(const UserCtx& u, int64_t c, bool full);
};

}  // namespace tf::api
