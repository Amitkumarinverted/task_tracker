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
      project = jint(p, "id");
    }
    bool reassign = jhas(b, "assignee_id") && jint(b, "assignee_id") != jint(t, "assignee_id");
    bool moved = project != jint(t, "project_id");
    Tx tx(app.db);
    // who sees this task (and its parent, siblings, sub-tasks) now - to tell those who lose access afterwards
    std::map<int64_t, std::set<int64_t>> viewers_before;
    std::set<int64_t> related, candidates;
    if (reassign || moved) {
      related = app.related_tasks(id);
      candidates = app.viewer_candidates(related);  // only these people can have seen the task before
      viewers_before = app.task_viewers(related, candidates);
    }
    app.db.exec("UPDATE tasks SET title=?,description=?,priority=?,start_date=?,due_date=?,project_id=? WHERE id=?",
                {title, desc, pri, start, due, project, id});
    if (moved) {  // sub-tasks always live in their main task's project
      app.db.exec("UPDATE tasks SET project_id=? WHERE parent_id=?", {project, id});
      for (auto& sub : app.db.query("SELECT id FROM tasks WHERE parent_id=?", {id})) app.touch_task(jint(sub, "id"));
      app.add_event(id, u->id, "Moved to project " + jstr(project_row(project), "name"));
    }
    if (due != jstr(t, "due_date")) {
      app.add_event(id, u->id, "Due date changed " + hdate(jstr(t, "due_date")) + " -> " + hdate(due));
      if (jint(t, "assignee_id") != u->id)
        app.notify(jint(t, "assignee_id"), "DUE_CHANGED", "Due date changed: " + jstr(t, "key"), "New due date " + hdate(due), id, 0, true);
      app.notify_all(app.requester_side(t), u->id, "DUE_CHANGED", "Due date changed: " + jstr(t, "key"), "New due date " + hdate(due), id);
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
      if (na != u->id) app.notify(na, "TASK_ASSIGNED", "Task assigned to you: " + jstr(t, "key"), title, id, 0, true);
      app.notify(jint(t, "assignee_id"), "STATUS_CHANGED", "Task reassigned: " + jstr(t, "key"), "Now handled by " + app.name_of(na), id);
    }
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
                {"extensions", app.db.query("SELECT * FROM extensions WHERE task_id=? ORDER BY id", {t["id"]})}};
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
    if (jint(t, "parent_id")) to.insert(jint(app.db.one("SELECT assignee_id FROM tasks WHERE id=?", {t["parent_id"]}), "assignee_id"));
    for (auto& c : app.db.query("SELECT DISTINCT author_id FROM comments WHERE task_id=? AND kind='COMMENT'", {t["id"]}))
      to.insert(jint(c, "author_id"));
    std::set<int64_t> allowed;  // never send comment text to someone who can no longer open the task
    for (auto x : to)
      if (x && x != u->id && app.user_can_view_task(x, t)) allowed.insert(x);
    std::string snippet = body.size() > 140 ? body.substr(0, 137) + "..." : body;
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
  json f = {{"title", title}, {"description", opt_text(b, "description", 5000, "Description")}, {"assignee_id", assignee},
            {"created_by", u.id}, {"priority", priority_of(b)}, {"status", "TODO"}, {"start_date", start}, {"due_date", due}};
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
    require(st == (hold ? "IN_PROGRESS" : "ON_HOLD"), 400, hold ? "Only tasks in progress can be put on hold" : "This task is not on hold");
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
    if (jint(t, "assignee_id") != u.id) app.notify(jint(t, "assignee_id"), "STATUS_CHANGED", "Task cancelled: " + key, c, id);
    // whoever asked for this work (help / cross-team) learns that it will not be done
    app.notify_all(app.requester_side(t), u.id, "STATUS_CHANGED", "Cancelled: " + label(t), c, id, 0, true);
    for (auto& s : app.db.query("SELECT id, assignee_id, key FROM tasks WHERE parent_id=? AND status IN "
                                "('TODO','IN_PROGRESS','ON_HOLD','IN_REVIEW')", {id})) {
      app.db.exec("UPDATE completions SET status='WITHDRAWN', decided_at=?, seq=? WHERE task_id=? AND status='PENDING'", {now, app.next_seq(), s["id"]});
      close_pending_items(jint(s, "id"), "The main task " + key + " was cancelled", u);
      app.db.exec("UPDATE tasks SET status='CANCELLED' WHERE id=?", {s["id"]});
      app.add_event(jint(s, "id"), u.id, "Cancelled together with main task " + key);
      if (jint(s, "assignee_id") != u.id)
        app.notify(jint(s, "assignee_id"), "STATUS_CHANGED", "Sub-task cancelled: " + jstr(s, "key"), "Main task " + key + " was cancelled", jint(s, "id"));
      app.touch_task(jint(s, "id"));
    }
    if (jint(t, "parent_id")) app.recompute_progress(jint(t, "parent_id"));
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
}

}  // namespace tf::api
