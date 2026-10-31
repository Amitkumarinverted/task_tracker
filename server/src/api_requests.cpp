// Inverted TaskFlow Server - work request endpoints (proposals, help requests, cross-team requests)
#include "api.h"

namespace tf::api {

// ================================================================== work requests
void Api::install_requests() {
  route("POST", "/requests", true, [this](const Req&, const json& b, UserCtx* u) { return create_request(b, *u); });
  route("POST", R"(/requests/(\d+)/(approve|reject|withdraw))", true, [this](const Req& r, const json& b, UserCtx* u) {
    json q = app.db.one("SELECT * FROM work_requests WHERE id=?", {id_arg(r)});
    require(!q.is_null(), 404, "Request not found");
    require(jstr(q, "status") == "PENDING", 400, "This request was already decided");
    std::string act = r.matches[2].str();
    if (act == "approve") return approve_request(q, b, *u);
    if (act == "reject") return reject_request(q, b, *u);
    return withdraw_request(q, *u);
  });
}

json Api::create_request(const json& b, const UserCtx& u) {
  const std::string type = upper(jstr(b, "type"));
  const std::string now = now_iso();
  if (type == "SELF_PROPOSAL") {
    auto mine = units_of(u.id);
    require(!mine.empty(), 400, "You must belong to a department to propose tasks");
    int64_t unit = jint(b, "dept_id") ? jint(b, "dept_id") : mine.front();
    require(u.member_of(unit), 403, "You are not a member of that department");
    auto mgrs = app.managers_of(u.id, unit);
    if (mgrs.empty()) {
      std::string why = app.missing_reviewer_reason(u.id, unit);
      throw ApiError(400, why.empty() ? "Nobody above you approves proposals - create the task directly" : why);
    }
    json p = project_row(jint(b, "project_id"));
    check_project_open(p);
    require(app.can_use_project(u, p), 403, "You are not part of project " + jstr(p, "name") + " - ask its owner to add you");
    std::string title = req_text(b, "title", "Title");
    std::string start = opt_date(b, "start_date", "Start date");
    std::string due = req_date(b, "due_date", "Due date");
    require(start.empty() || start <= due, 400, "Start date must be on or before the due date");
    std::string pri = priority_of(b);
    std::string desc = opt_text(b, "description", 5000, "Description");
    Tx tx(app.db);
    int64_t rid = app.db.insert(
        "INSERT INTO work_requests(type,status,project_id,title,description,requested_by,requesting_dept_id,target_dept_id,"
        "needed_by,priority,created_at,seq) VALUES('SELF_PROPOSAL','PENDING',?,?,?,?,?,?,?,?,?,?)",
        {p["id"], title, desc, u.id, unit, unit, due, pri, now, app.next_seq()});
    int64_t tid = app.create_task({{"project_id", p["id"]}, {"title", title}, {"description", desc}, {"dept_id", unit},
                                   {"assignee_id", u.id}, {"created_by", u.id}, {"origin", "SELF"}, {"request_id", rid},
                                   {"priority", pri}, {"status", "PROPOSED"}, {"start_date", start}, {"due_date", due}});
    app.db.exec("UPDATE work_requests SET resulting_task_id=? WHERE id=?", {tid, rid});
    app.add_event(tid, u.id, "Proposed by " + u.name);
    app.notify_all(mgrs, u.id, "PROPOSAL", "Task proposal from " + u.name, title, tid, rid, true);
    app.audit(u.id, "propose_task", "request", rid, title);
    tx.commit();
    return json{{"ok", true}, {"id", rid}, {"task_id", tid}};
  }
  if (type == "TEAM_HELP") {
    json task = app.get_task(jint(b, "parent_task_id"));
    require(app.can_view_task(u, task), 404, "Task not found");
    require(jint(task, "assignee_id") == u.id, 403, "You can ask for help on your own tasks");
    require(kOpen.count(jstr(task, "status")) > 0, 400, "Help can only be requested on open tasks");
    int64_t unit = jint(task, "dept_id");
    int64_t target = unit;  // who decides: the manager of the team - or of the department when the colleague is in another of its teams
    int64_t sugg = jint(b, "suggested_assignee_id");
    if (sugg) {
      require(sugg != u.id, 400, "Suggest a colleague other than yourself");
      require(is_active_user(sugg), 400, "The suggested colleague is not an active user");
      int64_t top = top_unit(unit);
      auto dept = app.subtree(top), team = app.subtree(unit);
      int64_t hunit = 0;
      for (auto x : units_of(sugg))
        if (team.count(x)) { hunit = x; break; }
      if (!hunit)
        for (auto x : units_of(sugg))
          if (dept.count(x)) { hunit = x; break; }
      require(hunit != 0, 400, app.name_of(sugg) + " is not in " + app.unit_label(top) +
                                   " - for other departments use 'Request work from another team'");
      if (!team.count(hunit)) target = top;  // another sub-department of the same department
    }
    auto mgrs = app.managers_of(u.id, target);
    require(!mgrs.empty(), 400, "Nobody above you approves help requests - add a sub-task for your team directly");
    std::string title = req_text(b, "title", "What help is needed");
    std::string needed = opt_date(b, "needed_by", "Needed-by date");
    Tx tx(app.db);
    int64_t rid = app.db.insert(
        "INSERT INTO work_requests(type,status,project_id,title,description,requested_by,requesting_dept_id,target_dept_id,"
        "parent_task_id,suggested_assignee_id,needed_by,priority,created_at,seq) VALUES('TEAM_HELP','PENDING',?,?,?,?,?,?,?,?,?,?,?,?)",
        {task["project_id"], title, opt_text(b, "description", 5000, "Description"), u.id, unit, target, task["id"],
         sugg ? json(sugg) : json(), needed, priority_of(b, jstr(task, "priority")), now, app.next_seq()});
    app.add_event(jint(task, "id"), u.id, "Asked for help: " + title + (sugg ? " (suggested " + app.name_of(sugg) + ")" : ""));
    app.notify_all(mgrs, u.id, "HELP_REQUEST", "Help requested by " + u.name + " on " + jstr(task, "key"), title, jint(task, "id"), rid, true);
    if (target != unit) {  // the department decides; the asking team's own managers are kept informed
      std::set<int64_t> fyi;
      for (auto m : app.managers_of(u.id, unit))
        if (!mgrs.count(m)) fyi.insert(m);
      app.notify_all(fyi, u.id, "HELP_ASKED", u.name + " asked " + (sugg ? app.name_of(sugg) : std::string("a colleague")) + " for help",
                     jstr(task, "key") + "  " + title + " - " + app.unit_label(top_unit(unit)) + " decides", jint(task, "id"));
    }
    app.touch_task(jint(task, "id"));
    app.audit(u.id, "request_help", "request", rid, title);
    tx.commit();
    return json{{"ok", true}, {"id", rid}};
  }
  if (type == "CROSS_TEAM") {
    int64_t target = jint(b, "target_dept_id");
    unit_row(target);
    auto mine = units_of(u.id);
    int64_t from = jint(b, "dept_id") ? jint(b, "dept_id") : (mine.empty() ? 0 : mine.front());
    if (from) require(u.member_of(from), 403, "You are not a member of that department");
    require(from || u.sees_all(), 400, "You must belong to a department to raise requests");
    require(target != from, 400, "For work inside your own team, ask your manager or use 'Ask a colleague'");
    json p = project_row(jint(b, "project_id"));
    check_project_open(p);
    require(app.can_use_project(u, p), 403, "You are not part of project " + jstr(p, "name") + " - choose a project of your own");
    std::string title = req_text(b, "title", "Title");
    std::string needed = req_date(b, "needed_by", "Needed-by date");
    json q = {{"type", "CROSS_TEAM"}, {"target_dept_id", target}};
    auto approvers = request_approvers(q);
    require(!approvers.empty(), 400, app.unit_label(target) + " has no head yet - ask the administrator to assign one");
    Tx tx(app.db);
    int64_t rid = app.db.insert(
        "INSERT INTO work_requests(type,status,project_id,title,description,requested_by,requesting_dept_id,target_dept_id,"
        "needed_by,priority,created_at,seq) VALUES('CROSS_TEAM','PENDING',?,?,?,?,?,?,?,?,?,?)",
        {p["id"], title, opt_text(b, "description", 5000, "Description"), u.id, from ? json(from) : json(), target, needed,
         priority_of(b), now, app.next_seq()});
    std::string who = from ? u.name + " (" + app.unit_label(from) + ")" : u.name + " (Management)";
    app.notify_all(approvers, u.id, "CROSS_TEAM_REQUEST", "Request from " + who, title, 0, rid, true);
    app.audit(u.id, "request_cross_team", "request", rid, title);
    tx.commit();
    return json{{"ok", true}, {"id", rid}};
  }
  throw ApiError(400, "Unknown request type");
}

json Api::approve_request(const json& q, const json& b, const UserCtx& u) {
  require(can_decide_request(u, q), 403, "Only the responsible manager can approve this request");
  const std::string type = jstr(q, "type");
  const int64_t rid = jint(q, "id");
  const std::string note = opt_text(b, "note", 1000, "Note");
  Tx tx(app.db);
  int64_t tid = 0;
  if (type == "SELF_PROPOSAL") {
    json t = app.get_task(jint(q, "resulting_task_id"));
    tid = jint(t, "id");
    std::string title = jhas(b, "title") ? req_text(b, "title", "Title") : jstr(t, "title");
    std::string desc = jhas(b, "description") ? opt_text(b, "description", 5000, "Description") : jstr(t, "description");
    std::string pri = jhas(b, "priority") ? priority_of(b) : jstr(t, "priority");
    std::string start = jhas(b, "start_date") ? opt_date(b, "start_date", "Start date") : jstr(t, "start_date");
    std::string due = jhas(b, "due_date") ? req_date(b, "due_date", "Due date") : jstr(t, "due_date");
    require(start.empty() || start <= due, 400, "Start date must be on or before the due date");
    int64_t project = jint(t, "project_id");
    if (jhas(b, "project_id") && jint(b, "project_id") != project) {
      json p = project_row(jint(b, "project_id"));
      check_project_open(p);
      require(app.can_use_project(u, p), 403, "You are not part of project " + jstr(p, "name") + " - ask its owner to add you");
      project = jint(p, "id");
    }
    check_project_open(project_row(project));
    int64_t assignee = jint(b, "assignee_id", jint(t, "assignee_id"));
    int64_t unit = jint(t, "dept_id");
    std::set<int64_t> related = app.related_tasks(tid), candidates = app.viewer_candidates(related);
    auto viewers_before = app.task_viewers(related, candidates);
    if (assignee != jint(t, "assignee_id")) {
      require(is_active_user(assignee), 400, "Choose an active person");
      unit = pick_unit(u, assignee, 0);
      require(app.has_authority(u, assignee, unit), 403, "You can only assign work to people you manage");
    }
    app.db.exec("UPDATE tasks SET title=?,description=?,priority=?,start_date=?,due_date=?,original_due=?,project_id=?,"
                "assignee_id=?,dept_id=?,status='TODO' WHERE id=?",
                {title, desc, pri, start, due, due, project, assignee, unit, tid});
    app.add_event(tid, u.id, "Proposal approved by " + u.name + (note.empty() ? "" : ": " + note));
    app.ensure_project_member(project, assignee, u.id);
    app.touch_task(tid);
    app.record_revocations(viewers_before, app.task_viewers(related, candidates));  // e.g. given to another team
    app.notify(jint(q, "requested_by"), "PROPOSAL_APPROVED", "Your proposal was approved: " + jstr(t, "key"), title, tid, rid, true);
    if (assignee != jint(q, "requested_by")) app.notify(assignee, "TASK_ASSIGNED", "New task for you: " + jstr(t, "key"), title, tid, 0, true);
  } else {
    int64_t assignee = jint(b, "assignee_id");
    require(assignee > 0 && is_active_user(assignee), 400, "Choose who will do this work");
    if (type == "TEAM_HELP") require(assignee != jint(q, "requested_by"), 400, "Choose someone other than the person asking for help");
    int64_t want = jint(b, "dept_id");
    if (!want && type == "CROSS_TEAM") {  // someone in several teams: count it as work of the requested team
      auto st = app.subtree(jint(q, "target_dept_id"));
      for (auto x : units_of(assignee))
        if (st.count(x)) { want = x; break; }
    }
    int64_t unit = pick_unit(u, assignee, want);
    bool allowed = app.has_authority(u, assignee, unit) || (assignee == u.id && u.leads(unit));
    require(allowed, 403, "You can only assign work to people you manage");
    if (type == "CROSS_TEAM") {
      auto st = app.subtree(jint(q, "target_dept_id"));
      require(st.count(unit) > 0, 400, "Assign the request to someone in " + app.unit_label(jint(q, "target_dept_id")));
    }
    std::string due = jhas(b, "due_date") ? req_date(b, "due_date", "Due date") : jstr(q, "needed_by");
    require(valid_date(due), 400, "Due date is required");
    if (type == "CROSS_TEAM") check_project_open(project_row(jint(q, "project_id")));  // closed meanwhile?
    std::string start = opt_date(b, "start_date", "Start date");
    require(start.empty() || start <= due, 400, "Start date must be on or before the due date");
    json f = {{"project_id", q["project_id"]},
              {"title", jhas(b, "title") ? req_text(b, "title", "Title") : jstr(q, "title")},
              {"description", jhas(b, "description") ? opt_text(b, "description", 5000, "Description") : jstr(q, "description")},
              {"dept_id", unit}, {"assignee_id", assignee}, {"created_by", u.id}, {"requested_by", q["requested_by"]},
              {"request_id", rid}, {"priority", jhas(b, "priority") ? priority_of(b) : jstr(q, "priority", "MEDIUM")},
              {"status", "TODO"}, {"start_date", start}, {"due_date", due}};
    if (type == "TEAM_HELP") {
      json parent = app.get_task(jint(q, "parent_task_id"));
      int64_t top = jint(parent, "parent_id") ? jint(parent, "parent_id") : jint(parent, "id");
      json topt = app.get_task(top);
      require(kOpen.count(jstr(topt, "status")) > 0, 400, "The task that needs help is no longer open");
      f["origin"] = "HELP";
      f["parent_id"] = top;
      f["project_id"] = topt["project_id"];
      if (jint(q, "requesting_dept_id") && jint(q, "requesting_dept_id") != unit)
        f["requesting_dept_id"] = q["requesting_dept_id"];  // help from another team: the asking team's heads keep sight of it
      if (!jstr(topt, "due_date").empty() && due > jstr(topt, "due_date")) f["due_date"] = topt["due_date"];
    } else {
      f["origin"] = "CROSS_TEAM";
      f["requesting_dept_id"] = q["requesting_dept_id"];
    }
    tid = app.create_task(f);
    json t = app.get_task(tid);
    app.add_event(tid, u.id, std::string(type == "TEAM_HELP" ? "Help task" : "Cross-team task") + " approved by " + u.name +
                                 " and assigned to " + app.name_of(assignee));
    if (type == "TEAM_HELP") {
      app.add_event(jint(t, "parent_id"), u.id, "Help approved: " + app.name_of(assignee) + " assigned (" + jstr(t, "key") + ")");
      app.touch_task(jint(t, "parent_id"));
      if (jint(q, "requesting_dept_id") && jint(q, "requesting_dept_id") != unit) {  // the helper's own head learns where their person helps
        app.notify_all(app.leaders(unit), u.id, "HELP_GIVEN", app.name_of(assignee) + " helps " + app.unit_label(jint(q, "requesting_dept_id")),
                       jstr(t, "key") + "  " + jstr(t, "title"), tid);
        // ... and so do the asking team's managers, when the department decided
        if (jint(q, "target_dept_id") != jint(q, "requesting_dept_id")) {
          std::set<int64_t> fyi = app.managers_of(jint(q, "requested_by"), jint(q, "requesting_dept_id"));
          for (auto l : app.leaders(unit)) fyi.erase(l);
          app.notify_all(fyi, u.id, "HELP_GIVEN", app.name_of(assignee) + " will help " + app.name_of(jint(q, "requested_by")),
                         jstr(t, "key") + "  " + jstr(t, "title"), tid);
        }
      }
    }
    app.notify(jint(q, "requested_by"), "REQUEST_APPROVED", "Request approved - " + app.name_of(assignee) + " will do it", jstr(q, "title"), tid, rid, true);
    if (assignee != u.id) app.notify(assignee, "TASK_ASSIGNED", "New task for you: " + jstr(t, "key"), jstr(t, "title"), tid, 0, true);
    if (type == "CROSS_TEAM" && jint(q, "requesting_dept_id"))
      app.notify_all(app.leaders(jint(q, "requesting_dept_id")), jint(q, "requested_by"), "REQUEST_APPROVED",
                     "Cross-team request accepted: " + jstr(t, "key"), jstr(q, "title"), tid, rid);
  }
  app.db.exec("UPDATE work_requests SET status='APPROVED', decided_by=?, decision_note=?, decided_at=?, resulting_task_id=?, seq=? WHERE id=?",
              {u.id, note, now_iso(), tid, app.next_seq(), rid});
  app.audit(u.id, "approve_request", "request", rid, type);
  tx.commit();
  return json{{"ok", true}, {"task_id", tid}};
}

json Api::reject_request(const json& q, const json& b, const UserCtx& u) {
  require(can_decide_request(u, q), 403, "Only the responsible manager can reject this request");
  std::string note = req_text(b, "note", "Reason", 1000);
  Tx tx(app.db);
  app.db.exec("UPDATE work_requests SET status='REJECTED', decided_by=?, decision_note=?, decided_at=?, seq=? WHERE id=?",
              {u.id, note, now_iso(), app.next_seq(), q["id"]});
  if (jstr(q, "type") == "SELF_PROPOSAL" && jint(q, "resulting_task_id")) {
    app.db.exec("UPDATE tasks SET status='REJECTED' WHERE id=?", {q["resulting_task_id"]});
    app.add_event(jint(q, "resulting_task_id"), u.id, "Proposal not approved: " + note);
    app.touch_task(jint(q, "resulting_task_id"));
    tell_waiting_tasks(app.get_task(jint(q, "resulting_task_id")), u);
  }
  if (jstr(q, "type") == "TEAM_HELP" && jint(q, "parent_task_id")) {
    app.add_event(jint(q, "parent_task_id"), u.id, "Help request not approved: " + note);
    app.touch_task(jint(q, "parent_task_id"));
  }
  app.notify(jint(q, "requested_by"), "REQUEST_REJECTED", "Not approved: " + jstr(q, "title"), note,
             jint(q, "resulting_task_id"), jint(q, "id"), true);
  app.audit(u.id, "reject_request", "request", jint(q, "id"), note);
  tx.commit();
  return json{{"ok", true}};
}

json Api::withdraw_request(const json& q, const UserCtx& u) {
  require(jint(q, "requested_by") == u.id, 403, "Only the person who raised it can withdraw this request");
  Tx tx(app.db);
  app.db.exec("UPDATE work_requests SET status='WITHDRAWN', decided_at=?, seq=? WHERE id=?", {now_iso(), app.next_seq(), q["id"]});
  if (jstr(q, "type") == "SELF_PROPOSAL" && jint(q, "resulting_task_id")) {
    app.db.exec("UPDATE tasks SET status='REJECTED' WHERE id=?", {q["resulting_task_id"]});
    app.add_event(jint(q, "resulting_task_id"), u.id, "Proposal withdrawn");
    app.touch_task(jint(q, "resulting_task_id"));
  }
  app.notify_all(request_approvers(q), u.id, "REQUEST_WITHDRAWN", "Request withdrawn by " + u.name, jstr(q, "title"), 0, jint(q, "id"));
  app.audit(u.id, "withdraw_request", "request", jint(q, "id"));
  tx.commit();
  return json{{"ok", true}};
}

}  // namespace tf::api
