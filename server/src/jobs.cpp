// Inverted TaskFlow Server - scheduled jobs: due/overdue reminders and daily backups (scheduler)
#include "server.h"

#include <chrono>
#include <filesystem>
#include <algorithm>

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
