// Inverted TaskFlow Server - organisation model (departments, sub-departments, managers) and who may see what (org module)
#include "server.h"

#include <algorithm>
#include <map>
#include <set>

namespace tf {

// ============================================================== organisation
int64_t App::parent_of(int64_t unit) { return jint(db.one("SELECT parent_id FROM departments WHERE id=?", {unit}), "parent_id"); }

std::vector<int64_t> App::ancestors(int64_t unit) {
  std::vector<int64_t> v;
  int64_t p = parent_of(unit);
  for (int guard = 0; p && guard < 8; guard++) {
    v.push_back(p);
    p = parent_of(p);
  }
  return v;
}

std::set<int64_t> App::subtree(int64_t unit) {
  std::set<int64_t> s = {unit};
  std::vector<int64_t> frontier = {unit};
  for (int guard = 0; !frontier.empty() && guard < 8; guard++) {
    std::vector<int64_t> next;
    for (auto f : frontier)
      for (auto& r : db.query("SELECT id FROM departments WHERE parent_id=? AND active=1", {f}))
        if (s.insert(r["id"].get<int64_t>()).second) next.push_back(r["id"].get<int64_t>());
    frontier = next;
  }
  return s;
}

std::string App::role_in(int64_t user, int64_t unit) {
  json r = db.one("SELECT m.role FROM memberships m JOIN departments d ON d.id=m.dept_id "
                  "WHERE m.user_id=? AND m.dept_id=? AND m.active=1 AND d.active=1", {user, unit});
  return r.is_null() ? std::string() : jstr(r, "role");
}

std::set<int64_t> App::leaders(int64_t unit, bool heads_only) {
  std::set<int64_t> s;
  for (auto& r : db.query(std::string("SELECT m.user_id FROM memberships m JOIN users u ON u.id=m.user_id "
                                      "JOIN departments d ON d.id=m.dept_id "
                                      "WHERE m.dept_id=? AND m.active=1 AND d.active=1 AND u.status='ACTIVE' AND m.role IN ") +
                              (heads_only ? "('HEAD')" : "('HEAD','DEPUTY')"),
                          {unit}))
    s.insert(r["user_id"].get<int64_t>());
  return s;
}

int64_t App::management_unit() {
  return jint(db.one("SELECT id FROM departments WHERE is_management=1 AND active=1 ORDER BY id LIMIT 1"), "id");
}

std::set<int64_t> App::managers_of(int64_t user, int64_t unit) {
  std::string role = role_in(user, unit);
  std::set<int64_t> m;
  if (role == "DEPUTY") m = leaders(unit, true);
  else if (role != "HEAD") m = leaders(unit, false);
  m.erase(user);
  if (m.empty())
    for (auto a : ancestors(unit)) {
      m = leaders(a, false);
      m.erase(user);
      if (!m.empty()) break;
    }
  if (m.empty()) {
    int64_t mg = management_unit();
    auto anc = ancestors(unit);
    bool inside_mgmt = mg == unit || std::find(anc.begin(), anc.end(), mg) != anc.end();
    if (mg && !inside_mgmt) {
      m = leaders(mg, false);  // the head of Management and their deputies
      m.erase(user);
    }
  }
  return m;  // empty = nobody above: see self_managed() / missing_reviewer_reason()
}

bool App::self_managed(int64_t user, int64_t unit) {
  if (!managers_of(user, unit).empty()) return false;
  int64_t mg = management_unit();
  if (!mg) return true;  // no Management department configured: department heads are the top
  std::string r = role_in(user, mg);
  return r == "HEAD" || r == "DEPUTY";
}

std::string App::missing_reviewer_reason(int64_t user, int64_t unit) {
  if (self_managed(user, unit) || !managers_of(user, unit).empty()) return "";
  return "Nobody can review this yet: there is no head above you (the Management department has no head). "
         "Ask the IT administrator to assign one in Administration > Departments.";
}

bool App::user_can_view_task(int64_t user, const json& t) {
  try {
    UserCtx u = load_ctx(user);
    if (u.status != "ACTIVE") return false;
    return can_view_task(u, t);
  } catch (...) {
    return false;
  }
}

std::set<int64_t> App::requester_side(const json& t) {
  std::set<int64_t> s;
  if (jint(t, "requested_by")) s.insert(jint(t, "requested_by"));
  if (jint(t, "requesting_dept_id"))
    for (auto l : leaders(jint(t, "requesting_dept_id"))) s.insert(l);
  return s;
}

std::set<int64_t> App::related_tasks(int64_t task_id) {
  std::set<int64_t> s = {task_id};
  json t = db.one("SELECT parent_id FROM tasks WHERE id=?", {task_id});
  int64_t parent = jint(t, "parent_id");
  for (auto& r : db.query("SELECT id FROM tasks WHERE parent_id=? OR (? <> 0 AND (parent_id=? OR id=?))", {task_id, parent, parent, parent}))
    s.insert(jint(r, "id"));
  return s;
}

// Everyone who could possibly see these tasks: the people on them and the leaders of every unit involved
// (a superset of the real viewers - far cheaper than checking every user of the company).
std::set<int64_t> App::viewer_candidates(const std::set<int64_t>& task_ids) {
  std::set<int64_t> c, units;
  for (auto id : task_ids) {
    json t = db.one("SELECT assignee_id, created_by, requested_by, dept_id, requesting_dept_id FROM tasks WHERE id=?", {id});
    if (t.is_null()) continue;
    for (const char* k : {"assignee_id", "created_by", "requested_by"})
      if (jint(t, k)) c.insert(jint(t, k));
    for (const char* k : {"dept_id", "requesting_dept_id"})
      if (jint(t, k)) {
        units.insert(jint(t, k));
        for (auto a : ancestors(jint(t, k))) units.insert(a);
      }
  }
  if (int64_t mg = management_unit()) units.insert(mg);
  for (auto u : units)
    for (auto l : leaders(u)) c.insert(l);
  return c;
}

std::map<int64_t, std::set<int64_t>> App::task_viewers(const std::set<int64_t>& task_ids, const std::set<int64_t>& candidates) {
  std::map<int64_t, std::set<int64_t>> out;
  std::vector<json> ts;
  for (auto id : task_ids) {
    json t = db.one("SELECT * FROM tasks WHERE id=?", {id});
    if (!t.is_null()) ts.push_back(t);
  }
  if (ts.empty()) return out;
  for (auto uid : candidates) {
    UserCtx u;
    try {
      u = load_ctx(uid);
    } catch (...) {
      continue;
    }
    if (u.status != "ACTIVE" || u.sees_all()) continue;  // management and IT never lose access
    VisCtx v = build_vis(u);
    for (auto& t : ts)
      if (can_view_task(u, t, v)) out[jint(t, "id")].insert(u.id);
  }
  return out;
}

void App::record_revocations(const std::map<int64_t, std::set<int64_t>>& before, const std::map<int64_t, std::set<int64_t>>& after) {
  int64_t seq = 0;
  for (auto& kv : before) {
    auto it = after.find(kv.first);
    for (auto user : kv.second) {
      if (it != after.end() && it->second.count(user)) continue;
      if (!seq) seq = next_seq();
      db.insert("INSERT INTO revocations(seq,user_id,entity,entity_id,created_at) VALUES(?,?,'task',?,?)", {seq, user, kv.first, now_iso()});
    }
  }
}

std::set<int64_t> App::authority_over(int64_t user, int64_t unit) {
  std::set<int64_t> a = managers_of(user, unit);
  for (auto anc : ancestors(unit))
    for (auto l : leaders(anc, false)) a.insert(l);
  a.erase(user);
  return a;
}

std::set<int64_t> App::managed_units(const UserCtx& u) {
  std::set<int64_t> s;
  for (auto& kv : u.units)
    if (kv.second == "HEAD" || kv.second == "DEPUTY")
      for (auto x : subtree(kv.first)) s.insert(x);
  return s;
}

std::string App::unit_label(int64_t unit) {
  json d = db.one("SELECT name, parent_id FROM departments WHERE id=?", {unit});
  if (d.is_null()) return "Unit #" + std::to_string(unit);
  if (jint(d, "parent_id")) {
    json p = db.one("SELECT name FROM departments WHERE id=?", {d["parent_id"]});
    if (!p.is_null()) return jstr(p, "name") + " / " + jstr(d, "name");
  }
  return jstr(d, "name");
}

// ============================================================== visibility
App::VisCtx App::build_vis(const UserCtx& u) {
  VisCtx v;
  for (auto& r : db.query("SELECT id, parent_id FROM tasks WHERE assignee_id=?", {u.id})) {
    v.my_tasks.insert(r["id"].get<int64_t>());
    if (!r["parent_id"].is_null()) v.parents_of_mine.insert(r["parent_id"].get<int64_t>());
  }
  v.managed = managed_units(u);
  return v;
}

bool App::can_view_task(const UserCtx& u, const json& t, const VisCtx& v) {
  const std::string st = jstr(t, "status");
  const int64_t unit = jint(t, "dept_id");
  const bool mine = jint(t, "assignee_id") == u.id || jint(t, "created_by") == u.id;
  if (st == "PROPOSED" || st == "REJECTED") return mine || has_authority(u, jint(t, "assignee_id"), unit);
  if (u.sees_all() || mine || v.managed.count(unit)) return true;
  if (jint(t, "requested_by") == u.id) return true;
  if (jint(t, "requesting_dept_id") && v.managed.count(jint(t, "requesting_dept_id"))) return true;
  if (jint(t, "parent_id") && v.my_tasks.count(jint(t, "parent_id"))) return true;  // sub-task of my task
  if (jint(t, "parent_id") && v.parents_of_mine.count(jint(t, "parent_id"))) return true;  // sibling of my sub-task
  if (v.parents_of_mine.count(jint(t, "id"))) return true;                          // parent of my sub-task
  return false;
}

bool App::can_view_request(const UserCtx& u, const json& r, const VisCtx& v) {
  if (jint(r, "requested_by") == u.id) return true;
  if (jstr(r, "type") == "CROSS_TEAM") {
    if (u.sees_all() || v.managed.count(jint(r, "target_dept_id"))) return true;
    if (jint(r, "requesting_dept_id") && v.managed.count(jint(r, "requesting_dept_id"))) return true;
    return false;
  }
  // proposals and help requests are personal: only the requester's managers see them (not a fellow deputy,
  // and not the deputy of a head who proposed own work)
  return has_authority(u, jint(r, "requested_by"), jint(r, "target_dept_id"));
}

}  // namespace tf
