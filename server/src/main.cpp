// Inverted TaskFlow Server - entry point, settings, background jobs and maintenance commands
//
//   TaskFlowServer [--config server.ini]                 run the server (Ctrl+C to stop)
//   TaskFlowServer --demo --reset --port 8080            local test server with demo data (testing only)
//   TaskFlowServer --backup-now | --check | --version
//   TaskFlowServer --reset-password EMAIL                 sets a temporary password and prints it
//   TaskFlowServer --test-email ADDRESS                   sends a test e-mail with the [email] settings
//   TaskFlowServer --install-service | --uninstall-service  (Windows)
#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <memory>
#include <thread>

#include "crypto.h"
#include "httplib.h"
#include "server.h"

#ifdef _WIN32
#include <windows.h>
#endif

namespace fs = std::filesystem;

namespace tf {
void register_routes(httplib::Server& srv, App& app);
#ifdef _WIN32
int run_as_service(std::function<int()> run, std::function<void()> stop);
int install_service();
int uninstall_service();
#endif
}  // namespace tf

using namespace tf;

static std::atomic<bool> g_running{true};
static std::atomic<httplib::Server*> g_srv{nullptr};

static void request_stop() {
  g_running = false;
  if (auto* s = g_srv.load()) s->stop();
}
static void on_signal(int) { request_stop(); }

static std::string trim(const std::string& s) {
  size_t a = s.find_first_not_of(" \t\r\n");
  if (a == std::string::npos) return "";
  size_t b = s.find_last_not_of(" \t\r\n");
  return s.substr(a, b - a + 1);
}
static bool truthy(const std::string& v) { return v == "1" || v == "true" || v == "yes" || v == "on"; }

static bool load_ini(const std::string& path, Config& c, bool must_exist) {
  std::ifstream f(path);
  if (!f) {
    if (must_exist) std::cerr << "  Settings file not found: " << path << "\n";
    return !must_exist;
  }
  std::string line;
  int n = 0;
  while (std::getline(f, line)) {
    n++;
    line = trim(line);
    if (line.empty() || line[0] == '#' || line[0] == ';' || line[0] == '[') continue;
    auto eq = line.find('=');
    if (eq == std::string::npos) {
      std::cerr << "  server.ini line " << n << " ignored (no '='): " << line << "\n";
      continue;
    }
    std::string k = trim(line.substr(0, eq)), v = trim(line.substr(eq + 1));
    if (v.size() >= 2 && v.front() == '"' && v.back() == '"') v = v.substr(1, v.size() - 2);
    auto num = [&](int lo, int hi, int def) {
      int x = std::atoi(v.c_str());
      if (x < lo || x > hi) {
        std::cerr << "  server.ini: " << k << " = " << v << " is out of range (" << lo << "-" << hi << "), using " << def << "\n";
        return def;
      }
      return x;
    };
    if (k == "bind") c.bind = v;
    else if (k == "port") c.port = num(1, 65535, 8080);
    else if (k == "tls_cert") c.tls_cert = v;
    else if (k == "tls_key") c.tls_key = v;
    else if (k == "max_clients") c.max_clients = num(10, 2000, 300);
    else if (k == "rate_limit_per_ip") c.rate_limit = num(5, 10000, 30);
    else if (k == "database") c.db_path = v;
    else if (k == "backup_dir") c.backup_dir = v;
    else if (k == "backup_time") c.backup_time = v;
    else if (k == "backup_keep") c.backup_keep = num(1, 3650, 30);
    else if (k == "log_file") c.log_file = v;
    else if (k == "company_name") c.company_name = v;
    else if (k == "company_tagline") c.company_tagline = v;
    else if (k == "session_hours") c.session_hours = num(1, 720, 12);
    else if (k == "remember_days") c.remember_days = num(1, 365, 30);
    else if (k == "admin_email") c.admin_email = v;
    else if (k == "admin_password") c.admin_password = v;
    else if (k == "min_client_version") c.min_client_version = v;
    else if (k == "email_enabled") c.email_enabled = truthy(v);
    else if (k == "smtp_host") c.smtp_host = v;
    else if (k == "smtp_port") c.smtp_port = num(1, 65535, 587);
    else if (k == "smtp_security") c.smtp_security = v;
    else if (k == "smtp_user") c.smtp_user = v;
    else if (k == "smtp_password") c.smtp_password = v;
    else if (k == "smtp_from") c.smtp_from = v;
    else if (k == "smtp_from_name") c.smtp_from_name = v;
    else if (k == "smtp_verify_cert") c.smtp_verify_cert = truthy(v);
    else if (k == "digest_time") c.digest_time = v;
    else if (k == "demo_data") c.demo = truthy(v);
    else std::cerr << "  server.ini line " << n << ": unknown setting '" << k << "' ignored\n";
  }
  if (c.smtp_security != "starttls" && c.smtp_security != "ssl" && c.smtp_security != "none") {
    std::cerr << "  server.ini: smtp_security must be starttls, ssl or none - using starttls\n";
    c.smtp_security = "starttls";
  }
  if (c.digest_time != "off") {
    int dh = 0, dm = 0;
    if (sscanf(c.digest_time.c_str(), "%d:%d", &dh, &dm) != 2 || dh < 0 || dh > 23 || dm < 0 || dm > 59) {
      std::cerr << "  server.ini: digest_time must look like 08:30 (or off) - using 08:30\n";
      c.digest_time = "08:30";
    }
  }
  int hh = 0, mm = 0;
  if (sscanf(c.backup_time.c_str(), "%d:%d", &hh, &mm) != 2 || hh < 0 || hh > 23 || mm < 0 || mm > 59) {
    std::cerr << "  server.ini: backup_time must look like 02:00 - using 02:00\n";
    c.backup_time = "02:00";
  }
  return true;
}

