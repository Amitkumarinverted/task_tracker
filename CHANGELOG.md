# Changelog - Inverted TaskFlow

Each release is published with `release_to_git.bat`. Add a section for the new version
(same number as `TF_VERSION` in `common/version.h`) before releasing - its text becomes the
release notes that users see in the update window.

## [1.0.1] - 2026-10-29

Audit release: department drill-down for management, and the fixes from an independent review of every
server endpoint and every screen action.

- **Department view for management.** Click any department (Home or Company) to open its projects
  (including sub-departments and other teams' projects it works on), sub-departments, people and open
  tasks; a department report in Word; "Request work from this team". Projects opened from there lead back.
- **Approvals.** Items waiting 2 days or more are flagged; people pickers show each person's open and
  overdue work; Company > Departments shows the average review time per department.
- **Reliability and security (server).** A deeply nested request can no longer crash the server; sub-tasks
  can only be added to your own tasks or tasks of people you manage; deputies of Management can review
  department heads; without a Management head, department heads get a clear message instead of their work
  closing unreviewed; the head of Management can re-plan and cancel her own work; people who lose access to
  a task (reassignment) are told precisely, others no longer receive hidden ids or wake up for nothing;
  comment text only reaches people who can still open the task; a main task moved to another project takes
  its sub-tasks along; finished or cancelled tasks close their open extension and help requests; renamed
  projects reach everyone who works in them; departments with active sub-departments cannot be deactivated;
  one session can hold at most two instant-update connections and one person at most four; a busy server
  always tells the app how long to wait; the owner of a main task gets one message when its last sub-task
  is done.
- **Speed.** A full download leaves out work finished more than a year ago, but always keeps running
  projects, every project's main tasks and the complete breakdown of any open task; a closed project's
  history (and older routine work, on request) loads when it is opened. At 100,000 tasks a full download
  takes about 1.3 s for a member and 1.6 s for management; editing a project no longer makes the server
  check every task of it for every signed-in user; the health check never waits.
- **Notifications.** Requesters (and the requesting department's heads) hear about due-date changes,
  completion and cancellation; nobody is notified about their own action; overdue reminders taper off
  (day 1, 3, 7, then weekly); old notifications are cleaned up.
- **Desktop app.** One failed action no longer switches the app to "offline"; errors after a confirmation
  are shown; if someone else changes a task while you edit it, Save says what they changed, takes their
  change into the fields you did not touch and keeps your own entries; notifications
  open the exact place where the decision is taken; actions that cannot succeed are no longer offered;
  inactive departments stay visible in Administration; the update check never delays your own changes;
  the local copy of the data is deleted on sign-out; a notification about a task this PC has not loaded
  fetches and opens it; a withdrawn proposal says so instead of opening an empty page; a comment that
  could not be posted comes back into the box.
- **Admin.** Server page shows which desktop app versions are in use.
- **Code structure.** Server and app source split into the modules of the architecture (see docs/Code_Map.md).

## [1.0.0] - 2026-10-29

First company-wide release.

- **Projects with main tasks and sub-tasks.** Progress of a main task moves automatically as its
  sub-tasks are approved; project progress is the average of its main tasks. No manual sliders.
- **Completion with review.** "Mark complete" asks what was done (comment required, report link
  optional). The manager is notified and approves or sends it back with comments. The task counts
  as completed only after approval. The team member can withdraw the request until it is decided.
- **Departments and sub-departments.** Management, Electronics Engineering, Mechanical
  Engineering, Quality, Process, Production, Sales, Supply Chain Management, Service, IT, HR - each
  can have sub-departments with their own head. Sub-department heads manage their members; the
  department head manages the sub-department heads (two levels). Deputy managers act for the head.
- **Instant updates and reliable notifications.** Changes reach everyone involved within a second;
  missed notifications arrive after reconnecting; desktop notifications and optional e-mails.
- **Dashboards for every role:** members, sub-department heads, department heads, deputies,
  management (company view) and IT.
- **Word reports:** project status report and portfolio report for managers and management.
- **Security:** HTTPS with certificate pinning, account lockout, per-computer rate limits,
  password policy, forced password change after a reset, audit log, daily backups.
- **Automatic updates** of the desktop app from GitHub releases (checksum verified).
- Windows installer, Linux server installer, `taskflowctl` admin tool and the IT Admin Guide.
