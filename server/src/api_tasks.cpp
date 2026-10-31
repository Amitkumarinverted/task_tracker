// Inverted TaskFlow Server - task endpoints (create, edit, life cycle, comments, extensions)
#include "api.h"

namespace tf::api {

// ================================================================== tasks
void Api::install_tasks() {
  route("POST", "/tasks", true, [this](const Req&, const json& b, UserCtx* u) { return create_task(b, *u); });

  route("PATCH", R"(/tasks/(\d+))", true, [this](const Req& r, const json& b, UserCtx* u) {
    json t = app.get_task(id_arg(r));
    int64_t id = jint(t, "id");
    require(app.can_view_task(*u, t), 404, "Task not found");
    // optimistic locking: an edit must say which version it is based on, so nobody overwrites a newer change unseen
    require(jhas(b, "row_version"), 400, "row_version is required when editing a task");
    require(jint(b, "row_version") == jint(t, "row_version"), 409, "This task changed while you were editing it (by someone else or through its sub-tasks). "
            "The newest version is loaded - check your entries and press Save again.");
    bool manager = app.can_manage_task(*u, t);
    bool own_sub = jstr(t, "origin") == "SUBTASK" && jint(t, "created_by") == u->id && jint(t, "assignee_id") == u->id;
    // the person at the top re-plans their own work themselves (nobody above could approve an extension)
    bool top_own = jint(t, "assignee_id") == u->id && app.self_managed(u->id, jint(t, "dept_id"));
    require(manager || own_sub || top_own, 403, "Only the manager of this task can edit it");
    require(kOpen.count(jstr(t, "status")) > 0, 400, "Only open tasks can be edited");
    std::string title = jhas(b, "title") ? req_text(b, "title", "Title") : jstr(t, "title");
    std::string desc = jhas(b, "description") ? opt_text(b, "description", 5000, "Description") : jstr(t, "description");
    std::string pri = jhas(b, "priority") ? priority_of(b) : jstr(t, "priority");
    std::string start = jhas(b, "start_date") ? opt_date(b, "start_date", "Start date") : jstr(t, "start_date");
    std::string due = jhas(b, "due_date") ? req_date(b, "due_date", "Due date") : jstr(t, "due_date");
    require(start.empty() || due.empty() || start <= due, 400, "Start date must be on or before the due date");
    if (jint(t, "parent_id") && due != jstr(t, "due_date")) {
      json parent = app.get_task(jint(t, "parent_id"));
      require(jstr(parent, "due_date").empty() || due <= jstr(parent, "due_date"), 400,
              "A sub-task must be due on or before its main task (" + hdate(jstr(parent, "due_date")) + ")");
    }
    int64_t project = jint(t, "project_id");
    if (jhas(b, "project_id") && jint(b, "project_id") != project) {
      require(!jint(t, "parent_id"), 400, "A sub-task always stays in its parent's project");
      json p = project_row(jint(b, "project_id"));
      check_project_open(p);
      require(app.can_use_project(*u, p), 403, "You are not part of project " + jstr(p, "name") + " - ask its owner to add you");
      project = jint(p, "id");
    }
    bool reassign = jhas(b, "assignee_id") && jint(b, "assignee_id") != jint(t, "assignee_id");
    bool moved = project != jint(t, "project_id");
    std::string recur = recur_of(b, jstr(t, "recur"));
    require(recur.empty() || !jint(t, "parent_id"), 400,
            "Only main tasks can repeat - a repeating sub-task would keep its main task open forever");
    // a monthly series keeps the day of the month it was set to (31 stays 31 after a short month)
    int64_t recur_day = recur != "MONTHLY" ? 0
                        : (recur == jstr(t, "recur") && due == jstr(t, "due_date") && jint(t, "recur_day")) ? jint(t, "recur_day")
                                                                                                             : (due.size() == 10 ? std::stoll(due.substr(8, 2)) : 0);
    int64_t milestone = jhas(b, "is_milestone") ? (jint(b, "is_milestone") ? 1 : 0) : jint(t, "is_milestone");
    bool set_help = jhas(b, "helpers"), set_dep = jhas(b, "depends_on");
    std::vector<int64_t> helpers = set_help ? id_list(b, "helpers", 10, "people helping") : std::vector<int64_t>();
    std::vector<int64_t> deps = set_dep ? id_list(b, "depends_on", 10, "tasks to wait for") : std::vector<int64_t>();
    Tx tx(app.db);
    // who sees this task (and its parent, siblings, sub-tasks) now - to tell those who lose access afterwards
    std::map<int64_t, std::set<int64_t>> viewers_before;
    std::set<int64_t> related, candidates;
    if (reassign || moved) {
      related = app.related_tasks(id);
      candidates = app.viewer_candidates(related, moved);  // only these people can have seen the task before
      viewers_before = app.task_viewers(related, candidates);
    }
    app.db.exec("UPDATE tasks SET title=?,description=?,priority=?,start_date=?,due_date=?,project_id=?,is_milestone=?,recur=?,recur_day=? "
                "WHERE id=?",
                {title, desc, pri, start, due, project, milestone, recur, recur_day, id});
    if (recur != jstr(t, "recur"))
      app.add_event(id, u->id, recur.empty() ? std::string("No longer repeats")
                                             : std::string("Repeats ") + (recur == "DAILY" ? "daily" : recur == "WEEKLY" ? "weekly" : "monthly") +
                                                   " - the next one is created when this one is done");
    if (moved) {  // sub-tasks always live in their main task's project
      app.db.exec("UPDATE tasks SET project_id=? WHERE parent_id=?", {project, id});
      for (auto& sub : app.db.query("SELECT id FROM tasks WHERE parent_id=?", {id})) app.touch_task(jint(sub, "id"));
      app.add_event(id, u->id, "Moved to project " + jstr(project_row(project), "name"));
      // a moved task and its sub-tasks no longer wait for (or are waited for by) tasks left in the old project
      std::set<int64_t> fam = {id};
      for (auto& sub : app.db.query("SELECT id FROM tasks WHERE parent_id=?", {id})) fam.insert(jint(sub, "id"));
      for (auto x : fam)
        for (auto& d : app.db.query("SELECT d.id, a.project_id AS pa, b.project_id AS pb FROM task_deps d JOIN tasks a ON a.id=d.task_id "
                                    "JOIN tasks b ON b.id=d.depends_on WHERE d.active=1 AND (d.task_id=? OR d.depends_on=?)", {x, x}))
          if (jint(d, "pa") != jint(d, "pb")) app.db.exec("UPDATE task_deps SET active=0, seq=? WHERE id=?", {app.next_seq(), d["id"]});
    }
    if (due != jstr(t, "due_date")) {
      app.add_event(id, u->id, "Due date changed " + hdate(jstr(t, "due_date")) + " -> " + hdate(due));
      if (jint(t, "assignee_id") != u->id)
        app.notify(jint(t, "assignee_id"), "DUE_CHANGED", "Due date changed: " + jstr(t, "key"), "New due date " + hdate(due), id, 0, true);
      std::set<int64_t> told = app.requester_side(t);
      for (auto h : app.task_helpers(id)) told.insert(h);
      app.notify_all(told, u->id, "DUE_CHANGED", "Due date changed: " + jstr(t, "key"), "New due date " + hdate(due), id);
    }
    if (reassign) {
      require(manager || top_own, 403, "Only the manager can reassign this task");
      int64_t na = jint(b, "assignee_id");
      require(is_active_user(na), 400, "That person is not an active user");
      int64_t unit = pick_unit(*u, na, jint(b, "dept_id"));
      require(app.has_authority(*u, na, unit) || (na == u->id && u->leads(unit)), 403,
              "You can only assign work to people you manage");
      app.db.exec("UPDATE tasks SET assignee_id=?, dept_id=? WHERE id=?", {na, unit, id});
      // the new owner of the main task now receives its sub-tasks' completions (they asked for that work)
      int64_t old = jint(t, "assignee_id");
      app.db.exec("UPDATE tasks SET requested_by=? WHERE parent_id=? AND (requested_by=? OR requested_by IS NULL OR requested_by=0) "
                  "AND assignee_id<>?", {na, id, old, na});
      app.db.exec("UPDATE tasks SET requested_by=NULL WHERE parent_id=? AND assignee_id=?", {id, na});
      for (auto& sub : app.db.query("SELECT id FROM tasks WHERE parent_id=?", {id})) app.touch_task(jint(sub, "id"));
      if (jint(t, "parent_id"))
        app.db.exec("UPDATE tasks SET seq=? WHERE (parent_id=? AND id<>?) OR id=?",
                    {app.next_seq(), t["parent_id"], id, t["parent_id"]});
      app.add_event(id, u->id, "Reassigned from " + app.name_of(jint(t, "assignee_id")) + " to " + app.name_of(na));
      if (app.task_helpers(id).count(na))  // the new owner is no longer just helping
        app.db.exec("UPDATE task_members SET active=0, seq=? WHERE task_id=? AND user_id=?", {app.next_seq(), id, na});
      if (na != u->id) app.notify(na, "TASK_ASSIGNED", "Task assigned to you: " + jstr(t, "key"), title, id, 0, true);
      app.notify(jint(t, "assignee_id"), "STATUS_CHANGED", "Task reassigned: " + jstr(t, "key"), "Now handled by " + app.name_of(na), id);
    }
    if (reassign || moved) {
      // whoever now does the work (and the sub-tasks' people, after a move) joins the project;
      // people who were in the old place only through this work leave it
      int64_t now_a = jint(app.get_task(id), "assignee_id");
      app.ensure_project_member(project, now_a, u->id);
      std::set<int64_t> subs;
      for (auto& sub : app.db.query("SELECT assignee_id FROM tasks WHERE parent_id=? AND status NOT IN ('PROPOSED','REJECTED','CANCELLED')", {id}))
        subs.insert(jint(sub, "assignee_id"));
      if (moved)
        for (auto s : subs) app.ensure_project_member(project, s, u->id);
      app.release_auto_member(jint(t, "project_id"), jint(t, "assignee_id"));
      if (moved)
        for (auto s : subs) app.release_auto_member(jint(t, "project_id"), s);
      if (reassign) app.release_auto_member(project, jint(t, "assignee_id"));
    }
    if (set_help) set_helpers(app.get_task(id), helpers, *u);
    if (set_dep) set_deps(app.get_task(id), deps, *u);
    app.touch_task(id);
    if (reassign || moved) app.record_revocations(viewers_before, app.task_viewers(related, candidates));
    app.audit(u->id, "update_task", "task", id, b.dump());
    tx.commit();
    return json{{"ok", true}};
  });

  route("GET", R"(/tasks/(\d+))", true, [this](const Req& r, const json&, UserCtx* u) {
    json t = app.get_task(id_arg(r));
    require(app.can_view_task(*u, t), 404, "Task not found");
    return json{{"task", t},
                {"comments", app.db.query("SELECT * FROM comments WHERE task_id=? ORDER BY id", {t["id"]})},
                {"completions", app.db.query("SELECT * FROM completions WHERE task_id=? ORDER BY id", {t["id"]})},
                {"extensions", app.db.query("SELECT * FROM extensions WHERE task_id=? ORDER BY id", {t["id"]})},
                {"task_members", app.db.query("SELECT * FROM task_members WHERE task_id=?", {t["id"]})},
                {"task_deps", visible_deps(app.db.query("SELECT * FROM task_deps WHERE task_id=? OR depends_on=?", {t["id"], t["id"]}), *u)}};
  });

  route("POST", R"(/tasks/(\d+)/(start|hold|resume|submit|withdraw|approve|reject|reopen|cancel))", true,
        [this](const Req& r, const json& b, UserCtx* u) { return task_action(app.get_task(id_arg(r)), r.matches[2].str(), b, *u); });

  route("POST", R"(/tasks/(\d+)/comments)", true, [this](const Req& r, const json& b, UserCtx* u) {
    json t = app.get_task(id_arg(r));
    require(app.can_view_task(*u, t), 404, "Task not found");
    std::string body = req_text(b, "body", "Comment", 5000);
    Tx tx(app.db);
    int64_t id = app.db.insert("INSERT INTO comments(task_id,author_id,kind,body,created_at,seq) VALUES(?,?,?,?,?,?)",
                               {t["id"], u->id, "COMMENT", body, now_iso(), app.next_seq()});
    std::set<int64_t> to = {jint(t, "assignee_id"), jint(t, "created_by"), jint(t, "requested_by")};
    for (auto h : app.task_helpers(jint(t, "id"))) to.insert(h);
    std::set<int64_t> mentioned = mentioned_in(body, t);  // "@Full Name" of someone who can open the task
    if (jint(t, "parent_id")) to.insert(jint(app.db.one("SELECT assignee_id FROM tasks WHERE id=?", {t["parent_id"]}), "assignee_id"));
    for (auto& c : app.db.query("SELECT DISTINCT author_id FROM comments WHERE task_id=? AND kind='COMMENT'", {t["id"]}))
      to.insert(jint(c, "author_id"));
    std::set<int64_t> allowed;  // never send comment text to someone who can no longer open the task
    for (auto x : to)
      if (x && x != u->id && app.user_can_view_task(x, t)) allowed.insert(x);
    std::string snippet = body.size() > 140 ? body.substr(0, 137) + "..." : body;
    for (auto m : mentioned) allowed.erase(m);  // one message each: the mention says more
    app.notify_all(mentioned, u->id, "MENTION", u->name + " mentioned you on " + jstr(t, "key"), snippet, jint(t, "id"), 0, true);
    app.notify_all(allowed, u->id, "COMMENT", u->name + " commented on " + jstr(t, "key"), snippet, jint(t, "id"));
    tx.commit();
    return json{{"ok", true}, {"id", id}};
  });

  route("POST", R"(/tasks/(\d+)/extensions)", true, [this](const Req& r, const json& b, UserCtx* u) {
    json t = app.get_task(id_arg(r));
    require(app.can_view_task(*u, t), 404, "Task not found");
    require(jint(t, "assignee_id") == u->id, 403, "Only the person doing the task can request an extension");
    require(kOpen.count(jstr(t, "status")) > 0, 400, "Extensions can only be requested on open tasks");
    std::string nd = req_date(b, "proposed_due", "New due date");
    require(nd > jstr(t, "due_date"), 400, "The new due date must be later than the current one");
    std::string reason = req_text(b, "reason", "Reason", 2000);
    require(jint(app.db.one("SELECT COUNT(*) AS n FROM extensions WHERE task_id=? AND status='PENDING'", {t["id"]}), "n") == 0,
            409, "An extension request is already waiting for this task");
    auto mgrs = app.managers_of(u->id, jint(t, "dept_id"));
    if (mgrs.empty()) {
      std::string why = app.missing_reviewer_reason(u->id, jint(t, "dept_id"));
      throw ApiError(400, why.empty() ? "Nobody above you approves extensions - change the due date directly (Edit)" : why);
    }
    Tx tx(app.db);
    int64_t id = app.db.insert("INSERT INTO extensions(task_id,requested_by,current_due,proposed_due,reason,status,created_at,seq) "
                               "VALUES(?,?,?,?,?,'PENDING',?,?)",
                               {t["id"], u->id, t["due_date"], nd, reason, now_iso(), app.next_seq()});
    app.add_event(jint(t, "id"), u->id, "Requested more time until " + hdate(nd) + ": " + reason);
    app.audit(u->id, "request_extension", "extension", id, nd);
    app.notify_all(mgrs, u->id, "EXTENSION_REQUESTED", "Extension requested: " + jstr(t, "key"),
                   u->name + " asks to move the due date to " + hdate(nd), jint(t, "id"), 0, true);
    app.touch_task(jint(t, "id"));
    tx.commit();
    return json{{"ok", true}, {"id", id}};
  });

  route("POST", R"(/extensions/(\d+)/(approve|reject))", true, [this](const Req& r, const json& b, UserCtx* u) {
    json e = app.db.one("SELECT * FROM extensions WHERE id=?", {id_arg(r)});
    require(!e.is_null(), 404, "Extension request not found");
    require(jstr(e, "status") == "PENDING", 400, "This request was already decided");
    json t = app.get_task(jint(e, "task_id"));
    require(app.can_manage_task(*u, t), 403, "Only the manager of this task can decide extensions");
    require(kOpen.count(jstr(t, "status")) > 0 || jstr(t, "status") == "IN_REVIEW", 400,
            "This task is already finished - there is no due date left to move");
    bool approve = r.matches[2].str() == "approve";
    std::string note = opt_text(b, "note", 1000, "Note");
    if (!approve) require(!note.empty(), 400, "Please give a reason for rejecting");
    std::string nd = approve ? (jhas(b, "due_date") ? req_date(b, "due_date", "Due date") : jstr(e, "proposed_due")) : "";
    Tx tx(app.db);
    app.db.exec("UPDATE extensions SET status=?, decided_by=?, decision_note=?, decided_at=?, seq=? WHERE id=?",
                {approve ? "APPROVED" : "REJECTED", u->id, note, now_iso(), app.next_seq(), e["id"]});
    if (approve) {
      app.db.exec("UPDATE tasks SET due_date=? WHERE id=?", {nd, t["id"]});
      app.add_event(jint(t, "id"), u->id, "Extension approved: due " + hdate(jstr(t, "due_date")) + " -> " + hdate(nd) + (note.empty() ? "" : " (" + note + ")"));
      app.notify_all(app.requester_side(t), u->id, "DUE_CHANGED", "Due date changed: " + jstr(t, "key"), "New due date " + hdate(nd), jint(t, "id"));
    } else {
      app.add_event(jint(t, "id"), u->id, "Extension rejected: " + note);
    }
    std::set<int64_t> told = {jint(e, "requested_by"), jint(t, "assignee_id")};  // the task may have changed hands since
    app.notify_all(told, u->id, approve ? "EXTENSION_APPROVED" : "EXTENSION_REJECTED",
                   std::string(approve ? "Extension approved: " : "Extension rejected: ") + jstr(t, "key"),
                   approve ? "New due date " + hdate(nd) : note, jint(t, "id"), 0, true);
    app.touch_task(jint(t, "id"));
    app.audit(u->id, approve ? "approve_extension" : "reject_extension", "extension", jint(e, "id"), nd);
    tx.commit();
    return json{{"ok", true}};
  });
}

json Api::create_task(const json& b, const UserCtx& u) {
  int64_t assignee = jint(b, "assignee_id", u.id);
  require(is_active_user(assignee), 400, "Choose an active person to do this task");
  std::string title = req_text(b, "title", "Title");
  std::string start = opt_date(b, "start_date", "Start date");
  std::string due = req_date(b, "due_date", "Due date");
  require(start.empty() || start <= due, 400, "Start date must be on or before the due date");
  int64_t parent_id = jint(b, "parent_id");
  require(!parent_id || recur_of(b, "").empty(), 400, "Only main tasks can repeat - a repeating sub-task would keep its main task open forever");
  json f = {{"title", title}, {"description", opt_text(b, "description", 5000, "Description")}, {"assignee_id", assignee},
            {"created_by", u.id}, {"priority", priority_of(b)}, {"status", "TODO"}, {"start_date", start}, {"due_date", due},
            {"recur", recur_of(b, "")}, {"is_milestone", jint(b, "is_milestone") ? 1 : 0}};
  std::vector<int64_t> helpers = jhas(b, "helpers") ? id_list(b, "helpers", 10, "people helping") : std::vector<int64_t>();
  std::vector<int64_t> deps = jhas(b, "depends_on") ? id_list(b, "depends_on", 10, "tasks to wait for") : std::vector<int64_t>();
  json parent;
  int64_t unit;
  if (parent_id) {
    parent = app.get_task(parent_id);
    require(app.can_view_task(u, parent), 404, "Parent task not found");
    require(!jint(parent, "parent_id"), 400, "A sub-task cannot have its own sub-tasks - add it to the main task instead");
    require(kOpen.count(jstr(parent, "status")) > 0, 400, "Sub-tasks can only be added to open tasks");
    // breaking a task down belongs to its owner and their managers - not to everyone who can see it
    require(jint(parent, "assignee_id") == u.id || app.can_manage_task(u, parent), 403,
            "You can add sub-tasks only to your own tasks or to tasks of people you manage");
    bool own = assignee == u.id && jint(parent, "assignee_id") == u.id;
    if (own) unit = jint(parent, "dept_id");
    else {
      unit = pick_unit(u, assignee, jint(b, "dept_id"));
      require(app.has_authority(u, assignee, unit) || (assignee == u.id && u.leads(unit)), 403,
              "You can add sub-tasks for yourself, or for people you manage");
    }
    if (due > jstr(parent, "due_date") && !jstr(parent, "due_date").empty())
      throw ApiError(400, "The sub-task is due after its main task (" + jstr(parent, "due_date") + ") - adjust one of the dates");
    f["parent_id"] = parent_id;
    f["project_id"] = parent["project_id"];
    f["origin"] = "SUBTASK";
    if (jint(parent, "assignee_id") != assignee) f["requested_by"] = parent["assignee_id"];
  } else {
    json p = project_row(jint(b, "project_id"));
    check_project_open(p);
    require(app.can_use_project(u, p), 403, "You are not part of project " + jstr(p, "name") + " - ask its owner to add you");
    unit = pick_unit(u, assignee, jint(b, "dept_id"));
    if (assignee == u.id) require(u.leads(unit) || u.management, 403, "To add your own work, use 'Propose task' - your manager approves it");
    else require(app.has_authority(u, assignee, unit), 403, "You can only assign tasks to people you manage");
    f["project_id"] = p["id"];
    f["origin"] = "MANAGER";
  }
  f["dept_id"] = unit;
  Tx tx(app.db);
  int64_t id = app.create_task(f);
  json t = app.get_task(id);
  app.add_event(id, u.id, std::string(parent_id ? "Sub-task created" : "Created") + " and assigned to " + app.name_of(assignee) + ", due " + hdate(due));
  if (!helpers.empty()) set_helpers(t, helpers, u);
  if (!deps.empty()) set_deps(t, deps, u);
  if (assignee != u.id)
    app.notify(assignee, parent_id ? "SUBTASK_ASSIGNED" : "TASK_ASSIGNED",
               std::string(parent_id ? "New sub-task for you: " : "New task for you: ") + jstr(t, "key"), title, id, 0, true);
  if (parent_id) {
    app.add_event(parent_id, u.id, "Sub-task " + jstr(t, "key") + " added for " + app.name_of(assignee) + ": " + title);
    int64_t owner = jint(parent, "assignee_id");
    if (owner != u.id && owner != assignee)
      app.notify(owner, "SUBTASK_ADDED", "Sub-task added to your task " + jstr(parent, "key"), title + " - " + app.name_of(assignee), id);
    app.touch_task(parent_id);
  }
  app.audit(u.id, parent_id ? "create_subtask" : "create_task", "task", id, jstr(t, "key"));
  tx.commit();
  return json{{"ok", true}, {"id", id}, {"key", t["key"]}};
}

json Api::task_action(const json& t, const std::string& act, const json& b, const UserCtx& u) {
  require(app.can_view_task(u, t), 404, "Task not found");
  const int64_t id = jint(t, "id");
  const std::string st = jstr(t, "status");
  const bool assignee = jint(t, "assignee_id") == u.id;
  const bool manager = app.can_manage_task(u, t);
  const bool top_own = assignee && app.self_managed(u.id, jint(t, "dept_id"));  // nobody above: manages own work
  const std::string key = jstr(t, "key"), lbl = label(t);
  Tx tx(app.db);
  json result = {{"ok", true}};

  if (act == "start") {
    require(st == "TODO", 400, "Only tasks that are 'To do' can be started");
    require(assignee || manager, 403, "Only the assignee or their manager can start this task");
    app.db.exec("UPDATE tasks SET status='IN_PROGRESS' WHERE id=?", {id});
    app.add_event(id, u.id, "Started");
  } else if (act == "hold" || act == "resume") {
    bool hold = act == "hold";
    // work can be held before it starts too (waiting for parts, a decision...); resuming means working on it
    require(hold ? (st == "IN_PROGRESS" || st == "TODO") : st == "ON_HOLD", 400,
            hold ? "Only work that is to do or in progress can be put on hold" : "This task is not on hold");
    require(assignee || manager, 403, "Only the assignee or their manager can do that");
    std::string c = hold ? req_text(b, "comment", "Reason", 1000) : opt_text(b, "comment", 1000, "Comment");
    app.db.exec("UPDATE tasks SET status=? WHERE id=?", {hold ? "ON_HOLD" : "IN_PROGRESS", id});
    app.add_event(id, u.id, std::string(hold ? "Put on hold" : "Resumed") + (c.empty() ? "" : ": " + c));
    if (assignee) app.notify_all(app.managers_of(u.id, jint(t, "dept_id")), u.id, "STATUS_CHANGED",
                                 std::string(hold ? "Put on hold by " : "Resumed by ") + u.name + ": " + key, c.empty() ? jstr(t, "title") : c, id);
    else app.notify(jint(t, "assignee_id"), "STATUS_CHANGED", std::string(hold ? "Put on hold: " : "Resumed: ") + key, c.empty() ? jstr(t, "title") : c, id);
  } else if (act == "submit") {
    require(assignee, 403, "Only the person doing the task can mark it complete");
    require(kOpen.count(st) > 0, 400, st == "IN_REVIEW" ? "This task is already waiting for review" : "This task cannot be completed now");
    int open = open_subtasks(id);
    require(open == 0, 400, "Finish or cancel the " + std::to_string(open) + " open sub-task(s) first");
    bool self_sub = is_self_subtask(t);
    std::string note = self_sub ? opt_text(b, "note", 2000, "Completion note") : req_text(b, "note", "Completion note", 2000);
    std::string link = valid_link(b, "link");
    auto reviewers = app.reviewers_for(t);
    std::string now = now_iso();
    if (reviewers.empty() && !self_sub) {
      std::string why = app.missing_reviewer_reason(u.id, jint(t, "dept_id"));
      require(why.empty(), 400, why);
    }
    if (reviewers.empty()) {
      app.db.insert("INSERT INTO completions(task_id,submitted_by,note,link,status,decision_note,decided_at,created_at,seq) "
                    "VALUES(?,?,?,?,'APPROVED',?,?,?,?)",
                    {id, u.id, note, link, self_sub ? "Own sub-task" : "No reviewer above - closed automatically", now, now, app.next_seq()});
      app.db.exec("UPDATE tasks SET status='DONE', progress=100, submitted_at=?, completed_at=? WHERE id=?", {now, now, id});
      app.add_event(id, u.id, "Completed" + (note.empty() ? "" : ": " + note) + (link.empty() ? "" : "  [" + link + "]"));
      after_done(t, u);
      result["status"] = "DONE";
    } else {
      app.db.insert("INSERT INTO completions(task_id,submitted_by,note,link,status,created_at,seq) VALUES(?,?,?,?,'PENDING',?,?)",
                    {id, u.id, note, link, now, app.next_seq()});
      app.db.exec("UPDATE tasks SET status='IN_REVIEW', submitted_at=? WHERE id=?", {now, id});
      app.add_event(id, u.id, "Marked complete - waiting for review: " + note + (link.empty() ? "" : "  [" + link + "]"));
      app.notify_all(reviewers, u.id, "REVIEW_REQUESTED", "Please review: " + u.name + " completed " + key,
                     jstr(t, "title") + (note.empty() ? "" : " - " + note), id, 0, true);
      result["status"] = "IN_REVIEW";
      std::vector<std::string> names;
      for (auto r : reviewers) names.push_back(app.name_of(r));
      result["reviewers"] = names;
    }
  } else if (act == "withdraw") {
    require(assignee, 403, "Only the person who submitted can withdraw the request");
    require(st == "IN_REVIEW", 400, "There is no completion request to withdraw");
    app.db.exec("UPDATE completions SET status='WITHDRAWN', decided_at=?, seq=? WHERE task_id=? AND status='PENDING'",
                {now_iso(), app.next_seq(), id});
    app.db.exec("UPDATE tasks SET status='IN_PROGRESS', submitted_at='' WHERE id=?", {id});
    app.add_event(id, u.id, "Withdrew the completion request");
    app.notify_all(app.reviewers_for(t), u.id, "REVIEW_WITHDRAWN", "Completion request withdrawn: " + key,
                   u.name + " is still working on it", id);
  } else if (act == "approve" || act == "reject") {
    bool ok = act == "approve";
    require(st == "IN_REVIEW", 400, "This task is not waiting for review");
    require(manager, 403, "Only the manager of this task can review it");
    std::string note = ok ? opt_text(b, "note", 2000, "Note") : req_text(b, "note", "Comments for the team member", 2000);
    std::string now = now_iso();
    app.db.exec("UPDATE completions SET status=?, decided_by=?, decision_note=?, decided_at=?, seq=? WHERE task_id=? AND status='PENDING'",
                {ok ? "APPROVED" : "REJECTED", u.id, note, now, app.next_seq(), id});
    if (ok) {
      app.db.exec("UPDATE tasks SET status='DONE', progress=100, completed_at=? WHERE id=?", {now, id});
      app.add_event(id, u.id, "Completion approved" + (note.empty() ? "" : ": " + note));
      app.notify(jint(t, "assignee_id"), "TASK_APPROVED", "Approved as complete: " + key, note.empty() ? jstr(t, "title") : note, id, 0, true);
      after_done(t, u);
      result["status"] = "DONE";
    } else {
      app.db.exec("UPDATE tasks SET status='IN_PROGRESS', submitted_at='' WHERE id=?", {id});
      app.add_event(id, u.id, "Changes requested: " + note);
      app.notify(jint(t, "assignee_id"), "TASK_REJECTED", "Changes requested on " + key + " by " + u.name, note, id, 0, true);
      result["status"] = "IN_PROGRESS";
    }
  } else if (act == "reopen") {
    require(st == "DONE", 400, "Only completed tasks can be reopened");
    require(manager, 403, "Only the manager of this task can reopen it");
    std::string c = req_text(b, "comment", "Reason", 1000);
    if (jint(t, "parent_id")) {
      json p = app.get_task(jint(t, "parent_id"));
      require(kOpen.count(jstr(p, "status")) > 0, 400, "Reopen the main task " + jstr(p, "key") + " first");
    }
    app.db.exec("UPDATE tasks SET status='IN_PROGRESS', completed_at='', submitted_at='' WHERE id=?", {id});
    app.add_event(id, u.id, "Reopened: " + c);
    app.notify(jint(t, "assignee_id"), "STATUS_CHANGED", "Task reopened: " + key, c, id, 0, true);
    app.recompute_progress(id);
  } else if (act == "cancel") {
    require(kOpen.count(st) > 0 || st == "IN_REVIEW", 400, "This task is already finished");
    require(manager || top_own, 403, "Only the manager of this task can cancel it");
    std::string c = req_text(b, "comment", "Reason", 1000);
    std::string now = now_iso();
    app.db.exec("UPDATE completions SET status='WITHDRAWN', decided_at=?, seq=? WHERE task_id=? AND status='PENDING'", {now, app.next_seq(), id});
    close_pending_items(id, "The task was cancelled", u);
    app.db.exec("UPDATE tasks SET status='CANCELLED' WHERE id=?", {id});
    app.add_event(id, u.id, "Cancelled: " + c);
    std::set<int64_t> left = {jint(t, "assignee_id")};  // people who may have been in the project only for this work
    if (jint(t, "assignee_id") != u.id) app.notify(jint(t, "assignee_id"), "STATUS_CHANGED", "Task cancelled: " + key, c, id);
    app.notify_all(app.task_helpers(id), u.id, "STATUS_CHANGED", "Task cancelled: " + key, c, id);
    // whoever asked for this work (help / cross-team) learns that it will not be done
    app.notify_all(app.requester_side(t), u.id, "STATUS_CHANGED", "Cancelled: " + label(t), c, id, 0, true);
    for (auto& s : app.db.query("SELECT id, assignee_id, key FROM tasks WHERE parent_id=? AND status IN "
                                "('TODO','IN_PROGRESS','ON_HOLD','IN_REVIEW')", {id})) {
      app.db.exec("UPDATE completions SET status='WITHDRAWN', decided_at=?, seq=? WHERE task_id=? AND status='PENDING'", {now, app.next_seq(), s["id"]});
      close_pending_items(jint(s, "id"), "The main task " + key + " was cancelled", u);
      app.db.exec("UPDATE tasks SET status='CANCELLED' WHERE id=?", {s["id"]});
      app.add_event(jint(s, "id"), u.id, "Cancelled together with main task " + key);
      left.insert(jint(s, "assignee_id"));
      tell_waiting_tasks(app.get_task(jint(s, "id")), u);
      if (jint(s, "assignee_id") != u.id)
        app.notify(jint(s, "assignee_id"), "STATUS_CHANGED", "Sub-task cancelled: " + jstr(s, "key"), "Main task " + key + " was cancelled", jint(s, "id"));
      app.touch_task(jint(s, "id"));
    }
    if (jint(t, "parent_id")) app.recompute_progress(jint(t, "parent_id"));
    tell_waiting_tasks(app.get_task(id), u);
    for (auto who : left) app.release_auto_member(jint(t, "project_id"), who);
  }
  app.touch_task(id);
  if (act == "approve" || act == "submit" || act == "reopen" || act == "cancel") app.recompute_progress(id);
  app.audit(u.id, "task_" + act, "task", id, st);
  tx.commit();
  return result;
}

// follow-ups once a task is DONE: tell the requester, refresh the parent, nudge the parent owner
// a finished or cancelled task leaves nothing to decide: pending extensions and help requests on it are closed
void Api::close_pending_items(int64_t task_id, const std::string& why, const UserCtx& u) {
  std::string now = now_iso();
  app.db.exec("UPDATE extensions SET status='REJECTED', decided_by=?, decision_note=?, decided_at=?, seq=? WHERE task_id=? AND status='PENDING'",
              {u.id, "Closed automatically: " + why, now, app.next_seq(), task_id});
  app.db.exec("UPDATE work_requests SET status='REJECTED', decided_by=?, decision_note=?, decided_at=?, seq=? "
              "WHERE type='TEAM_HELP' AND parent_task_id=? AND status='PENDING'",
              {u.id, "Closed automatically: " + why, now, app.next_seq(), task_id});
}

void Api::after_done(const json& t, const UserCtx& u) {
  int64_t id = jint(t, "id");
  close_pending_items(id, "the task was completed", u);
  std::set<int64_t> told = app.requester_side(t);
  if (jint(t, "parent_id")) {
    int64_t pid = jint(t, "parent_id");
    json p = app.get_task(pid);
    app.add_event(pid, u.id, "Sub-task " + jstr(t, "key") + " completed");
    app.recompute_progress(pid);
    int64_t owner = jint(p, "assignee_id");
    if (open_subtasks(pid) == 0 && kOpen.count(jstr(p, "status")) && owner != u.id) {
      told.erase(owner);  // one message for the owner: the summary below covers this sub-task too
      app.notify(owner, "SUBTASKS_DONE", "All sub-tasks of " + jstr(p, "key") + " are done",
                 "You can now mark " + jstr(p, "title") + " complete", pid);
    }
  }
  app.notify_all(told, u.id, "TASK_COMPLETED", "Completed: " + label(t), "Done by " + app.name_of(jint(t, "assignee_id")), id, 0, true);
  app.notify_all(app.task_helpers(id), u.id, "TASK_COMPLETED", "Completed: " + label(t), "Thank you for helping", id);
  tell_waiting_tasks(t, u);
  repeat_task(t, u);
}

// ================================================================== v1.1: helpers, dependencies, repeating, mentions
std::string Api::recur_of(const json& b, const std::string& def) {
  if (!jhas(b, "recur")) return def;
  std::string r = upper(jstr(b, "recur"));
  if (r == "NONE") r.clear();
  require(r.empty() || r == "DAILY" || r == "WEEKLY" || r == "MONTHLY", 400, "Repeat must be none, daily, weekly or monthly");
  return r;
}

std::vector<int64_t> Api::id_list(const json& b, const char* key, size_t max, const std::string& what) {
  const json& a = b[key];
  require(a.is_array(), 400, "The list of " + what + " is not valid");
  require(a.size() <= max, 400, "At most " + std::to_string(max) + " " + what);
  std::vector<int64_t> v;
  std::set<int64_t> seen;
  for (auto& x : a) {
    require(x.is_number_integer() && x.get<int64_t>() > 0, 400, "The list of " + what + " is not valid");
    if (seen.insert(x.get<int64_t>()).second) v.push_back(x.get<int64_t>());
  }
  return v;
}

void Api::set_helpers(const json& t, std::vector<int64_t> want, const UserCtx& u) {
  const int64_t id = jint(t, "id");
  std::set<int64_t> cur = app.task_helpers(id), next;
  for (auto x : want)
    if (x != jint(t, "assignee_id")) next.insert(x);  // the person responsible is not also "helping"
  for (auto x : next) {
    if (cur.count(x)) continue;
    require(is_active_user(x), 400, app.name_of(x) + " is not an active user");
    int64_t unit = pick_unit(u, x, 0);
    require(app.has_authority(u, x, unit) || (x == u.id && u.leads(unit)), 403,
            "You can only add people you manage - " + app.name_of(x) + " is not in your team");
    json m = app.db.one("SELECT id FROM task_members WHERE task_id=? AND user_id=?", {id, x});
    if (m.is_null())
      app.db.insert("INSERT INTO task_members(task_id,user_id,active,added_by,created_at,seq) VALUES(?,?,1,?,?,?)",
                    {id, x, u.id, now_iso(), app.next_seq()});
    else
      app.db.exec("UPDATE task_members SET active=1, added_by=?, seq=? WHERE id=?", {u.id, app.next_seq(), m["id"]});
    app.add_event(id, u.id, app.name_of(x) + " also works on this task");
    // the task, its main task and the other sub-tasks reach them (no new version: nobody's edit is refused for this)
    for (auto r : app.related_tasks(id)) app.db.exec("UPDATE tasks SET seq=? WHERE id=?", {app.next_seq(), r});
    if (x != u.id)
      app.notify(x, "HELPER_ADDED", "You also work on " + jstr(t, "key"), jstr(t, "title") + " - " + app.name_of(jint(t, "assignee_id")) +
                 " is responsible", id, 0, true);
  }
  for (auto x : cur) {
    if (next.count(x)) continue;
    app.db.exec("UPDATE task_members SET active=0, seq=? WHERE task_id=? AND user_id=?", {app.next_seq(), id, x});
    app.add_event(id, u.id, app.name_of(x) + " no longer works on this task");
    app.bump_scope(x);  // they may lose sight of it: their app downloads everything again
  }
}

bool Api::waits_for(int64_t from, int64_t target) {
  std::set<int64_t> seen = {from};
  std::vector<int64_t> todo = {from};
  while (!todo.empty() && seen.size() < 2000) {
    int64_t x = todo.back();
    todo.pop_back();
    // x cannot finish before the tasks it waits for, nor (as a main task) before its own sub-tasks
    for (auto& r : app.db.query("SELECT depends_on AS n FROM task_deps WHERE task_id=? AND active=1 "
                                "UNION SELECT id AS n FROM tasks WHERE parent_id=? AND status NOT IN ('CANCELLED','REJECTED')", {x, x})) {
      int64_t d = jint(r, "n");
      if (d == target) return true;
      if (seen.insert(d).second) todo.push_back(d);
    }
  }
  return false;
}

void Api::set_deps(const json& t, const std::vector<int64_t>& want_in, const UserCtx& u) {
  const int64_t id = jint(t, "id");
  auto vis = app.build_vis(u);
  std::set<int64_t> cur;
  for (auto& r : app.db.query("SELECT depends_on FROM task_deps WHERE task_id=? AND active=1", {id})) cur.insert(jint(r, "depends_on"));
  // what the editor cannot see stays as it is (they could neither have chosen nor removed it)
  std::vector<int64_t> want;
  for (auto d : want_in)
    if (!cur.count(d)) want.push_back(d);
  std::set<int64_t> next(want_in.begin(), want_in.end());
  for (auto d : cur) {
    json o = app.db.one("SELECT * FROM tasks WHERE id=?", {d});
    if (!o.is_null() && !app.can_view_task(u, o, vis)) next.insert(d);
  }
  for (auto d : want) {
    require(d != id, 400, "A task cannot wait for itself");
    json o = app.db.one("SELECT * FROM tasks WHERE id=?", {d});
    require(!o.is_null() && app.can_view_task(u, o, vis), 400, "One of the tasks to wait for does not exist");
    const std::string ost = jstr(o, "status");
    require(ost != "PROPOSED" && ost != "REJECTED" && ost != "CANCELLED", 400,
            jstr(o, "key") + " is " + (ost == "PROPOSED" ? "only proposed" : "not going ahead") + " - a task can only wait for real work");
    require(jint(o, "project_id") == jint(t, "project_id"), 400, jstr(o, "key") + " is in another project - a task can only wait for tasks of its own project");
    require(jint(o, "parent_id") != id && jint(t, "parent_id") != d, 400, "A main task and its own sub-task cannot wait for each other");
    require(!waits_for(d, id), 400, jstr(o, "key") + " already waits for this task - that would be a circle");
  }
  for (auto d : next) {
    if (cur.count(d)) continue;
    json m = app.db.one("SELECT id FROM task_deps WHERE task_id=? AND depends_on=?", {id, d});
    if (m.is_null())
      app.db.insert("INSERT INTO task_deps(task_id,depends_on,active,created_at,seq) VALUES(?,?,1,?,?)", {id, d, now_iso(), app.next_seq()});
    else
      app.db.exec("UPDATE task_deps SET active=1, seq=? WHERE id=?", {app.next_seq(), m["id"]});
    app.add_event(id, u.id, "Waits for " + jstr(app.get_task(d), "key"));  // the key only: not everyone may open that task
  }
  for (auto d : cur)
    if (!next.count(d)) {
      app.db.exec("UPDATE task_deps SET active=0, seq=? WHERE task_id=? AND depends_on=?", {app.next_seq(), id, d});
      app.add_event(id, u.id, "No longer waits for " + jstr(app.get_task(d), "key"));
    }
}

json Api::visible_deps(const json& rows, const UserCtx& u) {
  auto vis = app.build_vis(u);
  std::map<int64_t, json> cache;
  auto get = [&](int64_t tid) -> const json& {
    auto it = cache.find(tid);
    if (it == cache.end()) it = cache.emplace(tid, app.db.one("SELECT * FROM tasks WHERE id=?", {tid})).first;
    return it->second;
  };
  json out = json::array();
  for (auto r : rows) {
    const json& a = get(jint(r, "task_id"));
    if (a.is_null() || !app.can_view_task(u, a, vis)) continue;
    const json& d = get(jint(r, "depends_on"));
    if (d.is_null() || !app.can_view_task(u, d, vis)) {
      std::string st = d.is_null() ? "CANCELLED" : jstr(d, "status");
      r["depends_on"] = 0;
      r["hidden"] = 1;
      r["done"] = (st == "DONE" || st == "CANCELLED" || st == "REJECTED") ? 1 : 0;
    }
    out.push_back(r);
  }
  return out;
}

void Api::tell_waiting_tasks(const json& done, const UserCtx& u) {
  for (auto& r : app.db.query("SELECT s.* FROM task_deps d JOIN tasks s ON s.id=d.task_id WHERE d.depends_on=? AND d.active=1 "
                              "AND s.status IN ('TODO','IN_PROGRESS','ON_HOLD')", {done["id"]})) {
    int64_t sid = jint(r, "id");
    const bool finished = jstr(done, "status") == "DONE";
    app.add_event(sid, u.id, jstr(done, "key") + " (waited for) " + (finished ? "is done" : "will not be done - it was cancelled"));
    int64_t still = jint(app.db.one("SELECT COUNT(*) AS n FROM task_deps d JOIN tasks p ON p.id=d.depends_on WHERE d.task_id=? "
                                    "AND d.active=1 AND p.status NOT IN ('DONE','CANCELLED','REJECTED')", {sid}), "n");
    if (still == 0) {
      std::set<int64_t> who = app.task_helpers(sid);
      who.insert(jint(r, "assignee_id"));
      app.notify_all(who, u.id, "CAN_START", "You can start " + jstr(r, "key"),
                     jstr(done, "key") + (finished ? " is done: " : " was cancelled: ") + jstr(r, "title"), sid, 0, true);
    }
    app.touch_task(sid);
  }
}

void Api::repeat_task(const json& t, const UserCtx& u) {
  const std::string recur = jstr(t, "recur");
  const std::string due = jstr(t, "due_date");
  if (recur.empty() || !valid_date(due)) return;
  json p = project_row(jint(t, "project_id"));
  if (jstr(p, "status") != "ACTIVE" && jstr(p, "status") != "PLANNED") {  // project paused or closed: the series stops
    app.add_event(jint(t, "id"), u.id, "Repeats no more: the project is " + std::string(jstr(p, "status") == "CLOSED" ? "closed" : "on hold"));
    return;
  }
  if (jint(t, "parent_id")) return;  // only main tasks repeat (a repeating sub-task would keep its main task open)
  if (!is_active_user(jint(t, "assignee_id"))) {
    app.add_event(jint(t, "id"), u.id, "Repeats no more: " + app.name_of(jint(t, "assignee_id")) + " is no longer an active user");
    return;
  }
  const std::string today = today_local();
  const int anchor = (int)jint(t, "recur_day");
  std::string next = next_occurrence(due, recur, anchor);
  if (next < today) {  // finished late: the series continues from today, on its own rhythm
    if (recur == "DAILY") next = today;
    else if (recur == "WEEKLY") next = add_days(next, 7 * ((days_between(next, today) + 6) / 7));
    else
      for (int guard = 0; next < today && guard < 2400; guard++) next = next_occurrence(next, recur, anchor);
  }
  std::string start;
  if (valid_date(jstr(t, "start_date"))) start = add_days(next, -days_between(jstr(t, "start_date"), due));
  json f = {{"project_id", t["project_id"]}, {"title", t["title"]}, {"description", t["description"]}, {"dept_id", t["dept_id"]},
            {"assignee_id", t["assignee_id"]}, {"created_by", t["created_by"]}, {"origin", jstr(t, "origin", "MANAGER")},
            {"priority", t["priority"]}, {"status", "TODO"}, {"start_date", start}, {"due_date", next}, {"recur", recur}, {"recur_day", anchor},
            {"is_milestone", t["is_milestone"]}};
  for (const char* k : {"parent_id", "requesting_dept_id", "requested_by"})
    if (jint(t, k)) f[k] = jint(t, k);
  int64_t nid = app.create_task(f);
  for (auto h : app.task_helpers(jint(t, "id")))
    if (is_active_user(h))
      app.db.insert("INSERT INTO task_members(task_id,user_id,active,added_by,created_at,seq) VALUES(?,?,1,?,?,?)",
                    {nid, h, u.id, now_iso(), app.next_seq()});
  json n = app.get_task(nid);
  std::string word = recur == "DAILY" ? "daily" : recur == "WEEKLY" ? "weekly" : "monthly";
  app.add_event(jint(t, "id"), u.id, "Repeats " + word + ": next is " + jstr(n, "key") + ", due " + hdate(next));
  app.add_event(nid, u.id, "Created automatically (repeats " + word + ") after " + jstr(t, "key") + " was completed");
  app.db.exec("UPDATE tasks SET recur='' WHERE id=?", {t["id"]});  // the series continues from the new task only
  if (jint(t, "assignee_id") != u.id)  // (closing your own: you see the next one straight away)
    app.notify(jint(t, "assignee_id"), "TASK_ASSIGNED", "Next " + word + " task: " + jstr(n, "key"), jstr(n, "title") + " - due " + hdate(next), nid);
}

std::set<int64_t> Api::mentioned_in(const std::string& body, const json& t) {
  std::set<int64_t> out;
  if (body.find('@') == std::string::npos) return out;
  auto lower = [](std::string s) {
    for (auto& c : s) c = (char)tolower((unsigned char)c);
    return s;
  };
  std::string work = lower(body);
  // longest names first, so "@Amit Kumar Singh" is not also read as "@Amit Kumar"
  json users = app.db.query("SELECT id, name FROM users WHERE status='ACTIVE' ORDER BY LENGTH(name) DESC");
  for (auto& r : users) {
    std::string pat = "@" + lower(jstr(r, "name"));
    if (pat.size() < 3) continue;
    for (size_t at = work.find(pat); at != std::string::npos; at = work.find(pat, at + 1)) {
      size_t end = at + pat.size();
      if (end < work.size() && (isalnum((unsigned char)work[end]) || work[end] == '_')) continue;  // part of a longer word
      work.replace(at, pat.size(), std::string(pat.size(), ' '));
      int64_t uid = jint(r, "id");
      if (app.user_can_view_task(uid, t)) out.insert(uid);
      break;
    }
  }
  return out;
}

}  // namespace tf::api