static std::string local_hm() {
  std::time_t t = std::time(nullptr);
  std::tm l{};
#ifdef _WIN32
  localtime_s(&l, &t);
#else
  localtime_r(&t, &l);
#endif
  char buf[8];
  strftime(buf, sizeof buf, "%H:%M", &l);
  return buf;
}

// A database from TaskFlow 0.1 (schema 1) is kept aside so the new version starts clean.
static void retire_old_database(const Config& cfg) {
  std::error_code ec;
  if (!fs::exists(cfg.db_path, ec)) return;
  bool old = false;
  {
    Db probe(cfg.db_path);
    bool has_tasks = !probe.one("SELECT name FROM sqlite_master WHERE type='table' AND name='tasks'").is_null();
    bool has_meta = !probe.one("SELECT name FROM sqlite_master WHERE type='table' AND name='meta'").is_null();
    json schema = has_meta ? probe.one("SELECT value FROM meta WHERE key='schema'") : json();
    old = has_tasks && (schema.is_null() || std::atoi(jstr(schema, "value").c_str()) < 2);
  }
  if (!old) return;
  std::string keep = cfg.db_path + ".v01-backup";
  fs::rename(cfg.db_path, keep, ec);
  fs::remove(cfg.db_path + "-wal", ec);
  fs::remove(cfg.db_path + "-shm", ec);
  log_line("  Found a TaskFlow 0.1 test database - moved to " + keep + " and starting a new one.");
}

static std::unique_ptr<App> open_app(const Config& cfg) {
  std::unique_ptr<App> app(new App(cfg));
  std::lock_guard<std::recursive_mutex> lock(app->mu);
  app->init_schema();
  app->seed_base();
  app->bootstrap_admin();
  return app;
}

