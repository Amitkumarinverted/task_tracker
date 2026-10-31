# Inverted TaskFlow - IT Admin Guide

Version 1.1.0 - for the IT administrator of Inverted Energy Pvt. Ltd.

This guide explains how to install, secure, run, back up and update the Inverted TaskFlow server.
Most work is automated: one script installs everything, and one tool (`taskflowctl`) does the daily jobs.

## 1. What TaskFlow is made of

TaskFlow has two parts:

| Part | Where it runs | What it does |
|---|---|---|
| TaskFlow Server | one company server or VM (Ubuntu Linux recommended) | stores all data in one SQLite database, checks every permission, sends notifications and e-mails, makes daily backups |
| TaskFlow desktop app | every employee's Windows 10/11 PC | the user interface; it shows only what the person is allowed to see and keeps a local copy for speed |

How they talk:

- The app connects to the server over **HTTPS on port 8443** (TLS 1.2/1.3). Nothing else needs to be open.
- The app keeps one connection open per PC ("instant updates"). When someone changes something, the server answers that connection within milliseconds, so everyone involved sees the change at once.
- The app recognises the server by its **certificate fingerprint** (certificate pinning). Even with a self-signed certificate, nobody can impersonate the server.
- Desktop apps update themselves from the company's private GitHub repository (read-only token, SHA-256-checked installer). The server never needs internet access, except for e-mail if you use an external mail service.

Sizing: 2 vCPU, 2-4 GB RAM and 20 GB disk serve several hundred users. The database grows by roughly 50-100 MB per year for a 200-person company.

## 2. Before you start

You need:

- An **Ubuntu Server 22.04 or 24.04 LTS** machine or VM (Debian 12 also works) with a **fixed IP address** or a DNS name (for example `taskflow.inverted.local`).
- `sudo` rights on it and SSH access.
- Port **8443/tcp** reachable from the office network (and VPN, if people work remotely). Do **not** expose it to the internet unless you have a firewall rule that limits who can reach it.
- The server package: `InvertedTaskFlow_Server_v1.1.0.zip`.

## 3. Installation (about 10 minutes)

1. Copy the zip to the server (for example with WinSCP) and unzip it:

```
unzip InvertedTaskFlow_Server_v1.1.0.zip
cd InvertedTaskFlow_Server_v1.1.0/deploy/linux
sudo bash install_server.sh
```

2. The script asks two questions: the **server name or IP** the PCs will use, and the **e-mail of the first administrator**.
3. It then installs the build tools, compiles the server, creates the locked-down service account `taskflow`, creates the HTTPS certificate, writes the settings, starts the service and opens port 8443 in the `ufw` firewall.
4. At the end it prints three things. **Write them down**:

| Printed value | Who needs it |
|---|---|
| Server address, e.g. `https://taskflow.inverted.local:8443` | the person who builds the desktop installer |
| Certificate fingerprint (64 hex characters, `AB:CD:...`) | the same person - it is built into the installer |
| Where the first administrator password is | you |

5. Read the first password: `sudo cat /var/lib/taskflow/initial_admin_password.txt`

You can run the installer again at any time - it keeps the database, the settings and the certificate.

Non-interactive installation (for scripts):

```
sudo bash install_server.sh --host taskflow.inverted.local --admin it@inverted.in --yes
```

If you have a certificate from your own company CA, add `--cert cert.pem --key key.pem`.

### Where things are

| Path | Contents |
|---|---|
| `/opt/taskflow/TaskFlowServer` | the program (`TaskFlowServer.previous` = the version before the last update) |
| `/etc/taskflow/server.ini` | settings |
| `/etc/taskflow/cert.pem`, `key.pem` | HTTPS certificate and private key (readable only by root and the service) |
| `/var/lib/taskflow/taskflow.db` | the database (all data) |
| `/var/lib/taskflow/backups/` | daily backups |
| `/var/log/taskflow/server.log` | log (rotated at 10 MB, 5 files kept) |
| `/usr/local/bin/taskflowctl` | the admin tool |

