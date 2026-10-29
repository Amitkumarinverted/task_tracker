// Inverted TaskFlow Server - core context, organisation model, permission rules and helpers
#pragma once
#include <atomic>
#include <condition_variable>
#include <ctime>
#include <map>
#include <mutex>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>

#include "../../common/version.h"
#include "db.h"

namespace tf {

struct Config {
  // network
  std::string bind = "0.0.0.0";
  int port = 8080;
  std::string tls_cert, tls_key;  // set both to serve HTTPS (Linux build with OpenSSL)
  int max_clients = 300;          // concurrent desktop apps kept on instant-update connections
  int rate_limit = 30;            // requests per second per computer (burst: twice this)
  // storage
  std::string db_path = "taskflow.db";
  std::string backup_dir = "backups";
  std::string backup_time = "02:00";
  int backup_keep = 30;
  std::string log_file;  // empty = console only
  // company
  std::string company_name = TF_COMPANY_NAME;
  std::string company_tagline = "Projects - Tasks - Timelines";
  // security
  int session_hours = 12;
  int remember_days = 30;
  std::string admin_email = "admin@taskflow.local";
  std::string admin_password;  // empty = generate a random one on first start
  std::string min_client_version = "1.0.0";
  // e-mail (Linux build with OpenSSL)
  bool email_enabled = false;
  std::string smtp_host, smtp_security = "starttls", smtp_user, smtp_password, smtp_from, smtp_from_name = "Inverted TaskFlow";
  int smtp_port = 587;
  bool smtp_verify_cert = true;
  // testing
  bool demo = false;
  bool reset = false;
  std::string config_path = "server.ini";
};

// e-mail (mailer.cpp)
bool smtp_send(const struct Config& c, const std::string& to, const std::string& subject, const std::string& body, std::string& err);
class App;
void start_mailer(App& app);
void stop_mailer();

struct ApiError : std::runtime_error {
  int status;
  std::string code;  // machine-readable reason, e.g. PASSWORD_CHANGE_REQUIRED
  ApiError(int s, const std::string& m, const std::string& c = "") : std::runtime_error(m), status(s), code(c) {}
};

// The authenticated caller.
struct UserCtx {
  int64_t id = 0;
  std::string name, email, role, status;  // role: ADMIN | MANAGEMENT | STAFF
  int64_t scope_ver = 1;
  bool must_change_password = false;
  std::map<int64_t, std::string> units;  // unit (department or sub-department) id -> HEAD | DEPUTY | MEMBER
  bool management = false;               // MANAGEMENT role or member of the Management department
  bool is_admin() const { return role == "ADMIN"; }
  bool sees_all() const { return is_admin() || management; }
  bool member_of(int64_t unit) const { return units.count(unit) > 0; }
  bool leads(int64_t unit) const {
    auto it = units.find(unit);
    return it != units.end() && (it->second == "HEAD" || it->second == "DEPUTY");
  }
};

// helpers for reading request JSON
int64_t jint(const json& j, const char* k, int64_t def = 0);
std::string jstr(const json& j, const char* k, const std::string& def = "");
bool jhas(const json& j, const char* k);

std::string now_iso();
std::string iso_after_seconds(long long secs);
std::string today_local();
std::tm local_tm(std::time_t t);  // local time, thread-safe
std::string add_days(const std::string& ymd, int days);
bool valid_date(const std::string& ymd);
std::string hdate(const std::string& ymd);  // "3 Nov 2026" (for messages people read)
int compare_versions(const std::string& a, const std::string& b);  // <0, 0, >0
void log_line(const std::string& line);                             // console + log file
void set_log_file(const std::string& path);
void log_flush();  // wait (max 3 s) until queued log lines are written

class App {
 public:
  explicit App(const Config& cfg);
  Config cfg;
  Db db;
  std::recursive_mutex mu;          // serialises all database work
  std::condition_variable_any cv;   // signalled after every committed change (instant updates)
  std::atomic<int> waiters{0};
  std::atomic<bool> stopping{false}; // set (under mu) when the server shuts down: held instant-update requests return
  std::atomic<long long> backups_done{0};

  // ---- lifecycle
  void init_schema();           // creates / migrates the database
  void seed_base();             // standard departments + Routine projects (empty database only)
  void bootstrap_admin();       // first administrator (empty database only)
  void seed_demo();             // demo people, projects and tasks (testing only)
  void changed() { cv.notify_all(); }