static int run_server(Config cfg) {
  g_running = true;
  if (!cfg.log_file.empty()) {
    std::error_code ec;
    auto dir = fs::path(cfg.log_file).parent_path();
    if (!dir.empty()) fs::create_directories(dir, ec);
    set_log_file(cfg.log_file);
  }
  log_line("");
  log_line("  " TF_PRODUCT_NAME " Server " TF_VERSION);
  log_line("  ------------------------------------------------");
  if (cfg.reset) {
    std::error_code ec;
    fs::remove(cfg.db_path, ec);
    fs::remove(cfg.db_path + "-wal", ec);
    fs::remove(cfg.db_path + "-shm", ec);
    log_line("  Database reset (testing).");
  }
  std::unique_ptr<App> app;
  try {
    retire_old_database(cfg);
    app = open_app(cfg);
    std::lock_guard<std::recursive_mutex> lock(app->mu);
    std::string chk = app->db.check(false);
    if (chk != "ok") log_line("  !!! DATABASE CHECK FAILED: " + chk + "  - restore the latest backup (see the IT Admin Guide)");
  } catch (const std::exception& e) {
    log_line(std::string("  FATAL: ") + e.what());
    log_flush();
    return 2;
  }
  if (cfg.demo) {
    try { app->seed_demo(); } catch (const std::exception& e) { log_line(std::string("  demo data failed: ") + e.what()); }
  }

  std::unique_ptr<httplib::Server> srv;
  bool tls = !cfg.tls_cert.empty() || !cfg.tls_key.empty();
  if (tls) {
#ifdef CPPHTTPLIB_OPENSSL_SUPPORT
    auto* s = new httplib::SSLServer(cfg.tls_cert.c_str(), cfg.tls_key.c_str());
    if (!s->is_valid()) {
      log_line("  FATAL: cannot load the TLS certificate/key (" + cfg.tls_cert + ", " + cfg.tls_key + ")");
      delete s;
      return 2;
    }
    srv.reset(s);
#else
    log_line("  FATAL: tls_cert/tls_key are set but this build has no TLS support. Use the Linux server build, or put a");
    log_line("         TLS reverse proxy in front of this server (see the IT Admin Guide) and remove the two settings.");
    return 2;
#endif
  } else {
    srv.reset(new httplib::Server());
  }
  const int threads = cfg.max_clients + 32;  // one per live desktop app + headroom for normal requests
  srv->new_task_queue = [threads] { return new httplib::ThreadPool((size_t)threads); };
  srv->set_payload_max_length(1024 * 1024);
  srv->set_read_timeout(20, 0);
  srv->set_write_timeout(20, 0);
  srv->set_keep_alive_timeout(3);
  srv->set_default_headers({{"Server", "InvertedTaskFlow"},
                            {"Cache-Control", "no-store"},
                            {"X-Content-Type-Options", "nosniff"},
                            {"X-Frame-Options", "DENY"}});
  srv->set_logger([](const httplib::Request& req, const httplib::Response& res) {
    if (req.path == "/api/v1/sync" && res.status == 200) return;  // the instant-update channel would flood the log
    char line[400];
    snprintf(line, sizeof line, "  %-15s %-6s %-44.44s %d", req.remote_addr.c_str(), req.method.c_str(), req.path.c_str(), res.status);
    log_line(line);
  });
  srv->set_error_handler([](const httplib::Request&, httplib::Response& res) {
    if (res.body.empty()) {
      json e = {{"error", res.status == 404 ? "Not found" : "Request failed"}};
      if (res.status == 413) e["error"] = "Request too large";
      res.set_content(e.dump(), "application/json");
    }
  });
  srv->set_exception_handler([](const httplib::Request& req, httplib::Response& res, std::exception_ptr ep) {
    std::string what = "unknown";
    try { if (ep) std::rethrow_exception(ep); } catch (const std::exception& e) { what = e.what(); } catch (...) {}
    log_line("  ! " + req.method + " " + req.path + " -> " + what);
    res.status = 500;
    res.set_content(json{{"error", "Server error"}}.dump(), "application/json");
  });
  srv->Get("/", [](const httplib::Request&, httplib::Response& res) {
    res.set_content(TF_PRODUCT_NAME " Server " TF_VERSION " is running. Use the desktop app to connect.\n", "text/plain");
  });
  register_routes(*srv, *app);
  g_srv = srv.get();

  // ---- background jobs: reminders (10 min), daily backup, session clean-up, e-mail
  std::thread jobs([&] {
    int tick = 0;
    std::string digest_until = "23:59";
    {
      int dh = 8, dm = 30;
      if (sscanf(cfg.digest_time.c_str(), "%d:%d", &dh, &dm) == 2 && dh + 3 <= 23) {
        char b[16];
        snprintf(b, sizeof b, "%02d:%02d", (dh + 3) % 24, dm % 60);
        digest_until = b;
      }
    }
    auto next_backup_try = std::chrono::steady_clock::now();
    while (g_running) {
      try {
        if (tick % 600 == 10) app->run_reminders();
        if (tick % 30 == 15) {
          std::string today = today_local(), last;
          {
            std::lock_guard<std::recursive_mutex> lock(app->mu);
            last = jstr(app->db.one("SELECT value FROM meta WHERE key='last_backup_day'"), "value");
          }
          if (last != today && local_hm() >= cfg.backup_time && std::chrono::steady_clock::now() >= next_backup_try) {
            std::string err;
            if (app->run_backup(nullptr, &err)) {
              std::lock_guard<std::recursive_mutex> lock(app->mu);
              app->db.exec("INSERT INTO meta(key,value) VALUES('last_backup_day',?) "
                           "ON CONFLICT(key) DO UPDATE SET value=excluded.value", {today});
            } else {
              log_line("  !!! Daily backup failed: " + err + "  (next attempt in 1 hour)");
              next_backup_try = std::chrono::steady_clock::now() + std::chrono::hours(1);
            }
          }
        }
        // the morning digest, once a day (not on Sundays), when e-mail is set up
        // (only in the 3 hours after digest_time, so a server started late in the day does not send "good morning" at night)
        if (tick % 60 == 25 && cfg.email_enabled && cfg.digest_time != "off" && local_hm() >= cfg.digest_time &&
            local_hm() < digest_until) {
          std::string today = today_local(), last;
          {
            std::lock_guard<std::recursive_mutex> lock(app->mu);
            last = jstr(app->db.one("SELECT value FROM meta WHERE key='last_digest_day'"), "value");
            if (last != today)
              app->db.exec("INSERT INTO meta(key,value) VALUES('last_digest_day',?) ON CONFLICT(key) DO UPDATE SET value=excluded.value", {today});
          }
          std::tm l = local_tm(std::time(nullptr));
          if (last != today && l.tm_wday != 0) log_line("  Morning digest: " + std::to_string(app->run_digest()) + " e-mail(s) queued");
        }
        if (tick % 3600 == 120) {
          std::lock_guard<std::recursive_mutex> lock(app->mu);
          app->db.exec("DELETE FROM sessions WHERE expires_at < ?", {now_iso()});
          app->db.exec("DELETE FROM audit WHERE at < ?", {iso_after_seconds(-3LL * 365 * 86400)});
          // keep the database from growing without limit: read notifications after 90 days, all after a year,
          // e-mails that could never be delivered after 30 days, lost-access records after 60 days
          app->db.exec("DELETE FROM notifications WHERE (is_read=1 AND created_at < ?) OR created_at < ?",
                       {iso_after_seconds(-90LL * 86400), iso_after_seconds(-365LL * 86400)});
          app->db.exec("DELETE FROM email_outbox WHERE sent_at='' AND attempts>=6 AND created_at < ?", {iso_after_seconds(-30LL * 86400)});
          json old_rev = app->db.one("SELECT MAX(seq) AS s FROM revocations WHERE created_at < ?", {iso_after_seconds(-60LL * 86400)});
          if (jint(old_rev, "s")) {
            // apps that were away longer than that simply download everything again
            app->db.exec("DELETE FROM revocations WHERE seq <= ?", {old_rev["s"]});
            app->db.exec("UPDATE meta SET value=? WHERE key='revocations_pruned_seq'", {std::to_string(jint(old_rev, "s") + 1)});
          }
        }
      } catch (const std::exception& e) {
        log_line(std::string("  ! background job: ") + e.what());
      }
      std::this_thread::sleep_for(std::chrono::seconds(1));
      tick++;
    }
    // shutting down: release the held instant-update requests at once so the server stops within seconds
    {
      std::lock_guard<std::recursive_mutex> lock(app->mu);
      app->stopping = true;
    }
    app->changed();
  });
  start_mailer(*app);

  std::string scheme = tls ? "https" : "http";
  log_line("  Company   : " + cfg.company_name);
  log_line("  Database  : " + fs::absolute(cfg.db_path).string());
  log_line("  Backups   : " + fs::absolute(cfg.backup_dir).string() + "  (daily at " + cfg.backup_time + ", keep " +
           std::to_string(cfg.backup_keep) + ")");
  log_line("  Listening : " + scheme + "://" + cfg.bind + ":" + std::to_string(cfg.port) + (tls ? "  (TLS on)" : "  (no TLS)"));
  if (!tls && cfg.bind != "127.0.0.1")
    log_line("  NOTE: traffic is not encrypted. Enable TLS for production (IT Admin Guide, chapters 3 and 12).");
  int rc = 0;
  if (!srv->listen(cfg.bind, cfg.port)) {
    log_line("  FATAL: cannot listen on " + cfg.bind + ":" + std::to_string(cfg.port) +
             " - is another copy already running, or is the port in use? Change 'port' in server.ini.");
    rc = 3;
  }
  g_srv = nullptr;
  g_running = false;
  app->changed();
  stop_mailer();
  jobs.join();
  log_line("  Server stopped.");
  log_flush();
  return rc;
}

