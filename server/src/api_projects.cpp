// Inverted TaskFlow Server - project endpoints
#include "api.h"

namespace tf::api {

// ================================================================== projects
void Api::install_projects() {
  // all tasks of one project that the caller may see - the app loads closed projects' history on demand
  route("GET", R"(/projects/(\d+)/tasks)", true, [this](const Req& r, const json&, UserCtx* u) {
    json p = project_row(id_arg(r));
    auto vis = app.build_vis(*u);
    json out = json::array();
    for (auto& t : app.db.query("SELECT * FROM tasks WHERE project_id=?", {p["id"]}))
      if (app.can_view_task(*u, t, vis)) out.push_back(t);
    return json{{"tasks", out}};
  });

  auto can_own = [this](UserCtx* u, int64_t dept) {
    if (u->sees_all()) return true;
    if (u->leads(dept)) return true;
    for (auto a : app.ancestors(dept)) if (u->leads(a)) return true;
    return false;
  };
  route("POST", "/projects", true, [this, can_own](const Req&, const json& b, UserCtx* u) {
    int64_t dept = jint(b, "dept_id");
    unit_row(dept);
    require(can_own(u, dept), 403, "Only managers of that department, management or the administrator can create projects");
    std::string code = upper(req_text(b, "code", "Project code", 12));
    for (char c : code) require(isalnum((unsigned char)c), 400, "Project code may contain letters and digits only");
    require(app.db.one("SELECT id FROM projects WHERE code=?", {code}).is_null(), 409, "That project code is already used");
    std::string status = upper(jstr(b, "status", "ACTIVE"));
    require(status == "PLANNED" || status == "ACTIVE", 400, "A new project must be Planned or Active");
    std::string start = opt_date(b, "start_date", "Start date"), end = opt_date(b, "target_end", "Target end date");
    require(start.empty() || end.empty() || start <= end, 400, "Target end must be after the start date");
    std::vector<int64_t> members = jhas(b, "members") ? id_list(b, "members", 200, "project members") : std::vector<int64_t>();
    for (auto m : members) require(is_active_user(m), 400, app.name_of(m) + " is not an active user");
    Tx tx(app.db);
    int64_t id = app.db.insert(
        "INSERT INTO projects(code,name,description,owner_id,dept_id,status,start_date,target_end,created_at,seq) VALUES(?,?,?,?,?,?,?,?,?,?)",
        {code, req_text(b, "name", "Project name", 120), opt_text(b, "description", 2000, "Description"), u->id, dept, status,
         start, end, now_iso(), app.next_seq()});
    app.ensure_project_member(id, u->id, u->id, true);
    for (auto m : members)
      if (app.ensure_project_member(id, m, u->id, true) && m != u->id)
        app.notify(m, "PROJECT_MEMBER", "You were added to project " + code, req_text(b, "name", "Project name", 120) + " - added by " + u->name);
    app.audit(u->id, "create_project", "project", id, code);
    tx.commit();
    return json{{"ok", true}, {"id", id}};
  });

  // project members: they see all work of the project (only their own tasks are theirs to act on)
  route("POST", R"(/projects/(\d+)/members)", true, [this, can_own](const Req& r, const json& b, UserCtx* u) {
    json p = project_row(id_arg(r));
    require(can_own(u, jint(p, "dept_id")) || jint(p, "owner_id") == u->id, 403, "Only the project owner or the department's managers can add members");
    require(!jint(p, "is_routine"), 400, "Routine work is shared by the whole department - it has no member list");
    std::vector<int64_t> ids = id_list(b, "user_ids", 200, "people");
    require(!ids.empty(), 400, "Choose at least one person");
    for (auto m : ids) require(is_active_user(m), 400, app.name_of(m) + " is not an active user");
    Tx tx(app.db);
    int added = 0;
    for (auto m : ids)
      if (app.ensure_project_member(jint(p, "id"), m, u->id, true)) {
        added++;
        if (m != u->id) app.notify(m, "PROJECT_MEMBER", "You were added to project " + jstr(p, "code"), jstr(p, "name") + " - added by " + u->name);
      }
    app.audit(u->id, "add_project_members", "project", jint(p, "id"), b.dump());
    tx.commit();
    return json{{"ok", true}, {"added", added}};
  });

  route("DELETE", R"(/projects/(\d+)/members/(\d+))", true, [this, can_own](const Req& r, const json&, UserCtx* u) {
    json p = project_row(id_arg(r));
    int64_t who = id_arg(r, 2);
    require(can_own(u, jint(p, "dept_id")) || jint(p, "owner_id") == u->id, 403, "Only the project owner or the department's managers can remove members");
    require(who != jint(p, "owner_id"), 400, "The project owner always belongs to the project");
    json m = app.db.one("SELECT id FROM project_members WHERE project_id=? AND user_id=? AND active=1", {p["id"], who});
    require(!m.is_null(), 404, app.name_of(who) + " is not a member of this project");
    Tx tx(app.db);
    // "removed": taken off by hand - they do not come back on their own when they get work here, only when added again
    app.db.exec("UPDATE project_members SET active=0, removed=1, seq=? WHERE id=?", {app.next_seq(), m["id"]});
    app.db.exec("UPDATE projects SET seq=? WHERE id=?", {app.next_seq(), p["id"]});
    app.bump_scope(who);  // what they no longer see disappears from their app (their own tasks stay)
    app.audit(u->id, "remove_project_member", "project", jint(p, "id"), std::to_string(who));
    tx.commit();
    return json{{"ok", true}};
  });

  route("PATCH", R"(/projects/(\d+))", true, [this, can_own](const Req& r, const json& b, UserCtx* u) {
    json p = project_row(id_arg(r));
    require(can_own(u, jint(p, "dept_id")) || jint(p, "owner_id") == u->id, 403, "You cannot edit this project");
    std::string status = jhas(b, "status") ? upper(jstr(b, "status")) : jstr(p, "status");
    require(status == "PLANNED" || status == "ACTIVE" || status == "ON_HOLD" || status == "CLOSED", 400, "Invalid project status");
    if (status == "CLOSED" && jstr(p, "status") != "CLOSED") {
      int64_t open = jint(app.db.one("SELECT COUNT(*) AS n FROM tasks WHERE project_id=? AND status IN "
                                     "('TODO','IN_PROGRESS','ON_HOLD','IN_REVIEW','PROPOSED')", {p["id"]}), "n");
      require(open == 0, 400, "The project still has " + std::to_string(open) + " open task(s) - finish or cancel them first");
    }
    std::string start = jhas(b, "start_date") ? opt_date(b, "start_date", "Start date") : jstr(p, "start_date");
    std::string end = jhas(b, "target_end") ? opt_date(b, "target_end", "Target end date") : jstr(p, "target_end");
    require(start.empty() || end.empty() || start <= end, 400, "Target end must be after the start date");
    int64_t owner = jint(p, "owner_id");
    if (jhas(b, "owner_id") && jint(b, "owner_id") != owner) {  // hand the project over (e.g. the owner moved on)
      require(!jint(p, "is_routine"), 400, "Routine work has no owner to change");
      owner = jint(b, "owner_id");
      require(is_active_user(owner), 400, "Choose an active person as the new owner");
    }
    Tx tx(app.db);
    app.db.exec("UPDATE projects SET name=?, description=?, status=?, start_date=?, target_end=?, owner_id=?, seq=? WHERE id=?",
                {jhas(b, "name") ? req_text(b, "name", "Project name", 120) : jstr(p, "name"),
                 jhas(b, "description") ? opt_text(b, "description", 2000, "Description") : jstr(p, "description"), status,
                 start, end, owner ? json(owner) : json(), app.next_seq(), p["id"]});
    if (owner != jint(p, "owner_id")) {
      app.ensure_project_member(jint(p, "id"), owner, u->id, true);
      // the previous owner stays a member (the new owner or a manager can remove them)
      if (jint(p, "owner_id")) app.ensure_project_member(jint(p, "id"), jint(p, "owner_id"), u->id, true);
      app.bump_scope(owner);
      if (owner != u->id)
        app.notify(owner, "PROJECT_MEMBER", "You now own project " + jstr(p, "code"), jstr(p, "name") + " - handed over by " + u->name);
    }
    app.audit(u->id, "update_project", "project", jint(p, "id"), b.dump());
    tx.commit();
    return json{{"ok", true}};
  });
}

}  // namespace tf::api
