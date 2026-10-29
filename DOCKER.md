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
| `TASKFLOW_PORT` | `8443` | port on the **host**; the container side is always 8443 |
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

## 3. First deployment (Linux host with Docker Engine + Compose plugin)

Example host folders: `/srv/taskflow/{config,tls,data}`; server name `taskflow.inverted.local`.

```bash
unzip InvertedTaskFlow_Server_v1.0.1.zip && cd InvertedTaskFlow_Server_v1.0.1

# 1. host folders (uid/gid 10001 = the container user)
sudo install -d -o root  -g 10001 -m 0750 /srv/taskflow/config /srv/taskflow/tls
sudo install -d -o 10001 -g 10001 -m 0750 /srv/taskflow/data
sudo install -o root -g 10001 -m 0640 deploy/docker/server.ini /srv/taskflow/config/server.ini
sudo nano /srv/taskflow/config/server.ini        # admin_email, company name, e-mail settings

# 2. HTTPS certificate - the same command install_server.sh uses (EC P-256, 10 years).
#    Put every name/IP the PCs will use into subjectAltName (use IP:10.0.0.5 for an address).
HOST=taskflow.inverted.local
sudo openssl req -x509 -newkey ec -pkeyopt ec_paramgen_curve:prime256v1 -nodes -days 3650 \
  -keyout /srv/taskflow/tls/key.pem -out /srv/taskflow/tls/cert.pem \
  -subj "/O=Inverted Energy/CN=$HOST" -addext "subjectAltName=DNS:$HOST,DNS:localhost,IP:127.0.0.1"
sudo chown root:10001 /srv/taskflow/tls/key.pem /srv/taskflow/tls/cert.pem
sudo chmod 0640 /srv/taskflow/tls/key.pem && sudo chmod 0644 /srv/taskflow/tls/cert.pem
#    (own certificate from the company CA: copy it to cert.pem / key.pem with the same owner and modes)

# 3. Docker settings
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

# 5. values for the desktop installer (build_installer.bat)
echo "Server address: https://$HOST:8443"
sudo openssl x509 -in /srv/taskflow/tls/cert.pem -outform DER | sha256sum | awk '{print toupper($1)}' | sed 's/../&:/g; s/:$//'

# 6. first administrator password: sign in with the desktop app, choose a new password, then delete the file
sudo cat /srv/taskflow/data/initial_admin_password.txt
sudo rm /srv/taskflow/data/initial_admin_password.txt
```

### Hostname, certificate and the desktop app

The desktop installer has the **server address** and the **certificate fingerprint** built in (IT Admin Guide, chapter 5).
Both must stay exactly the same for the life of the installation:

- The address is `https://<name or IP>:<TASKFLOW_PORT>` - the name the PCs resolve, not the container's name.
  If you change `TASKFLOW_PORT`, the address changes and a new installer is needed.
- The fingerprint is the SHA-256 of `cert.pem`. A new certificate means a new fingerprint: rebuild and publish the
  installer (or users confirm the new fingerprint once at sign-in). **Back up `TASKFLOW_TLS_DIR` together with the data**,
  so a rebuilt server keeps the same certificate.
- Never disable pinning or switch to plain HTTP to "make it work"; check the fingerprint instead.

### Firewall

Docker publishes ports through its own iptables rules, which **bypass `ufw`** rules for that port. Therefore:

1. Set `TASKFLOW_BIND_IP` to the host's office-network address (not `0.0.0.0`) if the host has other interfaces.
2. Restrict who may connect in the `DOCKER-USER` chain (or on the network firewall), for example only the office/VPN range
   `10.0.0.0/8` (adapt and test; make it persistent with your distribution's iptables tooling):
   ```bash
   sudo iptables -I DOCKER-USER -p tcp -m conntrack --ctorigdstport 8443 --ctdir ORIGINAL ! -s 10.0.0.0/8 -j DROP
   ```
3. Do not expose the port to the internet. Only SSH and the TaskFlow port need to be reachable.

## 4. Day-to-day operations

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

## 5. Upgrading

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
format, also restore the backup from step 1 (section 6). Update the server first, then publish the desktop update.

## 6. Restoring a backup

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

## 7. Moving an existing systemd installation into Docker

1. `sudo taskflowctl backup`, then `sudo systemctl stop taskflow && sudo systemctl disable taskflow` - **only one server
   may ever write the database**.
2. Copy the data with the service stopped: `sudo cp -p /var/lib/taskflow/taskflow.db* /srv/taskflow/data/` and
   `sudo cp -rp /var/lib/taskflow/backups /srv/taskflow/data/`, then `sudo chown -R 10001:10001 /srv/taskflow/data`.
3. Copy `/etc/taskflow/cert.pem` and `key.pem` to `/srv/taskflow/tls/` (owner `root:10001`, key `0640`) - the fingerprint
   stays the same, so no new desktop installer is needed.
4. Carry your settings over into `/srv/taskflow/config/server.ini` (keep the container paths of `deploy/docker/server.ini`).
5. Keep the same `TASKFLOW_PORT` and host name as before, then `docker compose up -d`.

## 8. Kubernetes (not implemented - what it would take)

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

## 9. Limitations and things to know

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
