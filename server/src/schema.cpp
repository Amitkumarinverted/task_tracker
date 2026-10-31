// Inverted TaskFlow Server - database schema and the change sequence (sync module)
#include "server.h"

#include <string>

namespace tf {

// ============================================================== schema
App::App(const Config& c) : cfg(c), db(c.db_path) {}

void App::init_schema() {
  db.exec_script(R"SQL(
CREATE TABLE IF NOT EXISTS meta(key TEXT PRIMARY KEY, value TEXT);
INSERT OR IGNORE INTO meta(key,value) VALUES('seq','0');
INSERT OR IGNORE INTO meta(key,value) VALUES('schema','2');  -- migrated below to the current version

CREATE TABLE IF NOT EXISTS users(
  id INTEGER PRIMARY KEY, email TEXT NOT NULL UNIQUE COLLATE NOCASE, name TEXT NOT NULL,
  phone TEXT DEFAULT '', designation TEXT DEFAULT '',
  pw_salt TEXT, pw_hash TEXT, pw_iter INTEGER, must_change_password INTEGER DEFAULT 0,
  status TEXT NOT NULL DEFAULT 'PENDING', global_role TEXT NOT NULL DEFAULT 'STAFF',
  requested_dept_id INTEGER, decision_note TEXT DEFAULT '',
  failed_logins INTEGER DEFAULT 0, locked_until TEXT DEFAULT '',
  scope_ver INTEGER DEFAULT 1, created_at TEXT, last_login TEXT DEFAULT '', seq INTEGER DEFAULT 0);
CREATE INDEX IF NOT EXISTS ix_users_seq ON users(seq);

CREATE TABLE IF NOT EXISTS sessions(
  token_hash TEXT PRIMARY KEY, user_id INTEGER NOT NULL REFERENCES users(id),
  created_at TEXT, expires_at TEXT, last_seen TEXT, client TEXT DEFAULT '', ip TEXT DEFAULT '');

CREATE TABLE IF NOT EXISTS departments(
  id INTEGER PRIMARY KEY, code TEXT NOT NULL UNIQUE, name TEXT NOT NULL,
  parent_id INTEGER REFERENCES departments(id), is_management INTEGER DEFAULT 0, sort INTEGER DEFAULT 0,
  active INTEGER DEFAULT 1, seq INTEGER DEFAULT 0);
CREATE INDEX IF NOT EXISTS ix_dept_seq ON departments(seq);

CREATE TABLE IF NOT EXISTS memberships(
  id INTEGER PRIMARY KEY, user_id INTEGER NOT NULL REFERENCES users(id),
  dept_id INTEGER NOT NULL REFERENCES departments(id), role TEXT NOT NULL DEFAULT 'MEMBER',
  active INTEGER DEFAULT 1, seq INTEGER DEFAULT 0, UNIQUE(user_id, dept_id));
CREATE INDEX IF NOT EXISTS ix_memb_seq ON memberships(seq);
CREATE INDEX IF NOT EXISTS ix_memb_dept ON memberships(dept_id, role);

CREATE TABLE IF NOT EXISTS projects(
  id INTEGER PRIMARY KEY, code TEXT NOT NULL UNIQUE, name TEXT NOT NULL, description TEXT DEFAULT '',
  owner_id INTEGER REFERENCES users(id), dept_id INTEGER REFERENCES departments(id),
  status TEXT NOT NULL DEFAULT 'ACTIVE', start_date TEXT DEFAULT '', target_end TEXT DEFAULT '',
  is_routine INTEGER DEFAULT 0, next_num INTEGER DEFAULT 1, created_at TEXT, seq INTEGER DEFAULT 0);
CREATE INDEX IF NOT EXISTS ix_proj_seq ON projects(seq);

CREATE TABLE IF NOT EXISTS tasks(
  id INTEGER PRIMARY KEY, key TEXT, project_id INTEGER NOT NULL REFERENCES projects(id),
  parent_id INTEGER REFERENCES tasks(id), title TEXT NOT NULL, description TEXT DEFAULT '',
  dept_id INTEGER NOT NULL REFERENCES departments(id), assignee_id INTEGER REFERENCES users(id),
  created_by INTEGER REFERENCES users(id), origin TEXT NOT NULL DEFAULT 'MANAGER',
  requesting_dept_id INTEGER, requested_by INTEGER, request_id INTEGER,
  priority TEXT NOT NULL DEFAULT 'MEDIUM', status TEXT NOT NULL DEFAULT 'TODO',
  start_date TEXT DEFAULT '', due_date TEXT DEFAULT '', original_due TEXT DEFAULT '',
  progress INTEGER DEFAULT 0, submitted_at TEXT DEFAULT '', completed_at TEXT DEFAULT '',
  created_at TEXT, updated_at TEXT, row_version INTEGER DEFAULT 1, seq INTEGER DEFAULT 0);
CREATE INDEX IF NOT EXISTS ix_tasks_seq ON tasks(seq);
CREATE INDEX IF NOT EXISTS ix_tasks_assignee ON tasks(assignee_id, status);
CREATE INDEX IF NOT EXISTS ix_tasks_dept ON tasks(dept_id, status);
CREATE INDEX IF NOT EXISTS ix_tasks_parent ON tasks(parent_id);
CREATE INDEX IF NOT EXISTS ix_tasks_project ON tasks(project_id);

CREATE TABLE IF NOT EXISTS completions(
  id INTEGER PRIMARY KEY, task_id INTEGER NOT NULL REFERENCES tasks(id), submitted_by INTEGER NOT NULL,
  note TEXT DEFAULT '', link TEXT DEFAULT '', status TEXT NOT NULL DEFAULT 'PENDING',
  decided_by INTEGER, decision_note TEXT DEFAULT '', decided_at TEXT DEFAULT '', created_at TEXT, seq INTEGER DEFAULT 0);
CREATE INDEX IF NOT EXISTS ix_compl_task ON completions(task_id);
CREATE INDEX IF NOT EXISTS ix_compl_seq ON completions(seq);

CREATE TABLE IF NOT EXISTS work_requests(
  id INTEGER PRIMARY KEY, type TEXT NOT NULL, status TEXT NOT NULL DEFAULT 'PENDING',
  project_id INTEGER REFERENCES projects(id), title TEXT NOT NULL, description TEXT DEFAULT '',
  requested_by INTEGER NOT NULL REFERENCES users(id), requesting_dept_id INTEGER,
  target_dept_id INTEGER NOT NULL REFERENCES departments(id), parent_task_id INTEGER,
  suggested_assignee_id INTEGER, needed_by TEXT DEFAULT '', priority TEXT DEFAULT 'MEDIUM',
  resulting_task_id INTEGER, decided_by INTEGER, decision_note TEXT DEFAULT '', decided_at TEXT DEFAULT '',
  created_at TEXT, seq INTEGER DEFAULT 0);
CREATE INDEX IF NOT EXISTS ix_req_seq ON work_requests(seq);

CREATE TABLE IF NOT EXISTS extensions(
  id INTEGER PRIMARY KEY, task_id INTEGER NOT NULL REFERENCES tasks(id),
  requested_by INTEGER NOT NULL REFERENCES users(id), current_due TEXT, proposed_due TEXT,
  reason TEXT DEFAULT '', status TEXT NOT NULL DEFAULT 'PENDING', decided_by INTEGER,
  decision_note TEXT DEFAULT '', decided_at TEXT DEFAULT '', created_at TEXT, seq INTEGER DEFAULT 0);
CREATE INDEX IF NOT EXISTS ix_ext_task ON extensions(task_id);
CREATE INDEX IF NOT EXISTS ix_ext_seq ON extensions(seq);

CREATE TABLE IF NOT EXISTS comments(
  id INTEGER PRIMARY KEY, task_id INTEGER NOT NULL REFERENCES tasks(id),
  author_id INTEGER REFERENCES users(id), kind TEXT NOT NULL DEFAULT 'COMMENT',
  body TEXT NOT NULL, created_at TEXT, seq INTEGER DEFAULT 0);
CREATE INDEX IF NOT EXISTS ix_comments_task ON comments(task_id);
CREATE INDEX IF NOT EXISTS ix_comments_seq ON comments(seq);

CREATE TABLE IF NOT EXISTS notifications(
  id INTEGER PRIMARY KEY, user_id INTEGER NOT NULL REFERENCES users(id), type TEXT,
  title TEXT, body TEXT DEFAULT '', task_id INTEGER, request_id INTEGER,
  is_read INTEGER DEFAULT 0, created_at TEXT, seq INTEGER DEFAULT 0);
CREATE INDEX IF NOT EXISTS ix_notif_user ON notifications(user_id, seq);

CREATE TABLE IF NOT EXISTS audit(
  id INTEGER PRIMARY KEY, at TEXT, actor_id INTEGER, action TEXT, entity TEXT, entity_id INTEGER,
  detail TEXT DEFAULT '');

CREATE TABLE IF NOT EXISTS email_outbox(
  id INTEGER PRIMARY KEY, to_email TEXT, subject TEXT, body TEXT, created_at TEXT, sent_at TEXT DEFAULT '',
  attempts INTEGER DEFAULT 0, next_try TEXT DEFAULT '', last_error TEXT DEFAULT '');
CREATE INDEX IF NOT EXISTS ix_outbox_pending ON email_outbox(sent_at, next_try);

CREATE TABLE IF NOT EXISTS reminders_sent(
  task_id INTEGER, kind TEXT, day TEXT, PRIMARY KEY(task_id, kind, day));

-- a task someone could see and no longer can (reassigned / moved): their app removes it on the next update
CREATE TABLE IF NOT EXISTS revocations(
  id INTEGER PRIMARY KEY, seq INTEGER NOT NULL, user_id INTEGER NOT NULL, entity TEXT NOT NULL, entity_id INTEGER NOT NULL,
  created_at TEXT);
CREATE INDEX IF NOT EXISTS ix_revocations_user ON revocations(user_id, seq);
INSERT OR IGNORE INTO meta(key,value) VALUES('revocations_pruned_seq','0');

-- v1.1: the people of a (non-routine) project - they see all of its work. Added by the owner / managers, or joined
-- automatically ("auto") when one of them gives the person a task there; "removed" = taken off by hand: never re-joined
-- automatically, only by adding them again
CREATE TABLE IF NOT EXISTS project_members(
  id INTEGER PRIMARY KEY, project_id INTEGER NOT NULL REFERENCES projects(id), user_id INTEGER NOT NULL REFERENCES users(id),
  active INTEGER DEFAULT 1, added_by INTEGER, created_at TEXT, seq INTEGER DEFAULT 0, auto INTEGER DEFAULT 0,
  removed INTEGER DEFAULT 0, UNIQUE(project_id, user_id));
CREATE INDEX IF NOT EXISTS ix_pm_user ON project_members(user_id, active);
CREATE INDEX IF NOT EXISTS ix_pm_seq ON project_members(seq);
-- v1.1: people who also work on a task (the assignee stays the one responsible)
CREATE TABLE IF NOT EXISTS task_members(
  id INTEGER PRIMARY KEY, task_id INTEGER NOT NULL REFERENCES tasks(id), user_id INTEGER NOT NULL REFERENCES users(id),
  active INTEGER DEFAULT 1, added_by INTEGER, created_at TEXT, seq INTEGER DEFAULT 0, UNIQUE(task_id, user_id));
CREATE INDEX IF NOT EXISTS ix_tm_user ON task_members(user_id, active);
CREATE INDEX IF NOT EXISTS ix_tm_task ON task_members(task_id);
CREATE INDEX IF NOT EXISTS ix_tm_seq ON task_members(seq);
-- v1.1: finish-to-start dependencies: task_id can start when depends_on is done
CREATE TABLE IF NOT EXISTS task_deps(
  id INTEGER PRIMARY KEY, task_id INTEGER NOT NULL REFERENCES tasks(id), depends_on INTEGER NOT NULL REFERENCES tasks(id),
  active INTEGER DEFAULT 1, created_at TEXT, seq INTEGER DEFAULT 0, UNIQUE(task_id, depends_on));
CREATE INDEX IF NOT EXISTS ix_dep_task ON task_deps(task_id);
CREATE INDEX IF NOT EXISTS ix_dep_on ON task_deps(depends_on);
CREATE INDEX IF NOT EXISTS ix_dep_seq ON task_deps(seq);
)SQL");
  // ---- migrations
  auto has_col = [&](const char* table, const char* col) {
    for (auto& r : db.query(std::string("PRAGMA table_info(") + table + ")"))
      if (jstr(r, "name") == col) return true;
    return false;
  };
  Tx mig(db);  // all of a migration, or none of it
  if (!has_col("project_members", "auto")) {
    db.exec("ALTER TABLE project_members ADD COLUMN auto INTEGER DEFAULT 0");
    db.exec("ALTER TABLE project_members ADD COLUMN removed INTEGER DEFAULT 0");
    db.exec("UPDATE project_members SET auto=1 WHERE added_by IS NULL AND user_id NOT IN (SELECT owner_id FROM projects p "
            "WHERE p.id=project_id AND owner_id IS NOT NULL)");
  }
  if (!has_col("tasks", "is_milestone")) db.exec("ALTER TABLE tasks ADD COLUMN is_milestone INTEGER DEFAULT 0");
  if (!has_col("tasks", "recur")) db.exec("ALTER TABLE tasks ADD COLUMN recur TEXT DEFAULT ''");  // '', DAILY, WEEKLY, MONTHLY
  if (!has_col("tasks", "recur_day")) {  // the day of the month a monthly series is due (31 stays 31 after February)
    db.exec("ALTER TABLE tasks ADD COLUMN recur_day INTEGER DEFAULT 0");
    db.exec("UPDATE tasks SET recur_day=CAST(substr(due_date,9,2) AS INTEGER) WHERE recur='MONTHLY' AND length(due_date)=10");
  }
  if (jint(db.one("SELECT CAST(value AS INTEGER) AS v FROM meta WHERE key='schema'"), "v") < 3) {
    // v1.1: everyone already working in a project (and its owner) becomes a project member; every app downloads
    // everything once, because people now see more than before
    const std::string now = now_iso();
    // (owners first: they are members in their own right; everyone else joined through their work)
    db.exec("INSERT OR IGNORE INTO project_members(project_id,user_id,active,created_at,seq,auto) "
            "SELECT id, owner_id, 1, ?, 0, 0 FROM projects WHERE is_routine=0 AND owner_id IS NOT NULL", {now});
    db.exec("INSERT OR IGNORE INTO project_members(project_id,user_id,active,created_at,seq,auto) "
            "SELECT DISTINCT t.project_id, t.assignee_id, 1, ?, 0, 1 FROM tasks t JOIN projects p ON p.id=t.project_id "
            "WHERE p.is_routine=0 AND p.status<>'CLOSED' AND t.assignee_id IS NOT NULL "
            "AND t.status NOT IN ('PROPOSED','REJECTED','CANCELLED')", {now});
    db.exec("UPDATE users SET scope_ver=scope_ver+1");
    db.exec("UPDATE meta SET value='3' WHERE key='schema'");
  }
  mig.commit();
  db.exec("INSERT OR IGNORE INTO meta(key,value) VALUES('company_name',?)", {cfg.company_name});
  db.exec("INSERT OR IGNORE INTO meta(key,value) VALUES('company_tagline',?)", {cfg.company_tagline});
  db.exec("INSERT OR IGNORE INTO meta(key,value) VALUES('created_at',?)", {now_iso()});
  // future schema migrations go here:  if (schema < 4) { ...; UPDATE meta SET value='4' ... }
}

int64_t App::next_seq() {
  db.exec("UPDATE meta SET value = CAST(value AS INTEGER) + 1 WHERE key='seq'");
  return jint(db.one("SELECT CAST(value AS INTEGER) AS v FROM meta WHERE key='seq'"), "v");
}
int64_t App::current_seq() { return jint(db.one("SELECT CAST(value AS INTEGER) AS v FROM meta WHERE key='seq'"), "v"); }

}  // namespace tf