// ---------------------------------------------------------------- maintenance commands
static int cmd_backup(const Config& cfg) {
  auto app = open_app(cfg);
  std::string path, err;
  if (!app->run_backup(&path, &err)) {
    std::cerr << "  Backup FAILED: " << err << "\n";
    return 1;
  }
  return 0;  // run_backup already reported "Backup written: <file>"
}

static int cmd_digest(const Config& cfg) {
  if (!cfg.email_enabled) {
    std::cerr << "  E-mail is switched off (email_enabled = 0) - the digest is sent by e-mail only.\n";
    return 1;
  }
  auto app = open_app(cfg);
  std::cout << "  Morning digest: " << app->run_digest() << " e-mail(s) queued - the running server sends them within a minute.\n";
  return 0;
}

static int cmd_check(const Config& cfg) {
  auto app = open_app(cfg);
  std::lock_guard<std::recursive_mutex> lock(app->mu);
  std::string r = app->db.check(true);
  json c = app->db.one("SELECT (SELECT COUNT(*) FROM users) AS users, (SELECT COUNT(*) FROM departments) AS departments, "
                       "(SELECT COUNT(*) FROM projects) AS projects, (SELECT COUNT(*) FROM tasks) AS tasks, "
                       "(SELECT COUNT(*) FROM email_outbox WHERE sent_at='' AND attempts>=6) AS failed_mail");
  std::cout << "  Database   : " << fs::absolute(cfg.db_path).string() << "\n"
            << "  Integrity  : " << r << "\n"
            << "  Users " << jint(c, "users") << ", departments " << jint(c, "departments") << ", projects " << jint(c, "projects")
            << ", tasks " << jint(c, "tasks") << ", failed e-mails " << jint(c, "failed_mail") << "\n";
  return r == "ok" ? 0 : 1;
}

