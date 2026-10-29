// Inverted TaskFlow Server - standard organisation (first start) and demo data (--demo, testing only)
#include <iostream>

#include "server.h"

namespace tf {

namespace {
struct BaseDept {
  const char* code;
  const char* name;
  int management;
};
// The departments of Inverted Energy. Sub-departments are added by the administrator in the desktop app.
const BaseDept kDepartments[] = {
    {"MGT", "Management", 1},          {"ELE", "Electronics Engineering", 0}, {"MEC", "Mechanical Engineering", 0},
    {"QUA", "Quality", 0},             {"PRC", "Process", 0},                 {"PRD", "Production", 0},
    {"SAL", "Sales", 0},               {"SCM", "Supply Chain Management", 0}, {"SRV", "Service", 0},
    {"IT", "IT", 0},                   {"HR", "HR", 0},
};
}  // namespace

void App::seed_base() {
  if (jint(db.one("SELECT COUNT(*) AS n FROM departments"), "n") > 0) return;
  Tx tx(db);
  int sort = 10;
  for (const auto& d : kDepartments) {
    int64_t id = db.insert("INSERT INTO departments(code,name,parent_id,is_management,sort,active,seq) VALUES(?,?,NULL,?,?,1,?)",
                           {d.code, d.name, d.management, sort, next_seq()});
    sort += 10;
    db.insert("INSERT INTO projects(code,name,description,owner_id,dept_id,status,is_routine,created_at,seq) "
              "VALUES(?,?,?,NULL,?,'ACTIVE',1,?,?)",
              {std::string(d.code) + "R", std::string(d.name) + " - Routine", "Day-to-day work of the department", id,
               now_iso(), next_seq()});
  }
  tx.commit();
  log_line("  Created the 11 standard departments with their Routine projects.");
}

void App::seed_demo() {
  std::lock_guard<std::recursive_mutex> lock(mu);
  if (jint(db.one("SELECT COUNT(*) AS n FROM users"), "n") > 1) return;  // only on a fresh database
  log_line("  Loading demo data (password for every demo user: Demo@123) ...");
  const std::string T = today_local();
  auto D = [&](int off) { return add_days(T, off); };
  auto TS = [&](int off, const char* hm = "11:00:00") { return D(off) + "T" + hm + "Z"; };
  const std::string now = now_iso();

  Tx tx(db);
  std::string salt;
  int it;
  const std::string hash = hash_password("Demo@123", salt, it);  // one hash shared by all demo users (demo only)
  auto dep = [&](const char* code) { return jint(db.one("SELECT id FROM departments WHERE code=?", {code}), "id"); };
  auto routine = [&](const char* code) { return jint(db.one("SELECT id FROM projects WHERE code=?", {std::string(code) + "R"}), "id"); };
  auto sub = [&](const std::string& code, const std::string& name, int64_t parent) {
    return db.insert("INSERT INTO departments(code,name,parent_id,is_management,sort,active,seq) VALUES(?,?,?,0,50,1,?)",
                     {code, name, parent, next_seq()});
  };
  int phone = 41000;
  auto user = [&](const std::string& email, const std::string& name, const std::string& desig, const std::string& role = "STAFF",
                  const std::string& status = "ACTIVE", int64_t req_dept = 0) {
    return db.insert(
        "INSERT INTO users(email,name,designation,phone,pw_salt,pw_hash,pw_iter,status,global_role,requested_dept_id,"
        "created_at,last_login,seq) VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?)",
        {email, name, desig, "+91 98100 " + std::to_string(phone++), salt, hash, it, status, role,
         req_dept ? json(req_dept) : json(), now, "", next_seq()});
  };
  auto member = [&](int64_t u, int64_t d, const char* role) {
    db.insert("INSERT INTO memberships(user_id,dept_id,role,active,seq) VALUES(?,?,?,1,?)", {u, d, role, next_seq()});
  };
  auto project = [&](const std::string& code, const std::string& name, const std::string& desc, int64_t d, int64_t owner,
                     int s, int e) {
    return db.insert("INSERT INTO projects(code,name,description,owner_id,dept_id,status,start_date,target_end,created_at,seq) "
                     "VALUES(?,?,?,?,?,'ACTIVE',?,?,?,?)",
                     {code, name, desc, owner, d, D(s), D(e), now, next_seq()});
  };
  auto event = [&](int64_t task, int64_t actor, const std::string& body, const std::string& at) {
    db.insert("INSERT INTO comments(task_id,author_id,kind,body,created_at,seq) VALUES(?,?,?,?,?,?)",
              {task, actor, "EVENT", body, at, next_seq()});
  };
  auto comment = [&](int64_t task, int64_t author, const std::string& body, const std::string& at) {
    db.insert("INSERT INTO comments(task_id,author_id,kind,body,created_at,seq) VALUES(?,?,?,?,?,?)",
              {task, author, "COMMENT", body, at, next_seq()});
  };

  struct Spec {
    int64_t project, dept, assignee, creator;
    std::string title, priority, status;
    int start, due;
    int64_t parent = 0;
    std::string origin = "MANAGER";
    int64_t requested_by = 0, requesting_dept = 0;
    int done_day = 0;            // completion day offset (DONE tasks)
    std::string note;            // completion note
    std::string desc;
  };
  auto task = [&](const Spec& s) {
    int64_t id = create_task({{"project_id", s.project}, {"title", s.title}, {"description", s.desc}, {"dept_id", s.dept},
                              {"assignee_id", s.assignee}, {"created_by", s.creator}, {"origin", s.origin},
                              {"requesting_dept_id", s.requesting_dept}, {"requested_by", s.requested_by},
                              {"parent_id", s.parent}, {"priority", s.priority}, {"status", s.status},
                              {"start_date", D(s.start)}, {"due_date", D(s.due)}});
    db.exec("UPDATE tasks SET created_at=?, updated_at=? WHERE id=?", {TS(s.start - 2, "05:30:00"), now, id});
    event(id, s.creator, std::string(s.parent ? "Sub-task created" : "Created") + " and assigned to " + name_of(s.assignee) +
                             ", due " + hdate(D(s.due)), TS(s.start - 2, "05:30:00"));
    if (s.status == "DONE") {
      db.exec("UPDATE tasks SET completed_at=?, submitted_at=? WHERE id=?", {TS(s.done_day, "12:30:00"), TS(s.done_day, "09:00:00"), id});
      auto rev = reviewers_for(get_task(id));
      int64_t reviewer = rev.empty() ? 0 : *rev.begin();
      db.insert("INSERT INTO completions(task_id,submitted_by,note,link,status,decided_by,decision_note,decided_at,created_at,seq) "
                "VALUES(?,?,?,?,'APPROVED',?,?,?,?,?)",
                {id, s.assignee, s.note.empty() ? "Completed as planned." : s.note, "", reviewer ? json(reviewer) : json(),
                 reviewer ? "Looks good." : "Own sub-task", TS(s.done_day, "12:30:00"), TS(s.done_day, "09:00:00"), next_seq()});
      event(id, s.assignee, "Marked complete: " + (s.note.empty() ? std::string("Completed as planned.") : s.note), TS(s.done_day, "09:00:00"));
      if (reviewer) event(id, reviewer, "Completion approved: Looks good.", TS(s.done_day, "12:30:00"));
    }
    return id;
  };
  auto submit = [&](int64_t id, const std::string& note, const std::string& link, int day) {
    json t = get_task(id);
    db.insert("INSERT INTO completions(task_id,submitted_by,note,link,status,created_at,seq) VALUES(?,?,?,?,'PENDING',?,?)",
              {id, t["assignee_id"], note, link, TS(day, "10:15:00"), next_seq()});
    db.exec("UPDATE tasks SET status='IN_REVIEW', submitted_at=? WHERE id=?", {TS(day, "10:15:00"), id});
    event(id, jint(t, "assignee_id"), "Marked complete - waiting for review: " + note + (link.empty() ? "" : "  [" + link + "]"),
          TS(day, "10:15:00"));
    for (auto r : reviewers_for(t))
      notify(r, "REVIEW_REQUESTED", "Please review: " + name_of(jint(t, "assignee_id")) + " completed " + jstr(t, "key"),
             jstr(t, "title") + " - " + note, id);
  };

  // ------------------------------------------------------------------ organisation
  const int64_t MGT = dep("MGT"), ELE = dep("ELE"), MEC = dep("MEC"), QUA = dep("QUA"), PRD = dep("PRD"), SCM = dep("SCM"),
                SAL = dep("SAL"), SRV = dep("SRV"), PRC = dep("PRC"), HR = dep("HR"), IT = dep("IT");
  const int64_t L1 = sub("PRDL1", "Line 1 Assembly", PRD);
  const int64_t L2 = sub("PRDL2", "Line 2 Assembly", PRD);
  const int64_t FW = sub("ELEFW", "BMS Firmware", ELE);
  const int64_t PE = sub("ELEPE", "Power Electronics", ELE);
  const int64_t IQC = sub("QUAIQ", "Incoming Quality", QUA);

  int64_t admin = jint(db.one("SELECT id FROM users WHERE global_role='ADMIN' ORDER BY id LIMIT 1"), "id");
  int64_t anita = user("director@demo.local", "Anita Sharma", "Director - Operations");
  int64_t sameer = user("cfo@demo.local", "Sameer Kapoor", "Chief Financial Officer");
  int64_t vikram = user("vikram@demo.local", "Vikram Singh", "Head - Production");
  int64_t imran = user("imran@demo.local", "Imran Khan", "Deputy Manager - Production");
  int64_t rahul = user("rahul@demo.local", "Rahul Verma", "Line 2 In-charge");
  int64_t sneha = user("sneha@demo.local", "Sneha Iyer", "Process Engineer");
  int64_t manoj = user("manoj@demo.local", "Manoj Patil", "Line Technician");
  int64_t suresh = user("suresh@demo.local", "Suresh Babu", "Line 1 In-charge");
  int64_t pooja = user("pooja@demo.local", "Pooja Shah", "Production Engineer");
  int64_t arjun = user("arjun@demo.local", "Arjun Mehta", "Head - Electronics Engineering");
  int64_t rohan = user("rohan@demo.local", "Rohan Das", "Lead - BMS Firmware");
  int64_t nikhil = user("nikhil@demo.local", "Nikhil Joshi", "Firmware Engineer");
  int64_t meera = user("meera@demo.local", "Meera Pillai", "Power Electronics Engineer");
  int64_t farhan = user("farhan@demo.local", "Farhan Ali", "Head - Mechanical Engineering");
  int64_t kavya = user("kavya@demo.local", "Kavya Reddy", "Design Engineer");
  int64_t neha = user("neha@demo.local", "Neha Gupta", "Head - Quality");
  int64_t amit = user("amit@demo.local", "Amit Kumar", "Quality Engineer");
  int64_t lakshmi = user("lakshmi@demo.local", "Lakshmi Menon", "IQC Lead");
  int64_t priya = user("priya@demo.local", "Priya Nair", "Head - Supply Chain");
  int64_t deepak = user("deepak@demo.local", "Deepak Yadav", "Purchase Executive");
  int64_t aditya = user("aditya@demo.local", "Aditya Rao", "Head - Sales");
  int64_t gaurav = user("gaurav@demo.local", "Gaurav Sinha", "Head - Service");
  int64_t ritu = user("ritu@demo.local", "Ritu Bansal", "Head - HR");
  int64_t sunil = user("sunil@demo.local", "Sunil Nair", "IT Manager");
  int64_t tarun = user("tarun@demo.local", "Tarun Bhatt", "Head - Process");
  user("newjoiner@demo.local", "Karan Malhotra", "Graduate Engineer Trainee", "STAFF", "PENDING", ELE);

  member(anita, MGT, "HEAD");
  member(sameer, MGT, "MEMBER");
  member(vikram, PRD, "HEAD");
  member(imran, PRD, "DEPUTY");
  member(rahul, L2, "HEAD");
  member(sneha, L2, "MEMBER");
  member(manoj, L2, "MEMBER");
  member(suresh, L1, "HEAD");
  member(pooja, L1, "MEMBER");
  member(arjun, ELE, "HEAD");
  member(rohan, FW, "HEAD");
  member(nikhil, FW, "MEMBER");
  member(meera, PE, "MEMBER");
  member(farhan, MEC, "HEAD");
  member(kavya, MEC, "MEMBER");
  member(neha, QUA, "HEAD");
  member(amit, QUA, "MEMBER");
  member(lakshmi, IQC, "HEAD");
  member(priya, SCM, "HEAD");
  member(deepak, SCM, "MEMBER");
  member(aditya, SAL, "HEAD");
  member(gaurav, SRV, "HEAD");
  member(ritu, HR, "HEAD");
  member(sunil, IT, "HEAD");
  member(tarun, PRC, "HEAD");
  (void)sameer;
  (void)PE;

  // ------------------------------------------------------------------ projects
  int64_t pLine = project("LINE2", "Line 2 Capacity Ramp-up", "Raise Line 2 output to 1,200 packs per day by quarter end", PRD, vikram, -30, 40);
  int64_t pGen3 = project("GEN3", "51.2V 105Ah Pack Gen 3", "Next-generation pack: lighter enclosure, CAN V2 BMS", ELE, arjun, -45, 60);
  int64_t pIatf = project("IATF", "IATF Surveillance Audit", "Prepare for the annual IATF 16949 surveillance audit", QUA, neha, -20, 25);
  int64_t pCell = project("CELLQ", "Second Cell Vendor Qualification", "Qualify an alternate LFP cell source", SCM, priya, -15, 50);

  // ------------------------------------------------------------------ Production (two-level: Vikram -> Rahul -> team)
  int64_t weld = task({pLine, L2, rahul, vikram, "Install second laser welding station", "HIGH", "IN_PROGRESS", -20, 6, 0, "MANAGER", 0, 0, 0, "",
                       "Station 2 for busbar welding - includes commissioning and operator SOP"});
  task({pLine, L2, manoj, rahul, "Electrical hook-up and safety interlocks", "HIGH", "DONE", -18, -10, weld, "MANAGER", 0, 0, -11,
        "Hook-up done, interlocks tested with the safety officer."});
  task({pLine, L2, rahul, rahul, "Commissioning checklist", "MEDIUM", "DONE", -12, -6, weld, "MANAGER", 0, 0, -7});
  int64_t trials = task({pLine, L2, sneha, rahul, "Weld parameter trials (30 samples)", "HIGH", "IN_PROGRESS", -5, 3, weld});
  task({pLine, L2, manoj, rahul, "Operator SOP for station 2", "MEDIUM", "TODO", 1, 5, weld});

  int64_t cyc = task({pLine, L2, sneha, rahul, "Cycle-time study for module stacking", "HIGH", "IN_PROGRESS", -14, 1});
  submit(cyc, "Study done for both shifts - average 41 s per module, bottleneck is the stacking jig.",
         "https://intranet.inverted.local/reports/line2-cycle-time.xlsx", 0);
  int64_t pfmea = task({pLine, L2, sneha, rahul, "Update PFMEA for the new welding cell", "MEDIUM", "IN_PROGRESS", -10, -2});
  task({pLine, L2, manoj, rahul, "Operator training - shift B", "MEDIUM", "TODO", 3, 12});
  int64_t dash = task({pLine, L2, rahul, vikram, "Line 2 daily output dashboard", "MEDIUM", "IN_PROGRESS", -8, 2});
  submit(dash, "Dashboard live on the shop-floor TV, updated every hour from the EOL tester.", "", -1);
  task({pLine, PRD, imran, vikram, "Line balancing simulation", "MEDIUM", "DONE", -18, -2, 0, "MANAGER", 0, 0, -3,
        "Simulation shows 1,180 packs/day with 2 extra operators."});
  int64_t l1 = task({routine("PRD"), L1, pooja, suresh, "Daily 5S audit - this week", "LOW", "IN_PROGRESS", -2, 3});
  task({routine("PRD"), L1, suresh, vikram, "Calibrate torque tools", "MEDIUM", "DONE", -25, -15, 0, "MANAGER", 0, 0, -16});
  (void)l1;

  // ------------------------------------------------------------------ Electronics (Arjun -> Rohan -> Nikhil)
  int64_t can = task({pGen3, FW, rohan, arjun, "BMS firmware: CAN protocol V2", "HIGH", "IN_PROGRESS", -21, 10});
  task({pGen3, FW, nikhil, rohan, "Update CAN DBC file", "MEDIUM", "DONE", -20, -12, can, "MANAGER", 0, 0, -13});
  task({pGen3, FW, nikhil, rohan, "Bootloader compatibility test", "HIGH", "IN_PROGRESS", -10, 4, can});
  task({pGen3, FW, rohan, rohan, "Regression run on the HIL bench", "MEDIUM", "TODO", 4, 9, can});
  task({pGen3, FW, nikhil, rohan, "Cell balancing algorithm tuning", "MEDIUM", "DONE", -40, -20, 0, "MANAGER", 0, 0, -17,
        "Balancing current raised to 150 mA; validated on 3 packs."});
  task({pGen3, PE, meera, arjun, "Thermal simulation at 1C continuous", "HIGH", "TODO", 2, 18});
  task({pGen3, PE, meera, arjun, "Pre-charge circuit review", "MEDIUM", "IN_PROGRESS", -6, -1});

  // cross-team: Electronics asked Mechanical for the enclosure
  int64_t rqEnc = db.insert(
      "INSERT INTO work_requests(type,status,project_id,title,description,requested_by,requesting_dept_id,target_dept_id,"
      "needed_by,priority,decided_by,decided_at,created_at,seq) VALUES('CROSS_TEAM','APPROVED',?,?,?,?,?,?,?,?,?,?,?,?)",
      {pGen3, "Enclosure design freeze (rev C)", "Lighter enclosure for Gen 3 - target 2.1 kg", arjun, ELE, MEC, D(5), "CRITICAL",
       farhan, TS(-31), TS(-32), next_seq()});
  int64_t enc = task({pGen3, MEC, kavya, farhan, "Enclosure design freeze (rev C)", "CRITICAL", "IN_PROGRESS", -30, 5, 0, "CROSS_TEAM",
                      arjun, ELE});
  db.exec("UPDATE tasks SET request_id=? WHERE id=?", {rqEnc, enc});
  db.exec("UPDATE work_requests SET resulting_task_id=? WHERE id=?", {enc, rqEnc});
  task({pGen3, MEC, kavya, kavya, "Gasket groove dimensions", "HIGH", "DONE", -25, -8, enc, "SUBTASK", 0, 0, -9});
  task({pGen3, MEC, kavya, kavya, "Drawing release to PDM", "MEDIUM", "TODO", 1, 5, enc, "SUBTASK"});

  // ------------------------------------------------------------------ Quality
  int64_t audit_t = task({pIatf, QUA, amit, neha, "Internal audit - production processes", "HIGH", "IN_PROGRESS", -12, 7});
  task({pIatf, QUA, amit, amit, "Audit Line 1", "MEDIUM", "DONE", -10, -5, audit_t, "SUBTASK", 0, 0, -6});
  task({pIatf, QUA, amit, amit, "Audit Line 2", "MEDIUM", "TODO", 0, 5, audit_t, "SUBTASK"});
  task({pIatf, QUA, amit, neha, "Close last year's minor NCs", "HIGH", "DONE", -20, -4, 0, "MANAGER", 0, 0, -5});
  task({pIatf, QUA, neha, anita, "Management review presentation", "MEDIUM", "TODO", 6, 20});
  task({pIatf, IQC, lakshmi, neha, "Update incoming inspection plans", "MEDIUM", "IN_PROGRESS", -4, 8});

  // ------------------------------------------------------------------ Supply chain
  task({pCell, SCM, deepak, priya, "Request samples from 3 cell vendors", "HIGH", "DONE", -14, -7, 0, "MANAGER", 0, 0, -8});
  task({pCell, SCM, deepak, priya, "Commercial comparison sheet", "MEDIUM", "IN_PROGRESS", -5, 3});
  task({pCell, SCM, priya, anita, "Vendor audit visit planning", "MEDIUM", "TODO", 7, 21});

  // ------------------------------------------------------------------ pending items for the approval inboxes
  // help request (Rahul's team member asks for a colleague)
  int64_t rqHelp = db.insert(
      "INSERT INTO work_requests(type,status,project_id,title,description,requested_by,requesting_dept_id,target_dept_id,"
      "parent_task_id,suggested_assignee_id,needed_by,priority,created_at,seq) VALUES('TEAM_HELP','PENDING',?,?,?,?,?,?,?,?,?,?,?,?)",
      {pLine, "Pull-test 15 weld samples on night shift", "Need samples from both shifts for the trial report", sneha, L2, L2, trials,
       manoj, D(3), "HIGH", TS(0, "08:20:00"), next_seq()});
  notify(rahul, "HELP_REQUEST", "Help requested by Sneha Iyer on " + jstr(get_task(trials), "key"), "Pull-test 15 weld samples on night shift",
         trials, rqHelp);
  // own task proposal
  int64_t rqProp = db.insert(
      "INSERT INTO work_requests(type,status,project_id,title,description,requested_by,requesting_dept_id,target_dept_id,"
      "needed_by,priority,created_at,seq) VALUES('SELF_PROPOSAL','PENDING',?,?,?,?,?,?,?,?,?,?)",
      {pLine, "Poka-yoke fixture for cell polarity check", "Avoid reversed cells at stacking - simple fixture, 2 days work", manoj, L2,
       L2, D(10), "MEDIUM", TS(0, "07:45:00"), next_seq()});
  int64_t tProp = task({pLine, L2, manoj, manoj, "Poka-yoke fixture for cell polarity check", "MEDIUM", "PROPOSED", 2, 10, 0, "SELF"});
  db.exec("UPDATE tasks SET request_id=?, original_due='' WHERE id=?", {rqProp, tProp});
  db.exec("UPDATE work_requests SET resulting_task_id=? WHERE id=?", {tProp, rqProp});
  notify(rahul, "PROPOSAL", "Task proposal from Manoj Patil", "Poka-yoke fixture for cell polarity check", tProp, rqProp);
  // cross-team: supply chain asks quality
  int64_t rqX = db.insert(
      "INSERT INTO work_requests(type,status,project_id,title,description,requested_by,requesting_dept_id,target_dept_id,"
      "needed_by,priority,created_at,seq) VALUES('CROSS_TEAM','PENDING',?,?,?,?,?,?,?,?,?,?)",
      {pCell, "Teardown and incoming test of vendor cell samples", "Capacity, IR and dimensional check on 10 cells per vendor", priya,
       SCM, QUA, D(12), "HIGH", TS(-1), next_seq()});
  notify(neha, "CROSS_TEAM_REQUEST", "Request from Priya Nair (Supply Chain Management)", "Teardown and incoming test of vendor cell samples",
         0, rqX);
  // management asks production
  int64_t rqM = db.insert(
      "INSERT INTO work_requests(type,status,project_id,title,description,requested_by,requesting_dept_id,target_dept_id,"
      "needed_by,priority,created_at,seq) VALUES('CROSS_TEAM','PENDING',?,?,?,?,?,?,?,?,?,?)",
      {pLine, "Monthly capacity report for the board meeting", "Line 2 output trend, constraints and investment needs", anita, MGT, PRD,
       D(5), "MEDIUM", TS(0, "06:10:00"), next_seq()});
  notify(vikram, "CROSS_TEAM_REQUEST", "Request from Anita Sharma (Management)", "Monthly capacity report for the board meeting", 0, rqM);
  notify(imran, "CROSS_TEAM_REQUEST", "Request from Anita Sharma (Management)", "Monthly capacity report for the board meeting", 0, rqM);
  // extension request on an overdue task
  db.insert("INSERT INTO extensions(task_id,requested_by,current_due,proposed_due,reason,status,created_at,seq) VALUES(?,?,?,?,?,'PENDING',?,?)",
            {pfmea, sneha, D(-2), D(4), "Waiting for the final weld parameters from the trials", TS(0, "09:00:00"), next_seq()});
  notify(rahul, "EXTENSION_REQUESTED", "Extension requested: " + jstr(get_task(pfmea), "key"), "Sneha Iyer asks to move the due date to " + hdate(D(4)), pfmea);

  comment(weld, rahul, "Station delivered and hooked up. Trials running this week.", TS(-6, "12:00:00"));
  comment(weld, vikram, "Good progress. Share the trial results before we release the SOP.", TS(-5, "06:40:00"));
  comment(enc, kavya, "Rev C drawings uploaded to PDM. Gasket review done.", TS(-8, "10:00:00"));
  comment(can, nikhil, "DBC updated - bootloader test started on bench 2.", TS(-3, "11:20:00"));
  if (admin) notify(admin, "ACCOUNT_PENDING", "New account waiting for approval", "Karan Malhotra <newjoiner@demo.local>");

  // progress of every main task from its sub-tasks
  for (auto& t : db.query("SELECT id FROM tasks WHERE parent_id IS NULL")) recompute_progress(jint(t, "id"));
  for (auto& t : db.query("SELECT id FROM tasks WHERE status='DONE'")) db.exec("UPDATE tasks SET progress=100 WHERE id=?", {t["id"]});
  tx.commit();
  log_line("  Demo data ready.");
}

}  // namespace tf
