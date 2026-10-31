// Inverted TaskFlow Server - task primitives: keys, creation, progress roll-up, reviewers (tasks module)
#include "server.h"

#include <algorithm>

namespace tf {

// ============================================================== tasks
json App::get_task(int64_t id) {
  json t = db.one("SELECT * FROM tasks WHERE id=?", {id});
  if (t.is_null()) throw ApiError(404, "Task not found");
  return t;
}

std::string App::next_task_key(int64_t project_id) {
  json p = db.one("SELECT code, next_num FROM projects WHERE id=?", {project_id});
  if (p.is_null()) throw ApiError(404, "Project not found");
  db.exec("UPDATE projects SET next_num=next_num+1 WHERE id=?", {project_id});
  return jstr(p, "code") + "-" + std::to_string(jint(p, "next_num", 1));
}

int64_t App::create_task(const json& f) {
  const std::string now = now_iso();
  const std::string status = jstr(f, "status", "TODO");
  const std::string due = jstr(f, "due_date");
  auto opt = [&](const char* k) { return jint(f, k) ? json(jint(f, k)) : json(); };
  int64_t id = db.insert(
      "INSERT INTO tasks(key,project_id,parent_id,title,description,dept_id,assignee_id,created_by,origin,"
      "requesting_dept_id,requested_by,request_id,priority,status,start_date,due_date,original_due,progress,"
      "is_milestone,recur,recur_day,created_at,updated_at,row_version,seq) VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,0,?,?,?,?,?,1,?)",
      {next_task_key(jint(f, "project_id")), jint(f, "project_id"), opt("parent_id"), jstr(f, "title"),
       jstr(f, "description"), jint(f, "dept_id"), opt("assignee_id"), jint(f, "created_by"), jstr(f, "origin", "MANAGER"),
       opt("requesting_dept_id"), opt("requested_by"), opt("request_id"), jstr(f, "priority", "MEDIUM"), status,
       jstr(f, "start_date"), due, status == "PROPOSED" ? std::string() : due, jint(f, "is_milestone") ? 1 : 0,
       jstr(f, "recur"),
       jstr(f, "recur") != "MONTHLY" ? 0 : jint(f, "recur_day") ? jint(f, "recur_day") : (due.size() == 10 ? std::stoll(due.substr(8, 2)) : 0),
       now, now, next_seq()});
  // whoever is given work in a project joins it (and so sees the rest of the project's work)
  if (status != "PROPOSED" && jint(f, "assignee_id")) ensure_project_member(jint(f, "project_id"), jint(f, "assignee_id"), jint(f, "created_by"));
  if (jint(f, "parent_id")) {
    // the new assignee may now see the sibling sub-tasks: re-send them with this change
    db.exec("UPDATE tasks SET seq=? WHERE parent_id=? AND id<>?", {next_seq(), jint(f, "parent_id"), id});
    recompute_progress(jint(f, "parent_id"));
  }
  return id;
}

void App::touch_task(int64_t id) {
  db.exec("UPDATE tasks SET seq=?, updated_at=?, row_version=row_version+1 WHERE id=?", {next_seq(), now_iso(), id});
}

void App::recompute_progress(int64_t task_id) {
  json t = db.one("SELECT id, parent_id, status, progress FROM tasks WHERE id=?", {task_id});
  if (t.is_null()) return;
  json c = db.one("SELECT COUNT(*) AS total, SUM(CASE WHEN status='DONE' THEN 1 ELSE 0 END) AS done FROM tasks "
                  "WHERE parent_id=? AND status NOT IN ('CANCELLED','REJECTED','PROPOSED')", {task_id});
  int64_t total = jint(c, "total"), done = jint(c, "done");
  int64_t pct;
  if (jstr(t, "status") == "DONE") pct = 100;
  else if (total > 0) pct = (done * 100 + total / 2) / total;
  else pct = 0;
  if (pct != jint(t, "progress")) {
    db.exec("UPDATE tasks SET progress=? WHERE id=?", {pct, task_id});
    touch_task(task_id);
  }
  if (jint(t, "parent_id")) recompute_progress(jint(t, "parent_id"));
}

std::set<int64_t> App::reviewers_for(const json& t) {
  int64_t parent = jint(t, "parent_id");
  if (parent) {
    json p = db.one("SELECT assignee_id FROM tasks WHERE id=?", {parent});
    // a person's own breakdown of their own task needs no separate review - the parent task is reviewed
    if (!p.is_null() && jint(p, "assignee_id") == jint(t, "assignee_id")) return {};
  }
  return managers_of(jint(t, "assignee_id"), jint(t, "dept_id"));
}

}  // namespace tf
