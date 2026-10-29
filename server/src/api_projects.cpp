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
    Tx tx(app.db);
    int64_t id = app.db.insert(
        "INSERT INTO projects(code,name,description,owner_id,dept_id,status,start_date,target_end,created_at,seq) VALUES(?,?,?,?,?,?,?,?,?,?)",
        {code, req_text(b, "name", "Project name", 120), opt_text(b, "description", 2000, "Description"), u->id, dept, status,
         start, end, now_iso(), app.next_seq()});
    app.audit(u->id, "create_project", "project", id, code);
    tx.commit();
    return json{{"ok", true}, {"id", id}};
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
    app.db.exec("UPDATE projects SET name=?, description=?, status=?, start_date=?, target_end=?, seq=? WHERE id=?",
                {jhas(b, "name") ? req_text(b, "name", "Project name", 120) : jstr(p, "name"),
                 jhas(b, "description") ? opt_text(b, "description", 2000, "Description") : jstr(p, "description"), status,
                 start, end, app.next_seq(), p["id"]});
    app.audit(u->id, "update_project", "project", jint(p, "id"), b.dump());
    return json{{"ok", true}};
  });
}

}  // namespace tf::api