  // ---- core helpers (call with mu held)
  int64_t next_seq();
  int64_t current_seq();
  void audit(int64_t actor, const std::string& action, const std::string& entity, int64_t id,
             const std::string& detail = "");
  void notify(int64_t user_id, const std::string& type, const std::string& title, const std::string& body,
              int64_t task_id = 0, int64_t request_id = 0, bool email = false);
  void notify_all(const std::set<int64_t>& users, int64_t except, const std::string& type, const std::string& title,
                  const std::string& body, int64_t task_id = 0, int64_t request_id = 0, bool email = false);
  void add_event(int64_t task_id, int64_t actor, const std::string& body);
  void bump_scope(int64_t user_id);
  // An org change in a unit can narrow what others see: their apps download everything again.
  // leadership = heads/deputies changed or the unit (de)activated -> also the members of the unit and its sub-units.
  void bump_scope_around(int64_t unit, bool leadership);
  std::string name_of(int64_t user_id);

  // ---- auth
  std::string hash_password(const std::string& pw, std::string& salt_hex, int& iters);
  bool verify_password(const std::string& pw, const json& user_row);
  bool load_user_by_token(const std::string& token, UserCtx& out);
  UserCtx load_ctx(int64_t user_id);
  static std::string password_problem(const std::string& pw);  // "" if acceptable

  // ---- organisation (departments with optional sub-departments)
  int64_t parent_of(int64_t unit);
  std::vector<int64_t> ancestors(int64_t unit);          // parent, grandparent, ...
  std::set<int64_t> subtree(int64_t unit);               // unit + all descendants
  std::string role_in(int64_t user, int64_t unit);       // HEAD | DEPUTY | MEMBER | ""
  std::set<int64_t> leaders(int64_t unit, bool heads_only = false);  // active heads (+deputies)
  int64_t management_unit();                             // id of the Management department (0 if none)
  // Direct manager(s) of a person for work in a unit (who reviews / approves for them).
  std::set<int64_t> managers_of(int64_t user, int64_t unit);
  // Everyone who may manage that person's work in that unit (direct managers + leaders above).
  std::set<int64_t> authority_over(int64_t user, int64_t unit);
  std::set<int64_t> managed_units(const UserCtx& u);     // units the caller leads, incl. sub-departments
  bool has_authority(const UserCtx& u, int64_t user, int64_t unit) {
    auto a = authority_over(user, unit);
    return a.count(u.id) > 0;
  }
  std::string unit_label(int64_t unit);                  // "Production / Line 2"

  // ---- visibility
  struct VisCtx {
    std::set<int64_t> my_tasks, parents_of_mine, managed;
  };
  VisCtx build_vis(const UserCtx& u);
  bool can_view_task(const UserCtx& u, const json& t, const VisCtx& v);
  bool can_view_task(const UserCtx& u, const json& t) { return can_view_task(u, t, build_vis(u)); }
  bool can_view_request(const UserCtx& u, const json& r, const VisCtx& v);
  bool can_manage_task(const UserCtx& u, const json& t) {
    return has_authority(u, jint(t, "assignee_id"), jint(t, "dept_id"));
  }
  // The person at the top (head or deputy of Management, or any head when there is no Management department) has
  // nobody above: they close and re-plan their own work themselves.
  bool self_managed(int64_t user, int64_t unit);
  // Why nobody can review this person's work ("" = someone can, or they manage themselves).
  std::string missing_reviewer_reason(int64_t user, int64_t unit);
  bool user_can_view_task(int64_t user, const json& t);
  // Who asked for a task: the requester and, for cross-team work, the leaders of the requesting department.
  std::set<int64_t> requester_side(const json& t);
  // ---- lost access ("tombstones"): who stops seeing a task when it is reassigned or moved
  std::set<int64_t> related_tasks(int64_t task_id);  // the task, its parent, siblings and sub-tasks
  std::set<int64_t> viewer_candidates(const std::set<int64_t>& task_ids);  // everyone who might see them (superset)
  std::map<int64_t, std::set<int64_t>> task_viewers(const std::set<int64_t>& task_ids, const std::set<int64_t>& candidates);
  void record_revocations(const std::map<int64_t, std::set<int64_t>>& before, const std::map<int64_t, std::set<int64_t>>& after);

  // ---- tasks
  json get_task(int64_t id);                 // throws 404
  std::string next_task_key(int64_t project_id);
  int64_t create_task(const json& fields);  // low-level insert (fields already validated)
  void touch_task(int64_t id);              // bump seq / updated_at / row_version
  void recompute_progress(int64_t task_id); // progress from sub-tasks; also refreshes the parent
  std::set<int64_t> reviewers_for(const json& task);  // who is asked to review a completion

  // ---- background jobs
  void run_reminders();
  bool run_backup(std::string* out_path = nullptr, std::string* err = nullptr);
};

}  // namespace tf