## 4. First sign-in and setting up the organisation

1. Install the desktop app on your PC (see chapter 5) and sign in with the administrator e-mail and the password from step 5 above.
2. The app asks you to choose your own password. Then delete the file: `sudo rm /var/lib/taskflow/initial_admin_password.txt`
3. The 11 departments already exist: Management, Electronics Engineering, Mechanical Engineering, Quality, Process, Production, Sales, Supply Chain Management, Service, IT, HR. Each has a "Routine" project for day-to-day work.
4. Employees open the app and click **Request an account**. You approve them in **Administration > Accounts**: choose their department (or sub-department), their role in it (Member, Deputy manager, Head) and their access (Staff, Management, IT administrator).
5. In **Administration > Departments**:
   - make sure every department has a **Head** (and, if wanted, a **Deputy manager**);
   - add **sub-departments** (for example Production > Line 1 Assembly) with their own head.

How the two management levels work:

| Person | Whose work they approve |
|---|---|
| Member of a sub-department | - (their work is approved by the sub-department head) |
| Sub-department head | members of the sub-department |
| Department head (and deputy) | sub-department heads, and anyone directly in the department; may also decide for sub-department members |
| Head of the Management department (and its deputies) | department heads; the head of Management closes her own work herself |
| Management department / Management access | sees everything in the company (read and report); can request work from any team |

## 5. The desktop installer

The installer is built on a Windows PC with `build_installer.bat` (in the project folder). The first time it asks for:

1. the **server address** from chapter 3,
2. the **certificate fingerprint** from chapter 3,
3. the GitHub repository and a **read-only token** for automatic updates.

These values are stored only on that PC (`%APPDATA%\InvertedEnergy\TaskFlow`, the token encrypted with Windows DPAPI) and are built into the installer, so users never have to type a server address.

Users run `InvertedTaskFlow_Setup_v1.1.0.exe`. It installs per user (no administrator rights needed) into `%LOCALAPPDATA%\Programs\Inverted TaskFlow`, adds a Start-menu entry and, by default, starts TaskFlow with Windows in the tray so notifications always arrive.

You can also install it silently, for example with Group Policy or Intune, while the user is signed in:

```
InvertedTaskFlow_Setup_v1.1.0.exe /VERYSILENT /SUPPRESSMSGBOXES /NORESTART
```

Windows SmartScreen may warn about the installer the first time because it is not code-signed. If your company has a code-signing certificate, sign the setup file with `signtool` before distributing it.

## 6. Daily operations with taskflowctl

`taskflowctl` asks for your sudo password when it needs it, so `taskflowctl status` and `sudo taskflowctl status` do the same.

| Command | What it does |
|---|---|
| `taskflowctl status` | running? health, version, database size, last backup, free disk |
| `taskflowctl logs` / `taskflowctl logs -f` | last 200 log lines / follow the log |
| `sudo taskflowctl restart` | restart (users reconnect automatically within seconds) |
| `sudo taskflowctl backup` | write a backup now |
| `taskflowctl backups` | list the backups |
| `sudo taskflowctl restore FILE` | restore a backup (keeps the current database as `.before-restore-...`) |
| `sudo taskflowctl check` | full database integrity check |
| `sudo taskflowctl reset-password name@inverted.in` | temporary password; the user must change it at the next sign-in |
| `taskflowctl fingerprint` | print the certificate fingerprint again |
| `sudo taskflowctl test-email you@inverted.in` | test the e-mail settings |
| `sudo taskflowctl digest` | send today's morning summary e-mails now (normally automatic, chapter 8) |
| `sudo taskflowctl update PACKAGE.zip` | install a new server version (chapter 9) |
| `sudo taskflowctl update FOLDER` | the same from a folder with the server source, e.g. the git clone of chapter 9 |
| `sudo taskflowctl edit` | edit the settings and restart |

The service restarts itself automatically after a crash (systemd `Restart=always`) and starts with the server.