static int cmd_reset_password(const Config& cfg, const std::string& email) {
  auto app = open_app(cfg);
  std::lock_guard<std::recursive_mutex> lock(app->mu);
  json u = app->db.one("SELECT id, name, status FROM users WHERE email=?", {email});
  if (u.is_null()) {
    std::cerr << "  No account with e-mail " << email << "\n";
    return 1;
  }
  static const char* alpha = "ABCDEFGHJKLMNPQRSTUVWXYZabcdefghijkmnopqrstuvwxyz23456789";
  std::string pw;
  for (auto b : secure_random(12)) pw += alpha[b % 57];
  pw += "9";
  std::string salt;
  int it;
  std::string h = app->hash_password(pw, salt, it);
  Tx tx(app->db);
  app->db.exec("UPDATE users SET pw_salt=?,pw_hash=?,pw_iter=?,must_change_password=1,failed_logins=0,locked_until='',"
               "status=CASE WHEN status='DISABLED' AND global_role='ADMIN' THEN 'ACTIVE' ELSE status END WHERE id=?",
               {salt, h, it, u["id"]});
  app->db.exec("DELETE FROM sessions WHERE user_id=?", {u["id"]});
  app->audit(0, "cli_reset_password", "user", jint(u, "id"), email);
  tx.commit();
  std::cout << "  Temporary password for " << jstr(u, "name") << " <" << email << ">:  " << pw << "\n"
            << "  They must choose a new password at the next sign-in.\n";
  return 0;
}

