# Inverted TaskFlow Server - Docker deployment

This is an alternative to `deploy/linux/install_server.sh`. It runs the same `TaskFlowServer` program, the same
SQLite database and the same HTTPS-with-pinned-certificate set-up, inside one container.
**Use one or the other on a server, never both on the same database.**

## 1. What runs where

```
 Desktop apps ──HTTPS :TASKFLOW_PORT (default 8443)──►  container "taskflow-server"  (user taskflow, uid 10001)
                                                        /usr/local/bin/entrypoint.sh  → checks, then exec
                                                        /opt/taskflow/TaskFlowServer --config /etc/taskflow/conf/server.ini
   host folder TASKFLOW_CONFIG_DIR  ──read-only──►  /etc/taskflow/conf   (server.ini)
   host folder TASKFLOW_TLS_DIR     ──read-only──►  /etc/taskflow/tls    (cert.pem, key.pem)
   host folder TASKFLOW_DATA_DIR    ──read-write─►  /var/lib/taskflow    (taskflow.db, -wal, -shm, backups/,
                                                                          initial_admin_password.txt)
```

- The image contains only the compiled server, OpenSSL runtime libraries, CA certificates, time-zone data and `curl` (for the
  health check): no source code, no compiler, no settings, no keys.
- The container file system is read-only, all Linux capabilities are dropped, `no-new-privileges` is set; the only writable
  places are the data folder and a small `/tmp` (SQLite temporary files).
- The server reads **only `server.ini`** - it has no environment-variable settings. The `.env` file only controls Docker
  (image, port, host folders, user, time zone, resources). The container's `server.ini` is
  [`deploy/docker/server.ini`](deploy/docker/server.ini): it differs from the Linux one only in its paths and `log_file =`
  (empty: the log goes to `docker compose logs`, rotated by Docker at 10 MB x 5).
- Settings must use **absolute paths**: the server changes into the folder of `server.ini` (read-only) and resolves relative
  paths from there.
- Logs: `docker compose logs` (no log volume is needed).
- Health check: `GET /api/v1/health` - the server's own endpoint, which never waits for the database. It shows that the
  server answers HTTPS; it does not test the database (use `--check` for that).

### What the entrypoint checks before it starts the server

It refuses to start (and says why in the log) when: it runs as root; `--reset`, `--demo`, `--config`, `--db` or `--port`
are passed; `server.ini` is missing or unreadable; `demo_data` is on; `bind` is a loopback address; `port` is not 8443;
`database` / `backup_dir` / `log_file` are not absolute; the data or backup folder is not writable; the database or its
`-wal`/`-shm` files are not writable; only one of `tls_cert`/`tls_key` is set, or either file is missing or unreadable;
TLS is off without `TASKFLOW_ALLOW_HTTP=1`; **the database is missing although backups exist** (wrong mount or deleted
file - set `TASKFLOW_ALLOW_NEW_DB=1` once only if you really want an empty database). It never creates, copies, migrates
or deletes the database - the server does that itself (`CREATE TABLE IF NOT EXISTS`; an existing database is only opened).
Passwords and keys are never printed.

## 2. Settings

