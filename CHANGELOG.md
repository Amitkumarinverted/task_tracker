# Changelog - Inverted TaskFlow

Each release is published with `release_to_git.bat`. Add a section for the new version
(same number as `TF_VERSION` in `common/version.h`) before releasing - its text becomes the
release notes that users see in the update window.

## [1.2.0] - 2026-10-31

Easier to read, status in one click, project health that explains itself.

- **Larger, clearer text.** Text was sized smaller than intended on Windows (Segoe UI came out at about 12 px);
  it is now 14 px for normal text and larger for titles and key figures, a little darker for secondary text, with
  plain section headings in the task panel. **Settings > Text size** offers Smaller, Standard, Larger and Largest;
  Ctrl + and Ctrl - change it too (Ctrl 0 goes back to standard). The app also redraws sharply when moved to a screen
  with other scaling.
- **Change the status in one click.** The status chip in My tasks, My team, the projects and the task panel opens
  the next steps for that task: In progress, On hold (with the reason), Mark complete - or, for the reviewer, approve
  or send back. Everyone who sees the task sees the change at once. People who may not change a task's status see
  who can. Work can now also be put on hold before it starts (waiting for parts, a decision...).
- **Why a project is On track, At risk or Late.** The reason is written under the progress bar, shown when hovering
  the badge and included in the Word status report ("SP3WBMS-2 is 2 days overdue"). The rules: Late when the target
  date has passed with open work, when open work is planned beyond the target, or a main task is more than 3 days
  overdue; At risk when a main task is up to 3 days overdue, a sub-task is overdue, or much more of the planned time
  has passed than work is done. (Before, a project whose last task was due within 3 days of the target was "At risk"
  although nothing was late - that rule is gone.)
- People in project lists show with their face and first name; board cards no longer let tags run into the task key.
- Server: work that is "To do" can be put on hold.

## [1.1.1] - 2026-10-31

Release tools: updates of the desktop app can be installed again.

- **Checksums in the release notes.** `release_to_git.bat` handed the three file names to the notes script in a form
  PowerShell reads as one name, so the notes of v1.1.0 (and earlier) had no checksums and the apps refused to install
  the update ("The release has no checksum"). Fixed; the script now also stops before publishing when the
  installer's checksum is missing, and reads the published release back to confirm it.
- **Repairing a published release.** New `fix_release_checksums.bat`: downloads the files of the latest (or a named)
  release, checks them against the copies built on this PC, and adds their checksums to the release notes - nothing
  is rebuilt or uploaded again.
- The app's message for such a release now says what to do.
- No changes to the server program beyond the version number.

## [1.1.0] - 2026-10-31

Teamwork release: boards and timelines everywhere, project members, more people on one task, dependencies.

- **Board and timeline views.** My tasks, My team, the department view and every project switch between List,
  Board and Timeline. On the board, drag a card to start it, put it on hold, mark it complete or approve it -
  each person can only move what they are allowed to. The team timeline shows each person's open work on one chart.
- **Project members.** Add people when you create a project, or later on its new People tab. People given a task in
  it by the project's owner, its managers or a member join automatically, and leave again when that work goes to
  someone else, moves or is cancelled; someone removed by hand comes back only when added again. Members see every
  task of the project, its board, timeline and progress, and act only on their own tasks. Only the project's own
  people (and its department) can add, propose, move or request work in it. The owner can hand the project over
  (Edit project > Owner).
- **More than one person on a task.** "Also working on it": those people see the task (with its main task and
  sub-tasks) in My tasks (Helping on), hear about its changes and comment; the person it is assigned to stays
  responsible and marks it complete. Helping on one task does not open the whole project.
- **Help from your whole department.** Ask a colleague of another team of your department for help; the department
  head approves, and both teams' heads are kept informed.
- **Dependencies.** "Can start when these are done": the timeline draws arrows and shows clashes in red, the task
  lists what it waits for, its owner is told when they can start (also when the awaited work is cancelled), and
  starting early asks for confirmation. Circles - also through a main task and its sub-tasks - are refused; a task
  cannot wait for a mere proposal. A task you cannot open shows as "a task you cannot see" and stays when you edit.
- **Timeline detail.** Milestones (diamonds), the first due date under bars whose date moved, today and target lines,
  and a timeline of all projects on the Projects page and in the department view.
- **Repeating tasks.** Daily, weekly or monthly (main tasks) - the next one is created when the current one is done;
  a monthly task due on the 31st stays on month ends.
- **@mentions** in comments notify that person (only people who can open the task).
- **Morning summary e-mail.** With e-mail switched on, each person gets one e-mail at 08:30 (Monday to Saturday)
  listing overdue work, what is due today and tomorrow and, for heads, the decisions waiting - only when there is
  something to act on (`digest_time` in the server settings; IT Admin Guide chapter 8).
- The Excel export also lists who helps, what a task waits for, the first due date, milestones and repeats.
- After this update every app downloads its data once, because people now see more than before.

## [1.0.2] - 2026-10-31

Release tools: publish the server to the repository the company server updates itself from.

- **Publishing the server.** New `release_server_to_git.bat` (also offered at the end of `release_to_git.bat`):
  test-builds the server for Linux, replaces only the TaskFlow server files in IT's repository, shows the
  changes and asks before pushing, then waits until the live server reports the new version.
- **Server updates from a folder.** `taskflowctl update` also takes a folder with the server source (such as
  a git clone), so an automatic update gets the database backup, build check and rollback of a manual one.
- **Installer build.** `build_installer.bat` installs Zig 0.16.0; 0.17.0 is not published for Windows.
- No changes to the desktop app or the server program beyond the version number.

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