Things worth watching (monthly is enough):

- `taskflowctl status` shows a recent backup and enough free disk.
- `grep '!!!' /var/log/taskflow/server.log` - lines with `!!!` mean a failed backup or a failed database check.
- Unknown sign-in attempts: **Administration > Audit log** in the app lists every important action (approvals, account changes, lockouts, backups).

## 7. Backups and restore

- The server writes a **complete, consistent backup every day at 02:00** (`backup_time`) into `/var/lib/taskflow/backups/` and keeps the last **30** (`backup_keep`). A backup is safe while people work.
- If the server was off at 02:00, the backup is made as soon as it runs again that day.
- In the app, an administrator can click **Administration > Server > Back up now**.

**Copy the backups to another machine.** A backup on the same disk does not protect against a disk failure. For example, a nightly copy to a Windows file share:

```
sudo apt install cifs-utils
# /etc/cron.d/taskflow-offsite  (runs at 03:00)
0 3 * * * root rsync -a --delete /var/lib/taskflow/backups/ /mnt/nas/taskflow-backups/
```

To restore:

```
taskflowctl backups
sudo taskflowctl restore /var/lib/taskflow/backups/taskflow-20261028-020000.db
```

The tool checks that the backup is intact before it replaces anything and keeps the current database next to it.

**Disaster recovery on a new server:** install as in chapter 3 using the same host name, copy `/etc/taskflow/` (settings and certificate) from your safe copy, then restore the latest backup. If you cannot recover the old certificate, a new one is created - then publish a new installer with the new fingerprint (or users confirm the new fingerprint once when they sign in).

## 8. E-mail notifications (optional)

The app already shows notifications instantly and on the desktop. E-mail is an extra for important events: new task, review requested, approved or sent back, extension decisions, overdue reminders, account approved.

Edit the settings (`sudo taskflowctl edit`):

| Mail service | Settings |
|---|---|
| Microsoft 365 | `smtp_host = smtp.office365.com`, `smtp_port = 587`, `smtp_security = starttls`, a licensed mailbox (or SMTP AUTH enabled for it) as `smtp_user`/`smtp_password` |
| Google Workspace | `smtp_host = smtp.gmail.com`, `smtp_port = 587`, `smtp_security = starttls`, an app password |
| Internal relay | `smtp_host = mailrelay.inverted.local`, `smtp_port = 25`, `smtp_security = none` |

Set `smtp_from` (the sender address, usually the same mailbox) and `email_enabled = 1`, save, and test: `sudo taskflowctl test-email you@inverted.in`

E-mails are queued in the database first, so nothing is lost while the mail server is down; failed mails are retried after 1, 5, 15, 60 and 240 minutes.

**Morning summary.** With e-mail on, every person who has something to act on gets one e-mail a day, Monday to Saturday at `digest_time` (default `08:30`): their overdue tasks, what is due today and tomorrow, and - for heads and management - how many decisions wait for them and how many tasks of their team are overdue. People with nothing due get nothing. It is sent once a day, within 3 hours after `digest_time` (a server started later that day skips it). `digest_time = off` switches it off; `sudo taskflowctl digest` sends it straight away for a test.

## 9. Updating the server

New versions are published by `release_to_git.bat` as a GitHub release. Each release contains `InvertedTaskFlow_Server_vX.Y.Z.zip`.

```
sudo taskflowctl update InvertedTaskFlow_Server_vX.Y.Z.zip
```

The tool builds the new version, **backs up the database**, swaps the program, restarts and checks that it answers. If it does not start, it **rolls back** automatically. The database format is upgraded automatically when needed.

Update the server first, then publish the desktop update. The server tells older apps when they must update (`min_client_version`).