static int cmd_test_email(const Config& cfg, const std::string& to) {
  std::string err;
  std::cout << "  Sending a test e-mail to " << to << " via " << cfg.smtp_host << ":" << cfg.smtp_port << " (" << cfg.smtp_security
            << ") ...\n";
  if (!smtp_send(cfg, to, "Inverted TaskFlow test e-mail", "E-mail notifications from Inverted TaskFlow are working.", err)) {
    std::cerr << "  FAILED: " << err << "\n";
    return 1;
  }
  std::cout << "  Sent.\n";
  return 0;
}

static void usage() {
  std::cout << TF_PRODUCT_NAME " Server " TF_VERSION "\n"
            << "  --config FILE           settings file (default: server.ini next to the program)\n"
            << "  --port N / --db FILE    override the port / database file\n"
            << "  --demo                  load demo people and tasks into a new database (testing only)\n"
            << "  --reset                 delete the database first (testing only)\n"
            << "  --backup-now            write a backup now (safe while the server is running)\n"
            << "  --check                 verify the database\n"
            << "  --reset-password EMAIL  set a temporary password for an account and print it\n"
            << "  --test-email ADDRESS    send a test e-mail using the e-mail settings\n"
            << "  --digest-now            queue today's morning digest e-mails now\n"
#ifdef _WIN32
            << "  --install-service       install as a Windows service (run as Administrator)\n"
            << "  --uninstall-service     remove the Windows service\n"
#endif
            << "  --version               print the version\n";
}