| `.env` variable | Default | Effect |
|---|---|---|
| `TASKFLOW_IMAGE`, `TASKFLOW_TAG` | `inverted/taskflow-server`, `1.0.1` | image name and tag that is built and run |
| `TASKFLOW_HOST` | `task.inverted.in` | the name the desktop apps use. Used **only by the entrypoint**: it refuses to start if the certificate does not cover this name, and logs `https://TASKFLOW_HOST:TASKFLOW_PORT` and the certificate fingerprint. It does not change what the server listens on, and it does not set up DNS or create a certificate |
| `TASKFLOW_PORT` | `8443` | port on the **host** (also the port in the logged client address); the container side is always 8443 |
| `TASKFLOW_BIND_IP` | `0.0.0.0` | host address the port is published on (set the office-network IP to limit it) |
| `TASKFLOW_CONFIG_DIR` | `./taskflow-data/config` | host folder with `server.ini` → `/etc/taskflow/conf` (read-only) |
| `TASKFLOW_TLS_DIR` | `./taskflow-data/tls` | host folder with `cert.pem`, `key.pem` → `/etc/taskflow/tls` (read-only) |
| `TASKFLOW_DATA_DIR` | `./taskflow-data/data` | host folder for database, WAL files, backups → `/var/lib/taskflow` (writable) |
| `TASKFLOW_UID`, `TASKFLOW_GID` | `10001` | user the server runs as; must own the data folder and be able to read `key.pem`. A change requires `docker compose build` |
| `TASKFLOW_TZ` | `Asia/Kolkata` | time zone for "today", due/overdue reminders and `backup_time` |
| `TASKFLOW_CPUS`, `TASKFLOW_MEMORY` | `2`, `1g` | container limits (also: max 1024 processes/threads; the server uses `max_clients` + 32 threads) |
| `BUILD_JOBS` | `2` | parallel compile jobs (each needs up to ~0.7 GB RAM) |
| `TASKFLOW_ALLOW_HTTP` | `0` | `1` = allow starting without TLS (only behind your own TLS reverse proxy) |
| `TASKFLOW_ALLOW_NEW_DB` | `0` | `1` = allow an empty new database although backups exist |

| `server.ini` (container) | Value | Note |
|---|---|---|
| `bind`, `port` | `0.0.0.0`, `8443` | do not change (checked) |
| `tls_cert`, `tls_key` | `/etc/taskflow/tls/cert.pem`, `.../key.pem` | from `TASKFLOW_TLS_DIR` |
| `database` | `/var/lib/taskflow/taskflow.db` | its folder also holds `-wal`, `-shm` and `initial_admin_password.txt` |
| `backup_dir` | `/var/lib/taskflow/backups` | inside the data volume |
| `log_file` | empty | container output |
| everything else | as in the IT Admin Guide, chapter 13 | `smtp_password` belongs only in this file - keep it `0640 root:10001` |

## 3. Production name: `https://task.inverted.in:8443`

Three separate things must agree; the server program itself has no host-name setting (it listens on `0.0.0.0:8443`
inside the container and answers whatever name was used):

| What | Where it is configured | Production value |
|---|---|---|
| DNS record | your DNS provider for `inverted.in` (not in this project) | `task.inverted.in` → the server's IP (an `A` record) |
| TLS certificate | `TASKFLOW_TLS_DIR/cert.pem` + `key.pem`, loaded by the server from `tls_cert`/`tls_key` | covers `DNS:task.inverted.in` |
| Desktop app address + pin | `build_installer.bat setup` (`SERVER_URL`, `SERVER_CERT_SHA256`) | `https://task.inverted.in:8443` + the certificate fingerprint |

`TASKFLOW_HOST` / `TASKFLOW_PORT` in `.env` only let the container check the certificate and print these values.

### DNS

- Create (or check) an `A` record `task.inverted.in` → the server's IP. At the time of writing, `task.inverted.in`
  already resolves publicly to **15.207.128.179** - make sure that is the machine that runs this container.
- That is a public address: the port is reachable from the internet unless the cloud security group / firewall blocks it
  (see *Firewall*). For an office-only service, allow only the office and VPN address ranges.
- If the server should only be reached inside the office/VPN, use an internal DNS zone (split DNS) with the private IP
  instead of a public record.
- Check from the server and from an employee PC:
  ```bash
  nslookup task.inverted.in            # Windows and Linux
  dig +short task.inverted.in          # Linux
  ```
  PowerShell on a PC: `Resolve-DnsName task.inverted.in` and `Test-NetConnection task.inverted.in -Port 8443`

### HTTPS certificate

The C++ server terminates TLS itself (OpenSSL, TLS 1.2/1.3); no reverse proxy is used or needed.

