// Inverted TaskFlow Server - scheduled jobs: due/overdue reminders and daily backups (scheduler)
#include "server.h"

#include <chrono>
#include <filesystem>
#include <algorithm>
#include <map>
#include <set>

namespace fs = std::filesystem;

namespace tf {

// ============================================================== reminders
void App::run_reminders() {
  std::lock_guard<std::recursive_mutex> lock(mu);
  const std::string today = today_local();
  const std::string tomorrow = add_days(today, 1);
  json open = db.query(
      "SELECT id,key,title,assignee_id,dept_id,due_date FROM tasks "
      "WHERE status IN ('TODO','IN_PROGRESS','ON_HOLD') AND due_date<>'' AND due_date<=?", {tomorrow});
  json reviews = db.query("SELECT id,key,title,assignee_id,dept_id,parent_id,submitted_at FROM tasks "
                          "WHERE status='IN_REVIEW' AND submitted_at<>'' AND submitted_at<?", {iso_after_seconds(-2 * 86400)});
  Tx tx(db);
  bool any = false;
  for (auto& t : open) {
    std::string due = jstr(t, "due_date");
    std::string kind = due < today ? "OVERDUE" : "DUE_SOON";
    int late = 0;  // days overdue
    if (kind == "OVERDUE") {
      while (late < 4000 && add_days(due, late) < today) late++;
      if (!(late == 1 || late == 3 || late == 7 || (late > 7 && late % 7 == 0))) continue;  // not every day forever
    }
    if (db.exec("INSERT OR IGNORE INTO reminders_sent(task_id,kind,day) VALUES(?,?,?)", {t["id"], kind, today}) == 0) continue;
    any = true;
    std::string label = jstr(t, "key") + "  " + jstr(t, "title");
    if (kind == "DUE_SOON") {
      notify(jint(t, "assignee_id"), "DUE_SOON", due == today ? "Due today" : "Due tomorrow", label, jint(t, "id"), 0, true);
    } else {
      notify(jint(t, "assignee_id"), "OVERDUE", "Task overdue (was due " + hdate(due) + ")", label, jint(t, "id"), 0, late == 1 || late == 7);
      notify_all(managers_of(jint(t, "assignee_id"), jint(t, "dept_id")), jint(t, "assignee_id"), "OVERDUE",
                 "Team task overdue: " + name_of(jint(t, "assignee_id")) + " (was due " + hdate(due) + ")", label, jint(t, "id"));
    }
  }
  for (auto& t : reviews) {
    if (db.exec("INSERT OR IGNORE INTO reminders_sent(task_id,kind,day) VALUES(?,?,?)", {t["id"], "REVIEW_WAITING", today}) == 0)
      continue;
    any = true;
    notify_all(reviewers_for(t), jint(t, "assignee_id"), "REVIEW_REQUESTED",
               "Reminder: completion waiting for your review", jstr(t, "key") + "  " + jstr(t, "title"), jint(t, "id"), 0, true);
  }
  db.exec("DELETE FROM reminders_sent WHERE day < ?", {add_days(today, -60)});
  tx.commit();
  if (any) changed();
}

// ============================================================== morning digest
// One e-mail per person who has something to act on today: their overdue work, what is due today and tomorrow,
// and - for managers - the decisions waiting for them and their team's overdue work. Nobody else is e-mailed.
int App::run_digest() {
  std::lock_guard<std::recursive_mutex> lock(mu);
  const std::string today = today_local(), tomorrow = add_days(today, 1);
  std::map<int64_t, std::vector<std::string>> overdue, due_today, due_tomorrow;
  std::map<int64_t, int> decisions, team_overdue;
  for (auto& t : db.query("SELECT id,key,title,assignee_id,dept_id,due_date FROM tasks WHERE status IN ('TODO','IN_PROGRESS','ON_HOLD') "
                          "AND due_date<>'' AND due_date<=?", {tomorrow})) {
    int64_t a = jint(t, "assignee_id");
    std::string line = jstr(t, "key") + "  " + jstr(t, "title");
    std::string due = jstr(t, "due_date");
    if (due < today) {
      overdue[a].push_back(line + "  (due " + hdate(due) + ")");
      for (auto m : authority_over(a, jint(t, "dept_id"))) team_overdue[m]++;
    } else if (due == today) due_today[a].push_back(line);
    else due_tomorrow[a].push_back(line);
  }
  for (auto& t : db.query("SELECT * FROM tasks WHERE status='IN_REVIEW'"))
    for (auto r : reviewers_for(t)) decisions[r]++;
  for (auto& e : db.query("SELECT t.assignee_id, t.dept_id FROM extensions e JOIN tasks t ON t.id=e.task_id WHERE e.status='PENDING'"))
    for (auto m : managers_of(jint(e, "assignee_id"), jint(e, "dept_id"))) decisions[m]++;
  for (auto& q : db.query("SELECT type, requested_by, target_dept_id FROM work_requests WHERE status='PENDING'")) {
    std::set<int64_t> who;
    if (jstr(q, "type") == "CROSS_TEAM") {
      who = leaders(jint(q, "target_dept_id"));
      if (who.empty())
        for (auto a : ancestors(jint(q, "target_dept_id"))) {
          who = leaders(a);
          if (!who.empty()) break;
        }
    } else {
      who = managers_of(jint(q, "requested_by"), jint(q, "target_dept_id"));
    }
    for (auto w : who) decisions[w]++;
  }
  std::set<int64_t> people;
  for (auto* m : {&overdue, &due_today, &due_tomorrow})
    for (auto& kv : *m) people.insert(kv.first);
  for (auto* m : {&decisions, &team_overdue})
    for (auto& kv : *m) people.insert(kv.first);
  int queued = 0;
  Tx tx(db);
  for (auto uid : people) {
    json u = db.one("SELECT email, name, status FROM users WHERE id=?", {uid});
    if (u.is_null() || jstr(u, "status") != "ACTIVE" || jstr(u, "email").empty()) continue;
    auto list = [&](const char* title, const std::vector<std::string>& v) {
      if (v.empty()) return std::string();
      std::string s = std::string(title) + "\r\n";
      for (size_t i = 0; i < v.size() && i < 10; i++) s += "  - " + v[i] + "\r\n";
      if (v.size() > 10) s += "  ... and " + std::to_string(v.size() - 10) + " more\r\n";
      return s + "\r\n";
    };
    std::vector<std::string> head;
    if (!overdue[uid].empty()) head.push_back(std::to_string(overdue[uid].size()) + " overdue");
    if (!due_today[uid].empty()) head.push_back(std::to_string(due_today[uid].size()) + " due today");
    if (decisions[uid]) head.push_back(std::to_string(decisions[uid]) + " waiting for your decision");
    if (team_overdue[uid]) head.push_back(std::to_string(team_overdue[uid]) + " overdue in your team");
    if (!due_tomorrow[uid].empty()) head.push_back(std::to_string(due_tomorrow[uid].size()) + " due tomorrow");
    std::string subject = "[TaskFlow] Your day: ";
    for (size_t i = 0; i < head.size(); i++) subject += (i ? ", " : "") + head[i];
    std::string body = "Good morning " + jstr(u, "name") + ",\r\n\r\n" + list("Overdue - please finish or ask for more time:", overdue[uid]) +
                       list("Due today:", due_today[uid]) + list("Due tomorrow:", due_tomorrow[uid]);
    if (decisions[uid])
      body += std::to_string(decisions[uid]) + " item(s) wait for your decision - open Approvals in TaskFlow.\r\n\r\n";
    if (team_overdue[uid]) body += std::to_string(team_overdue[uid]) + " task(s) of people you manage are overdue - see My team.\r\n\r\n";
    body += "-- Inverted TaskFlow (morning summary; sent only when something needs you)";
    db.insert("INSERT INTO email_outbox(to_email,subject,body,created_at) VALUES(?,?,?,?)", {u["email"], subject, body, now_iso()});
    queued++;
  }
  tx.commit();
  return queued;
}

// ============================================================== backups
bool App::run_backup(std::string* out_path, std::string* err) {
  std::error_code ec;
  fs::create_directories(cfg.backup_dir, ec);
  std::tm l = local_tm(std::time(nullptr));
  char name[64];
  strftime(name, sizeof name, "taskflow-%Y%m%d-%H%M%S.db", &l);
  std::string path = (fs::path(cfg.backup_dir) / name).string();
  {
    std::lock_guard<std::recursive_mutex> lock(mu);
    std::string e;
    if (!db.backup_to(path, e)) {
      if (err) *err = e;
      log_line("  ! backup failed: " + e);
      return false;
    }
  }
  // prune old backups
  std::vector<fs::path> files;
  for (auto& de : fs::directory_iterator(cfg.backup_dir, ec)) {
    auto fn = de.path().filename().string();
    if (fn.rfind("taskflow-", 0) == 0 && de.path().extension() == ".db") files.push_back(de.path());
  }
  std::sort(files.begin(), files.end());
  while ((int)files.size() > std::max(1, cfg.backup_keep)) {
    fs::remove(files.front(), ec);
    files.erase(files.begin());
  }
  backups_done++;
  if (out_path) *out_path = path;
  log_line("  Backup written: " + path);
  return true;
}

}  // namespace tf
