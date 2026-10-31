// Inverted TaskFlow Server - instant updates (long-poll GET /sync) and the per-user change feed
#include "api.h"

namespace tf::api {

// ================================================================== sync (instant updates)
// GET /sync?cursor=N&scope=V&wait=S  - returns what changed since N that the caller may see.
// With wait>0 the request is held open (up to 25 s) until something relevant changes.
void Api::sync_handler(const Req& req, httplib::Response& res) {
  if (!allow_request(client_ip(req))) return send(res, 429, {{"error", "Too many requests"}, {"code", "RATE_LIMIT"}});
  int64_t cursor = 0, scope = 0, wait = 0;
  try {
    if (req.has_param("cursor")) cursor = std::stoll(req.get_param_value("cursor"));
    if (req.has_param("scope")) scope = std::stoll(req.get_param_value("scope"));
    if (req.has_param("wait")) wait = std::max<int64_t>(0, std::min<int64_t>(25, std::stoll(req.get_param_value("wait"))));
  } catch (...) {
    return send(res, 400, {{"error", "Invalid parameters"}});
  }
  std::string a = req.get_header_value("Authorization");
  std::string token = a.rfind("Bearer ", 0) == 0 ? a.substr(7) : "";
  bool counted = false, busy = false, session_counted = false, user_checked = false;
  int64_t user_counted = 0;
  if (wait > 0) {
    // one session may hold at most 2 waiting requests, so nobody can take all the slots
    std::lock_guard<std::mutex> l(waits_mu);
    int& mine = waits_by_session[token];
    if (mine >= 2) {
      wait = 0;
      busy = true;
    } else {
      mine++;
      session_counted = true;
    }
  }
  if (wait > 0) {
    counted = true;
    if (app.waiters.fetch_add(1) >= app.cfg.max_clients) { wait = 0; busy = true; }  // full: answer now, client backs off
  }
  auto deadline = Clock::now() + std::chrono::seconds(wait);
  json out;
  int status = 200;
  try {
    std::unique_lock<std::recursive_mutex> lock(app.mu);
    for (;;) {
      UserCtx u;
      if (!app.load_user_by_token(token, u)) throw ApiError(401, "Session expired - please sign in again", "SESSION_EXPIRED");
      if (u.must_change_password) throw ApiError(403, "Please change your password first", "PASSWORD_CHANGE_REQUIRED");
      if (wait > 0 && !user_checked) {  // one person (several PCs) may hold at most 4 waiting requests
        user_checked = true;
        std::lock_guard<std::mutex> l(waits_mu);
        int& n = waits_by_user[u.id];
        if (n >= 4) {
          wait = 0;
          busy = true;
        } else {
          n++;
          user_counted = u.id;
        }
      }
      int64_t now_seq = app.current_seq();
      // cursor ahead of the server (database restored from a backup), or older than the kept list of lost
      // access: send everything again
      int64_t pruned = jint(app.db.one("SELECT CAST(value AS INTEGER) AS v FROM meta WHERE key='revocations_pruned_seq'"), "v");
      bool full = cursor <= 0 || scope != u.scope_ver || cursor > now_seq || cursor < pruned;
      if (full || now_seq > cursor) {
        out = build_delta(u, full ? 0 : cursor, full);
        if (full || !out.value("empty", false) || wait == 0) break;
        cursor = now_seq;  // nothing relevant for this user - keep waiting from here
      }
      if (wait == 0 || Clock::now() >= deadline || app.stopping) {
        out = {{"cursor", cursor}, {"scope_ver", u.scope_ver}, {"full", false}, {"empty", true}, {"server_time", now_iso()}};
        break;
      }
      app.cv.wait_until(lock, deadline);
    }
    // no free slot: the app must wait before asking again - also when this answer carries data
    // (otherwise a refused app would ask again at once, over and over)
    if (busy) out["retry_after"] = 10;
  } catch (const ApiError& e) {
    status = e.status;
    out = {{"error", e.what()}};
    if (!e.code.empty()) out["code"] = e.code;
  } catch (const std::exception& e) {
    status = 500;
    out = {{"error", "Server error"}};
    log_line(std::string("  ! sync -> ") + e.what());
  }
  if (counted) app.waiters--;
  if (session_counted || user_counted) {
    std::lock_guard<std::mutex> l(waits_mu);
    auto it = waits_by_session.find(token);
    if (session_counted && it != waits_by_session.end() && --it->second <= 0) waits_by_session.erase(it);
    auto iu = waits_by_user.find(user_counted);
    if (user_counted && iu != waits_by_user.end() && --iu->second <= 0) waits_by_user.erase(iu);
  }
  send(res, status, out);
}

json Api::build_delta(const UserCtx& u, int64_t c, bool full) {
  int64_t now_seq = app.current_seq();
  json out = {{"cursor", now_seq}, {"scope_ver", u.scope_ver}, {"full", full}, {"server_time", now_iso()}, {"today", today_local()}};
  auto vis = app.build_vis(u);
  bool any = false;
  auto put = [&](const char* k, json v) {
    if (!v.empty()) any = true;
    out[k] = std::move(v);
  };
  if (u.is_admin())
    put("users", app.db.query("SELECT id,name,email,phone,designation,global_role,status,requested_dept_id,decision_note,"
                              "created_at,last_login FROM users WHERE seq>?", {c}));
  else
    put("users", app.db.query("SELECT id,name,email,phone,designation,global_role,status FROM users "
                              "WHERE seq>? AND status IN ('ACTIVE','DISABLED')", {c}));
  put("departments", app.db.query("SELECT * FROM departments WHERE seq>?", {c}));
  put("memberships", app.db.query("SELECT * FROM memberships WHERE seq>?", {c}));

  json tasks = json::array(), revoked_tasks = json::array();
  std::set<int64_t> delta_ids, project_refs;
  std::map<int64_t, bool> vis_cache;
  // A full download leaves out work that closed more than a year ago in closed projects and in the routine
  // projects (architecture 7.2); open work and everything of running projects is always included, so progress
  // figures stay complete. Old tasks can still be opened one by one (GET /tasks/{id}).
  const std::string year_ago = iso_after_seconds(-365LL * 86400);
  // kept: open or recent work, everything of running projects, the main tasks of closed projects (their progress),
  // and every sub-task of a kept open / recent main task (so a task's breakdown is never half there)
  json task_rows = full ? app.db.query(
                              "WITH live AS (SELECT id FROM tasks WHERE status IN ('TODO','IN_PROGRESS','ON_HOLD','IN_REVIEW','PROPOSED') "
                              "              OR updated_at>=?) "
                              "SELECT t.* FROM tasks t JOIN projects p ON p.id=t.project_id "
                              "WHERE t.id IN (SELECT id FROM live) OR t.parent_id IN (SELECT id FROM live) "
                              "OR (p.status<>'CLOSED' AND p.is_routine=0) "
                              "OR (p.is_routine=0 AND (t.parent_id IS NULL OR t.parent_id=0))", {year_ago})
                        : app.db.query("SELECT * FROM tasks WHERE seq>?", {c});
  for (auto& t : task_rows) {
    int64_t id = jint(t, "id");
    bool ok = app.can_view_task(u, t, vis);
    vis_cache[id] = ok;
    if (ok) {
      delta_ids.insert(id);
      project_refs.insert(jint(t, "project_id"));
      tasks.push_back(std::move(t));
    }
  }
  // lost access is told only to the people who had it (reassignment / move); others never hear about the row
  if (!full)
    for (auto& r : app.db.query("SELECT DISTINCT entity_id FROM revocations WHERE user_id=? AND seq>? AND entity='task'", {u.id, c}))
      if (!delta_ids.count(jint(r, "entity_id"))) revoked_tasks.push_back(jint(r, "entity_id"));
  auto task_visible = [&](int64_t tid) {
    auto it = vis_cache.find(tid);
    if (it != vis_cache.end()) return it->second;
    json t = app.db.one("SELECT * FROM tasks WHERE id=?", {tid});
    bool ok = !t.is_null() && app.can_view_task(u, t, vis);
    vis_cache[tid] = ok;
    return ok;
  };
  auto in_list = [](const std::set<int64_t>& s) {
    std::string l;
    for (auto id : s) l += (l.empty() ? "" : ",") + std::to_string(id);
    return l;
  };
  // task-attached rows: changed ones on visible tasks + all rows for tasks that just became visible/changed
  const std::string recent = iso_after_seconds(-120LL * 86400);
  auto attached = [&](const char* table) {
    json rows = json::array();
    std::set<int64_t> seen;
    std::string sql = std::string("SELECT x.* FROM ") + table + " x WHERE x.seq>?";
    json params = json::array({c});
    if (full) {
      sql = std::string("SELECT x.* FROM ") + table + " x JOIN tasks t ON t.id=x.task_id WHERE t.status IN "
            "('TODO','IN_PROGRESS','ON_HOLD','IN_REVIEW','PROPOSED') OR t.updated_at>=?";
      params = json::array({recent});
    }
    for (auto& r : app.db.query(sql, params))
      if (task_visible(jint(r, "task_id")) && seen.insert(jint(r, "id")).second) rows.push_back(r);
    if (!full && !delta_ids.empty())
      for (auto& r : app.db.query(std::string("SELECT * FROM ") + table + " WHERE task_id IN (" + in_list(delta_ids) + ")"))
        if (seen.insert(jint(r, "id")).second) rows.push_back(r);
    return rows;
  };
  json comments = attached("comments");
  json exts = attached("extensions");
  json compl_rows = attached("completions");
  json helper_rows = attached("task_members");
  json dep_rows = json::array();
  for (auto r : attached("task_deps")) {  // the far end of a task this person cannot open is sent as 0 ("hidden")
    if (!task_visible(jint(r, "depends_on"))) {
      json d = app.db.one("SELECT status FROM tasks WHERE id=?", {r["depends_on"]});
      std::string st = d.is_null() ? "CANCELLED" : jstr(d, "status");
      r["depends_on"] = 0;
      r["hidden"] = 1;
      r["done"] = (st == "DONE" || st == "CANCELLED" || st == "REJECTED") ? 1 : 0;
    }
    dep_rows.push_back(std::move(r));
  }

  json reqs = json::array(), revoked_reqs = json::array();  // request visibility never narrows (fixed parties)
  json req_rows = full ? app.db.query("SELECT * FROM work_requests WHERE status='PENDING' OR decided_at>=? OR created_at>=?",
                                      {year_ago, year_ago})
                       : app.db.query("SELECT * FROM work_requests WHERE seq>?", {c});
  for (auto& q : req_rows) {
    if (app.can_view_request(u, q, vis)) {
      if (jint(q, "project_id")) project_refs.insert(jint(q, "project_id"));
      reqs.push_back(std::move(q));
    }
  }
  json projects = json::array();
  std::set<int64_t> seen_p;
  std::map<int64_t, bool> proj_cache;
  auto project_visible = [&](const json& p) {
    int64_t pid = jint(p, "id");
    auto it = proj_cache.find(pid);
    if (it != proj_cache.end()) return it->second;
    bool ok = u.sees_all() || u.member_of(jint(p, "dept_id")) || vis.managed.count(jint(p, "dept_id")) || project_refs.count(pid) ||
              vis.member_projects.count(pid);
    // projects of the parent department are useful to sub-department members
    if (!ok)
      for (auto& kv : u.units)
        if (app.parent_of(kv.first) == jint(p, "dept_id")) ok = true;
    // a renamed / re-dated project must also reach people who see it only through one of its tasks
    if (!ok && !full) {
      std::string cond = "assignee_id=? OR created_by=? OR requested_by=?";
      if (!vis.managed.empty())
        cond += " OR dept_id IN (" + in_list(vis.managed) + ") OR requesting_dept_id IN (" + in_list(vis.managed) + ")";
      if (!vis.my_tasks.empty()) cond += " OR id IN (" + in_list(vis.my_tasks) + ") OR parent_id IN (" + in_list(vis.my_tasks) + ")";
      if (!vis.parents_of_mine.empty())
        cond += " OR parent_id IN (" + in_list(vis.parents_of_mine) + ") OR id IN (" + in_list(vis.parents_of_mine) + ")";
      ok = !app.db.one("SELECT 1 AS x FROM tasks WHERE project_id=? AND (" + cond + ") LIMIT 1", {pid, u.id, u.id, u.id}).is_null();
    }
    proj_cache[pid] = ok;
    return ok;
  };
  for (auto& p : app.db.query("SELECT * FROM projects WHERE seq>?", {c})) {
    if (!project_visible(p)) continue;
    seen_p.insert(jint(p, "id"));
    projects.push_back(std::move(p));
  }
  // the people of a project: for its members, its owner and the people who manage it (not for everyone who sees
  // the project's name)
  json pm_rows = json::array();
  std::map<int64_t, bool> pm_cache;
  auto pm_entitled = [&](int64_t pid) {
    auto it = pm_cache.find(pid);
    if (it != pm_cache.end()) return it->second;
    bool ok = vis.member_projects.count(pid) > 0;
    if (!ok) ok = app.may_manage_project(u, app.db.one("SELECT * FROM projects WHERE id=?", {pid}));
    pm_cache[pid] = ok;
    return ok;
  };
  if (full) {
    for (auto& r : app.db.query("SELECT * FROM project_members"))
      if (pm_entitled(jint(r, "project_id"))) pm_rows.push_back(std::move(r));
  } else {
    std::set<int64_t> joined;  // projects this person just joined: all of their people
    for (auto& r : app.db.query("SELECT * FROM project_members WHERE seq>?", {c})) {
      if (!pm_entitled(jint(r, "project_id"))) continue;
      if (jint(r, "user_id") == u.id && jint(r, "active")) joined.insert(jint(r, "project_id"));
      pm_rows.push_back(std::move(r));
    }
    if (!joined.empty())
      for (auto& r : app.db.query("SELECT * FROM project_members WHERE seq<=? AND project_id IN (" + in_list(joined) + ")", {c}))
        pm_rows.push_back(std::move(r));
  }
  std::set<int64_t> missing;
  for (auto pid : project_refs) if (!seen_p.count(pid)) missing.insert(pid);
  if (!full && !missing.empty())
    for (auto& p : app.db.query("SELECT * FROM projects WHERE id IN (" + in_list(missing) + ")")) projects.push_back(std::move(p));

  put("tasks", std::move(tasks));
  put("comments", std::move(comments));
  put("extensions", std::move(exts));
  put("completions", std::move(compl_rows));
  put("requests", std::move(reqs));
  put("projects", std::move(projects));
  put("project_members", std::move(pm_rows));
  put("task_members", std::move(helper_rows));
  put("task_deps", std::move(dep_rows));
  put("notifications", full ? app.db.query("SELECT * FROM (SELECT * FROM notifications WHERE user_id=? ORDER BY id DESC LIMIT 300) ORDER BY id", {u.id})
                            : app.db.query("SELECT * FROM notifications WHERE user_id=? AND seq>?", {u.id, c}));
  if (!revoked_tasks.empty() || !revoked_reqs.empty()) any = true;
  out["revoked"] = {{"tasks", revoked_tasks}, {"requests", revoked_reqs}};
  if (!any && !full) out["empty"] = true;
  return out;
}

}  // namespace tf::api