When a new version changes the database (1.1.0 adds project members, people helping on a task and dependencies), the server upgrades it by itself at the first start - `taskflowctl update` has already taken a backup. After the 1.1.0 upgrade every desktop app downloads its data once (about 1 to 2 seconds per PC), because people now see more than before. The upgrade makes everyone who already has work in a running project a member of it (they then see all of that project's work); project owners can review this on each project's People tab. Older apps keep working against the new server until they update.

### Automatic updates from a GitHub repository

The developer can publish the server straight to a GitHub repository with `release_server_to_git.bat`. That script test-builds the server first and changes only the TaskFlow server files in the repository (`server/`, `common/`, `third_party/`, `deploy/linux/`, `tests/`, the changelog and this guide); anything else you keep there is left alone. Every commit is titled `TaskFlow server vX.Y.Z`.

If the server updates itself when the repository changes, let the job end with `taskflowctl update` on the pulled folder, so every update gets the same protection as a manual one (database backup, nothing changes when the build fails, automatic rollback when the new version does not start):

```
cd /srv/task_tracker            # your clone of the repository
git pull --ff-only
sudo taskflowctl update /srv/task_tracker
```

`taskflowctl` replaces itself from the repository on every update. The first time only, install the version that understands folders by hand:

```
sudo install -m 0755 /srv/task_tracker/deploy/linux/taskflowctl /usr/local/bin/taskflowctl
```

`release_server_to_git.bat` then waits until `https://<server>/api/v1/health` reports the new version and tells the developer if it did not arrive within 10 minutes.

## 10. Security

What is already in place:

- **Encryption:** HTTPS (TLS 1.2+) with an elliptic-curve certificate; the app pins the certificate fingerprint and refuses any other certificate before it sends a password or session token.
- **Accounts:** an administrator approves every new account; passwords need 8+ characters with letters and numbers and are stored only as salted PBKDF2-SHA256 hashes (120,000 rounds); 5 wrong passwords lock the account for 15 minutes; 20 failures from one computer block that computer for 10 minutes.
- **Sessions:** random 256-bit tokens, stored only as hashes; they expire after 12 hours (30 days with "keep me signed in"); changing or resetting a password signs out the other sessions; disabled accounts are signed out at once. On the PCs the token is encrypted with Windows DPAPI.
- **Permissions:** every request is checked on the server: people only see the tasks of their own teams, the sub-departments they manage, and work they asked for; only the responsible managers can approve.
- **Abuse protection:** at most 30 requests per second per computer (`rate_limit_per_ip`), 1 MB per request, JSON nested no deeper than 8 levels, at most 2 waiting instant-update connections per session and 4 per person, 5 account requests per hour per computer, all text input length-checked, all database access parameterised (no SQL injection), report links must be `http(s)://` links.
- **Service isolation:** the server runs as the unprivileged `taskflow` user in a systemd sandbox (read-only system, private /tmp, no new privileges, no capabilities). It can only write its data and log folders.
- **Audit log:** approvals, account changes, lockouts, password resets and backups are recorded (kept 3 years).

What you should do:

1. Keep Ubuntu patched: `sudo apt install unattended-upgrades` (security updates install automatically).
2. Enable the firewall with only SSH and TaskFlow open:

```
sudo ufw allow OpenSSH
sudo ufw allow 8443/tcp
sudo ufw enable
```

3. Allow SSH only with keys (`PasswordAuthentication no` in `/etc/ssh/sshd_config`).
4. Copy backups to another machine (chapter 7) and test a restore twice a year.
5. Keep the GitHub token **read-only** and limited to the TaskFlow repository; renew it before it expires (`build_installer.bat setup`, then publish a release).
6. Remove accounts of people who leave: **Administration > Accounts > Manage > Disable account**.

## 11. Troubleshooting

| Symptom | What to check |
|---|---|
| App says "Server not reachable" | `taskflowctl status`; from the PC: `Test-NetConnection taskflow.inverted.local -Port 8443` in PowerShell; firewall; DNS name |
| App says the certificate does not match | Did you create a new certificate? Compare `taskflowctl fingerprint` with the one in the installer. If someone else answers on that address, stop and investigate. |
| Service does not start | `journalctl -u taskflow -n 50` and `taskflowctl logs`; a typo in `server.ini` is reported there |
| "port in use" | another program uses 8443 - change `port` in `server.ini` (and rebuild the installer with the new address) |
| Database check failed | `sudo taskflowctl check`; restore the latest good backup (chapter 7) |
| E-mails do not arrive | `sudo taskflowctl test-email ...` shows the mail server's answer; check the spam folder; **Administration > Server** shows failed e-mails |
| "Nobody can review this yet" when a department head marks work complete | The Management department has no head. Assign one in **Administration > Departments** (Management > + Person > Head). |
| A user is locked out | wait 15 minutes, or `sudo taskflowctl reset-password name@inverted.in` |
| Everybody is slow | `taskflowctl status` (disk, load); `max_clients` must be larger than the number of PCs |
| Wrong dates (overdue too early/late) | server time zone: `timedatectl set-timezone Asia/Kolkata` |

## 12. Windows Server instead of Linux

Linux is recommended (built-in HTTPS, e-mail over TLS, sandboxing). If you must use Windows Server 2016 or newer:

1. Copy the `windows` folder of the package to the server.
2. Right-click `install_service.bat` > **Run as administrator**. It installs to `C:\InvertedTaskFlow`, opens port 8080 for domain/private networks, installs the service `InvertedTaskFlow` (automatic start, restarts after failures) and starts it.
3. The first administrator password is in `C:\InvertedTaskFlow\initial_admin_password.txt`.
4. The Windows build has **no built-in HTTPS**. Either use it only inside a trusted office network (address `http://SERVERNAME:8080`), or put the free **Caddy** web server in front of it for HTTPS (see `Caddyfile.example`).
5. Maintenance: `backup_now.bat`, `check_database.bat`, `reset_password.bat`; backups in `C:\InvertedTaskFlow\backups`. Copy them to another machine.

## 13. Settings reference (server.ini)

| Setting | Default | Meaning |
|---|---|---|
| `bind`, `port` | 0.0.0.0, 8443 | network address and port (the locked-down Linux service cannot use ports below 1024, such as 443) |
| `tls_cert`, `tls_key` | /etc/taskflow/*.pem | HTTPS certificate and key (empty = plain HTTP) |
| `max_clients` | 300 | PCs connected at the same time |
| `rate_limit_per_ip` | 30 | requests per second per computer |
| `database` | /var/lib/taskflow/taskflow.db | database file |
| `backup_dir`, `backup_time`, `backup_keep` | backups, 02:00, 30 | daily backup folder, time and number kept |
| `log_file` | /var/log/taskflow/server.log | log file |
| `company_name`, `company_tagline` | Inverted Energy Pvt. Ltd., Projects - Tasks - Timelines | shown on the sign-in page |
| `session_hours`, `remember_days` | 12, 30 | session lifetime |
| `admin_email`, `admin_password` | | first administrator (only when the database is created) |
| `min_client_version` | 1.0.0 | older desktop apps are asked to update |
| `email_enabled`, `smtp_*` | off | e-mail notifications (chapter 8) |
| `digest_time` | 08:30 | morning summary e-mail, Monday to Saturday (`off` = none); needs e-mail on |

## 14. Command-line options of the server program

| Option | Meaning |
|---|---|
| `--config FILE` | settings file |
| `--backup-now` | write a backup and exit |
| `--check` | database integrity check |
| `--reset-password EMAIL` | temporary password for an account |
| `--test-email ADDRESS` | send a test e-mail |
| `--digest-now` | queue today's morning summary e-mails |
| `--install-service`, `--uninstall-service` | Windows service |
| `--demo --reset` | demo data in a new database - testing only, never on the real server |
| `--version` | print the version |

## 15. Contacts and records

Fill in for your company:

| Item | Value |
|---|---|
| Server name / IP | |
| Certificate fingerprint | |
| Where off-site backups are | |
| GitHub repository (desktop releases) | |
| GitHub repository the server updates from | |
| Token expiry date | |
| Responsible IT person | |