int main(int argc, char** argv) {
#ifdef _WIN32
  SetConsoleOutputCP(CP_UTF8);
  SetConsoleTitleA("Inverted TaskFlow Server");
  {  // a click in the console window would otherwise pause all console output ("Select" mode)
    HANDLE in = GetStdHandle(STD_INPUT_HANDLE);
    DWORD mode = 0;
    if (in != INVALID_HANDLE_VALUE && GetConsoleMode(in, &mode))
      SetConsoleMode(in, (mode & ~ENABLE_QUICK_EDIT_MODE) | ENABLE_EXTENDED_FLAGS);
  }
#endif
  std::cout.setf(std::ios::unitbuf);
  std::string exe_dir;
  {
    std::error_code ec;
#ifdef _WIN32
    char buf[MAX_PATH * 2];
    GetModuleFileNameA(nullptr, buf, sizeof buf);
    exe_dir = fs::path(buf).parent_path().string();
#else
    exe_dir = fs::canonical("/proc/self/exe", ec).parent_path().string();
#endif
  }
  std::string ini, action, action_arg;
  bool service = false;
  Config cfg;
  for (int i = 1; i < argc; i++) {
    std::string a = argv[i];
    auto next = [&]() -> std::string {
      if (i + 1 >= argc) { std::cerr << "  " << a << " needs a value\n"; std::exit(2); }
      return argv[++i];
    };
    if (a == "--config") ini = next();
    else if (a == "--service") service = true;
    else if (a == "--port" || a == "--db" || a == "--reset-password" || a == "--test-email") next();
    else if (a == "--demo" || a == "--reset" || a == "--backup-now" || a == "--check" || a == "--digest-now" || a == "--install-service" ||
             a == "--uninstall-service") {
    } else if (a == "--version" || a == "-v") {
      std::cout << TF_VERSION << "\n";
      return 0;
    } else if (a == "--help" || a == "-h" || a == "/?") {
      usage();
      return 0;
    } else {
      std::cerr << "  Unknown option " << a << "  (use --help)\n";
      return 2;
    }
  }
  // settings: --config, else server.ini next to the program, else in the current folder
  bool explicit_ini = !ini.empty();
  if (ini.empty()) {
    std::error_code ec;
    ini = fs::exists(fs::path(exe_dir) / "server.ini", ec) ? (fs::path(exe_dir) / "server.ini").string() : "server.ini";
  }
  // relative paths in the settings are relative to the settings file's folder
  {
    std::error_code ec;
    auto dir = fs::absolute(ini, ec).parent_path();
    if (!dir.empty()) fs::current_path(dir, ec);
    ini = fs::path(ini).filename().string();
  }
  if (!load_ini(ini, cfg, explicit_ini)) return 2;
  cfg.config_path = ini;
  for (int i = 1; i < argc; i++) {
    std::string a = argv[i];
    if (a == "--port" && i + 1 < argc) cfg.port = std::atoi(argv[++i]);
    else if (a == "--db" && i + 1 < argc) cfg.db_path = argv[++i];
    else if (a == "--demo") cfg.demo = true;
    else if (a == "--reset") cfg.reset = true;
    else if (a == "--config") i++;
    else if (a == "--backup-now" || a == "--check" || a == "--digest-now" || a == "--install-service" || a == "--uninstall-service") action = a;
    else if ((a == "--reset-password" || a == "--test-email") && i + 1 < argc) { action = a; action_arg = argv[++i]; }
  }
  try {
    // maintenance commands: flush the log queue before exiting so no line is lost
    auto done = [](int rc) { log_flush(); return rc; };
    if (action == "--backup-now") return done(cmd_backup(cfg));
    if (action == "--check") return done(cmd_check(cfg));
    if (action == "--digest-now") return done(cmd_digest(cfg));
    if (action == "--reset-password") return done(cmd_reset_password(cfg, action_arg));
    if (action == "--test-email") return done(cmd_test_email(cfg, action_arg));
#ifdef _WIN32
    if (action == "--install-service") return install_service();
    if (action == "--uninstall-service") return uninstall_service();
    if (service) {
      if (cfg.log_file.empty()) cfg.log_file = "logs\\server.log";
      return run_as_service([cfg] { int rc = run_server(cfg); log_flush(); return rc; }, [] { request_stop(); });
    }
#else
    if (!action.empty() || service) {
      std::cerr << "  " << (service ? "--service" : action) << " is only available on Windows\n";
      return 2;
    }
#endif
  } catch (const std::exception& e) {
    log_flush();
    std::cerr << "  FAILED: " << e.what() << "\n";
    return 1;
  }
  std::signal(SIGINT, on_signal);
  std::signal(SIGTERM, on_signal);
#ifndef _WIN32
  std::signal(SIGPIPE, SIG_IGN);
#endif
  int rc = run_server(cfg);
  log_flush();
  return rc;
}