- The desktop app trusts the server **only by the pinned SHA-256 fingerprint of the whole certificate** (when a pin is
  built in, it does not check the CA, the name or the expiry date - `client/src/platform_win32.cpp`). So:
  - Use a **long-lived** certificate: the self-signed one below (10 years, like `install_server.sh`) or one from the company
    CA. Do **not** use a 90-day public certificate (e.g. Let's Encrypt): every renewal changes the fingerprint and every
    installed app would refuse to connect until it is reinstalled.
  - Still put `task.inverted.in` into the certificate (`subjectAltName`) - the entrypoint checks it, and tools such as
    `curl`, or an app installed without a pin, verify the name.
- Never disable the pin or switch to plain HTTP to make a connection work - compare fingerprints instead.

### Desktop installer (built on the Windows PC with the client project, `D:\InvertedTaskFlow`)

```bat
build_installer.bat setup
   Server address              : https://task.inverted.in:8443
   Certificate fingerprint     : <the 64-hex value from "docker compose logs" or the openssl command in step 5>
```

The values are stored only on that PC (`%APPDATA%\InvertedEnergy\TaskFlow\release_config.txt`) and built into
`InvertedTaskFlow_Setup_vX.Y.Z.exe`; `installer\make_build_config.ps1` accepts addresses of the form
`https://name:port`. Distribute that installer. Already-installed apps can also be pointed at the new address once on
the sign-in screen (server settings: address + fingerprint), but a new installer is the reliable way.

### If the certificate (fingerprint) changes

A new key, a renewed certificate or a certificate from another CA all produce a **new fingerprint**:

1. Plan it: create the new certificate first, note its fingerprint, rebuild the installer with it
   (`build_installer.bat setup`, then build) and have it ready.
2. Replace `cert.pem`/`key.pem` in `TASKFLOW_TLS_DIR` (owner `root:10001`, key `0640`), then
   `docker compose restart taskflow-server`, and check the fingerprint in `docker compose logs`.
3. Distribute the new installer at the same time. Until users update, their apps show "the certificate does not
   match" - which is exactly what pinning must do; tell users to confirm the new fingerprint only if IT gave it to them.
4. Keep the old certificate files; if something goes wrong, putting them back restores the old fingerprint.

## 4. First deployment (Linux host with Docker Engine + Compose plugin)

Example host folders: `/srv/taskflow/{config,tls,data}`; server name `task.inverted.in`.

```bash
unzip InvertedTaskFlow_Server_v1.0.1.zip && cd InvertedTaskFlow_Server_v1.0.1

# 1. host folders (uid/gid 10001 = the container user)
sudo install -d -o root  -g 10001 -m 0750 /srv/taskflow/config /srv/taskflow/tls
sudo install -d -o 10001 -g 10001 -m 0750 /srv/taskflow/data
sudo install -o root -g 10001 -m 0640 deploy/docker/server.ini /srv/taskflow/config/server.ini
sudo nano /srv/taskflow/config/server.ini        # admin_email, company name, e-mail settings

# 2. HTTPS certificate - the same command install_server.sh uses (EC P-256, 10 years).
#    Skip this if you already have the certificate the installed apps pin (e.g. /etc/taskflow/*.pem): copy those instead.
#    Put every name/IP the PCs will use into subjectAltName (e.g. add ,IP:15.207.128.179).
HOST=task.inverted.in
[ -e /srv/taskflow/tls/key.pem ] && echo "a key already exists - not overwriting it" || \
sudo openssl req -x509 -newkey ec -pkeyopt ec_paramgen_curve:prime256v1 -nodes -days 3650 \
  -keyout /srv/taskflow/tls/key.pem -out /srv/taskflow/tls/cert.pem \
  -subj "/O=Inverted Energy/CN=$HOST" -addext "subjectAltName=DNS:$HOST,DNS:localhost,IP:127.0.0.1"
sudo chown root:10001 /srv/taskflow/tls/key.pem /srv/taskflow/tls/cert.pem
sudo chmod 0640 /srv/taskflow/tls/key.pem && sudo chmod 0644 /srv/taskflow/tls/cert.pem
#    (own certificate from the company CA: copy it to cert.pem / key.pem with the same owner and modes)

# 3. Docker settings (TASKFLOW_HOST=task.inverted.in and TASKFLOW_PORT=8443 are already the defaults)
cp .env.example .env
sed -i 's|^TASKFLOW_CONFIG_DIR=.*|TASKFLOW_CONFIG_DIR=/srv/taskflow/config|;
        s|^TASKFLOW_TLS_DIR=.*|TASKFLOW_TLS_DIR=/srv/taskflow/tls|;
        s|^TASKFLOW_DATA_DIR=.*|TASKFLOW_DATA_DIR=/srv/taskflow/data|' .env
docker compose config >/dev/null && echo "compose file OK"

# 4. build and start
docker compose build
docker compose up -d
docker compose ps                                  # STATUS must become "healthy"
docker compose logs --tail 50

# 5. values for the desktop installer (build_installer.bat setup) - also printed by the container at start-up
docker compose logs taskflow-server | grep -E "desktop app server address|certificate fingerprint" | tail -2
sudo openssl x509 -in /srv/taskflow/tls/cert.pem -outform DER | sha256sum | awk '{print toupper($1)}' | sed 's/../&:/g; s/:$//'

#    verify DNS, port and certificate from the server itself and from an office PC
dig +short task.inverted.in
curl -fsS --cacert /srv/taskflow/tls/cert.pem https://task.inverted.in:8443/api/v1/health    # self-signed cert: name + chain checked
openssl s_client -connect task.inverted.in:8443 -servername task.inverted.in </dev/null 2>/dev/null \
  | openssl x509 -outform DER | sha256sum                                                      # must equal the fingerprint

# 6. first administrator password: sign in with the desktop app, choose a new password, then delete the file
sudo cat /srv/taskflow/data/initial_admin_password.txt
sudo rm /srv/taskflow/data/initial_admin_password.txt
```

### Address, port and the desktop app

The desktop installer has the **server address** and the **certificate fingerprint** built in. Both must stay the
same for the life of the installation:

- The address is `https://task.inverted.in:8443` = `https://TASKFLOW_HOST:TASKFLOW_PORT`. If you change
  `TASKFLOW_PORT`, the address changes and a new installer is needed. The container side is always 8443.
- The fingerprint is the SHA-256 of `cert.pem` (section 3). **Back up `TASKFLOW_TLS_DIR` together with the data**,
  so a rebuilt server keeps the same certificate.

### Firewall

`task.inverted.in` resolves to a public address, so restrict access before the first start:

1. **Cloud firewall / security group** (first line of defence): allow TCP 8443 only from the office public IP(s) and the
   VPN range, and SSH (22) only from the admin addresses. Nothing else inbound.
2. Docker publishes ports through its own iptables rules, which **bypass `ufw`** for that port. To filter on the host as
   well, use the `DOCKER-USER` chain, e.g. allow only `203.0.113.0/24` (replace with your office/VPN range; test it, and
   make it persistent with your distribution's iptables tooling):
   ```bash
   sudo iptables -I DOCKER-USER -p tcp -m conntrack --ctorigdstport 8443 --ctdir ORIGINAL ! -s 203.0.113.0/24 -j DROP
   ```
3. `TASKFLOW_BIND_IP` can limit the published port to one interface of the host (on a cloud VM usually the private
   interface address that the public IP is mapped to; `0.0.0.0` = all).
4. Do not open 8443 to the whole internet unless that is a deliberate decision - the server has rate limits and lockouts,
   but every sign-in page on the internet attracts password guessing.

## 5. Day-to-day operations

`taskflowctl` (Linux systemd install) does not apply here; these are the equivalents:

| Task | Command |
|---|---|
| status / health | `docker compose ps` · `docker inspect -f '{{.State.Health.Status}}' taskflow-server` |
| logs | `docker compose logs --tail 200` · `docker compose logs -f` |
| restart (e.g. after editing `server.ini`) | `docker compose restart taskflow-server` |
| stop / start | `docker compose stop` (graceful, SIGTERM, up to 30 s) · `docker compose up -d` |
| backup now | `docker compose exec taskflow-server entrypoint.sh --backup-now` |
| list backups | `ls -lt /srv/taskflow/data/backups` |
| integrity check | `docker compose exec taskflow-server entrypoint.sh --check` |
| reset a password | `docker compose exec taskflow-server entrypoint.sh --reset-password name@inverted.in` |
| test e-mail | `docker compose exec taskflow-server entrypoint.sh --test-email you@inverted.in` |
| certificate fingerprint | `openssl x509 -in /srv/taskflow/tls/cert.pem -outform DER \| sha256sum` |

The daily backup (02:00 in `TASKFLOW_TZ`, 30 kept) runs inside the server as before; if the server was off at that time it
runs on the next start that day. **Copy the backups to another machine**, e.g. `/etc/cron.d/taskflow-offsite`:
`0 3 * * * root rsync -a /srv/taskflow/data/backups/ /mnt/nas/taskflow-backups/` - and keep a copy of `/srv/taskflow/tls`
and `/srv/taskflow/config` with it.

## 6. Upgrading

```bash
docker compose exec taskflow-server entrypoint.sh --backup-now       # 1. backup with the running version
unzip InvertedTaskFlow_Server_vX.Y.Z.zip && cd InvertedTaskFlow_Server_vX.Y.Z
cp ../InvertedTaskFlow_Server_v1.0.1/.env .                          # 2. same settings and host folders
sed -i 's/^TASKFLOW_TAG=.*/TASKFLOW_TAG=X.Y.Z/' .env                 # 3. new tag - the old image stays for a rollback
docker compose build                                                 # 4. a failed build changes nothing
docker compose up -d                                                 # 5. replaces the container; data stays on the host
docker compose ps && docker compose logs --tail 50
```

Rollback: set `TASKFLOW_TAG` back to the old version and `docker compose up -d`. If the new version upgraded the database
format, also restore the backup from step 1 (section 7). Update the server first, then publish the desktop update.

## 7. Restoring a backup

```bash
cd InvertedTaskFlow_Server_v1.0.1
docker compose stop taskflow-server                                   # the server must not run while files are swapped
D=/srv/taskflow/data; B=$D/backups/taskflow-20261028-020000.db        # the backup to restore
K=$D/taskflow.db.before-restore-$(date +%Y%m%d-%H%M%S)
sudo mv $D/taskflow.db $K                                             # keep the current database ...
for x in wal shm; do [ -f $D/taskflow.db-$x ] && sudo mv $D/taskflow.db-$x $K-$x; done   # ... with its WAL files
sudo install -o 10001 -g 10001 -m 0640 $B $D/taskflow.db
docker compose run --rm --no-deps taskflow-server --check             # must print "Integrity  : ok"
docker compose up -d && docker compose ps
```

If the check fails, put the kept files back (`mv $K $D/taskflow.db`, and its `-wal`/`-shm`) and choose another backup.

## 8. Moving an existing systemd installation into Docker

1. `sudo taskflowctl backup`, then `sudo systemctl stop taskflow && sudo systemctl disable taskflow` - **only one server
   may ever write the database**.
2. Copy the data with the service stopped: `sudo cp -p /var/lib/taskflow/taskflow.db* /srv/taskflow/data/` and
   `sudo cp -rp /var/lib/taskflow/backups /srv/taskflow/data/`, then `sudo chown -R 10001:10001 /srv/taskflow/data`.
3. Certificate - check which name the installed apps use and what the old certificate covers:
   `openssl x509 -in /etc/taskflow/cert.pem -noout -ext subjectAltName`
   - **Apps already use `https://task.inverted.in:8443` and the certificate covers it:** copy `/etc/taskflow/cert.pem` and
     `key.pem` to `/srv/taskflow/tls/` (owner `root:10001`, key `0640`) - same fingerprint, no new installer.
   - **Apps use an older address (IP or other name):** moving to `task.inverted.in` changes the address built into the
     apps anyway, so a new installer is needed. Create a new certificate for `task.inverted.in` (section 4, step 2) and
     follow *If the certificate (fingerprint) changes* in section 3. (Keeping the old certificate with
     `TASKFLOW_HOST=<old name>` only works while the apps keep using the old address.)
4. Carry your settings over into `/srv/taskflow/config/server.ini` (keep the container paths of `deploy/docker/server.ini`).
5. Keep the same `TASKFLOW_PORT` as before (8443), then `docker compose up -d`.

## 9. Kubernetes (not implemented - what it would take)

- **Exactly one replica**: a `StatefulSet` (or `Deployment` with `replicas: 1` and `strategy: Recreate`) - never two pods on
  the same database, no horizontal autoscaling. Updates mean a short outage.
- **Storage**: a `ReadWriteOnce` PersistentVolumeClaim on block storage for `/var/lib/taskflow`. Not NFS/SMB or other
  network file systems: SQLite's WAL mode needs reliable POSIX locks and shared memory on a local file system.
- **TLS**: a `kubernetes.io/tls` Secret mounted read-only at `/etc/taskflow/tls` (key names `cert.pem`/`key.pem` via `items`).
  The pod must terminate TLS itself, so expose it with a `LoadBalancer`/`NodePort` Service (or an ingress in TCP/TLS
  *passthrough* mode) - an ingress that terminates TLS would present a different certificate and break pinning.
- **Settings**: `server.ini` from a Secret (it can contain `smtp_password`), mounted at `/etc/taskflow/conf`.
- **Security context**: `runAsUser/runAsGroup/fsGroup: 10001`, `runAsNonRoot`, `readOnlyRootFilesystem`, drop all
  capabilities, an `emptyDir` for `/tmp`, `TZ` as an env var.
- **Probes**: liveness/readiness `httpGet` on `/api/v1/health`, scheme HTTPS, port 8443; a startup probe of ~60 s.
  `terminationGracePeriodSeconds: 30`.
- **Long-lived connections**: every desktop app holds a long-poll request of up to 25 s - load balancer idle timeouts must be
  above 30 s. The server sees client IPs only if the Service keeps them (`externalTrafficPolicy: Local`); otherwise the
  per-computer limits apply to the load balancer address.
- **Backups**: the in-server daily backup writes to the same volume - add volume snapshots or a CronJob that copies the
  `backups/` folder off the cluster.

## 10. Limitations and things to know

- **Client IP addresses.** The server rate-limits per computer (30 requests/s, 20 failed sign-ins per 10 minutes,
  5 sign-ups per hour). With Docker Engine's default port publishing (iptables) it sees the real client address. If the
  userland proxy is used (Docker Desktop, rootless Docker, or `"userland-proxy"` traffic) every client appears as one
  gateway address and shares these limits. Check `docker compose logs` - the first column is the address the server sees.
- **No HTTP-only mode by default.** Running without TLS is refused unless `TASKFLOW_ALLOW_HTTP=1`.
- **One writer.** Never run the container and the systemd service (or two containers) on the same data folder. The fixed
  `container_name` stops `docker compose up --scale`.
- **Configuration errors** make the container restart in a loop (`restart: unless-stopped`); the reason is the last
  `entrypoint: ERROR` line in `docker compose logs`.
- Base image `debian:bookworm-slim` is referenced by tag, not by digest; rebuild regularly to get security updates.
- Health status `healthy` means the server answers HTTPS; database integrity is checked with `--check` and logged as
  `!!! DATABASE CHECK FAILED` at every start.
