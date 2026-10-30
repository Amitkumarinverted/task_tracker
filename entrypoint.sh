#!/bin/sh
# =============================================================================
#  Inverted TaskFlow Server - container entrypoint
#
#  Checks the mounted settings, data folder and TLS files, then replaces itself with the
#  server program (exec), so the server receives SIGTERM directly and shuts down cleanly.
#
#    (no arguments)                    run the server
#    --backup-now | --check            maintenance commands of the server program
#    --reset-password EMAIL            (run them with: docker compose exec taskflow-server entrypoint.sh ...)
#    --test-email ADDRESS
#    --version | --help
#
#  It never creates, replaces or migrates the database itself: the server program does that
#  (CREATE TABLE IF NOT EXISTS - an existing database is only opened, never emptied).
# =============================================================================
set -eu

BIN=/opt/taskflow/TaskFlowServer
CONFIG=/etc/taskflow/conf/server.ini
CONTAINER_PORT=8443

log() { printf 'entrypoint: %s\n' "$*" >&2; }
die() { printf 'entrypoint: ERROR: %s\n' "$*" >&2; exit 1; }

# value of a setting in server.ini - same rules as the server (last occurrence wins, optional quotes)
setting() {
  sed -n -E "s/^[[:space:]]*$1[[:space:]]*=[[:space:]]*//p" "$CONFIG" | tail -n 1 |
    sed -E 's/[[:space:]]+$//; s/^"(.*)"$/\1/'
}
truthy() { case "$1" in 1|true|yes|on) return 0 ;; *) return 1 ;; esac; }
absolute() { case "$1" in /*) return 0 ;; *) return 1 ;; esac; }

[ -x "$BIN" ] || die "server program not found at $BIN"

case "${1:-}" in
  --version|-v|--help|-h) exec "$BIN" "$1" ;;
esac

# ---------------------------------------------------------------- arguments
for a in "$@"; do
  case "$a" in
    --reset|--demo)
      die "$a is a testing option (it deletes the database / loads demo accounts) and is refused in the container" ;;
    --config|--db|--port)
      die "$a cannot be overridden here - change $CONFIG instead" ;;
  esac
done

# ---------------------------------------------------------------- user
[ "$(id -u)" -ne 0 ] || die "refusing to run as root - start the container with the taskflow user (see docker-compose.yml)"

# ---------------------------------------------------------------- settings file
[ -f "$CONFIG" ] || die "settings file $CONFIG not found - copy deploy/docker/server.ini into the folder set by TASKFLOW_CONFIG_DIR"
[ -r "$CONFIG" ] || die "settings file $CONFIG is not readable by uid $(id -u) - check its permissions on the host"

DEMO="$(setting demo_data)"
if [ -n "$DEMO" ] && truthy "$DEMO"; then
  die "demo_data is switched on in server.ini - never on a real server; remove that line"
fi

BIND="$(setting bind)"
case "$BIND" in
  127.0.0.1|localhost|::1) die "bind = $BIND is unreachable through Docker port publishing - use bind = 0.0.0.0 (restrict access with TASKFLOW_BIND_IP and the firewall instead)" ;;
esac

PORT="$(setting port)"
[ "$PORT" = "$CONTAINER_PORT" ] ||
  die "server.ini must say port = $CONTAINER_PORT (the port inside the container; found '${PORT:-<not set, server default 8080>}'). Change TASKFLOW_PORT for the port on the host."

[ -z "$(setting admin_password)" ] ||
  log "warning: admin_password is set in server.ini (plain text). Leave it empty: a random first password is written to the data folder."

# ---------------------------------------------------------------- database and data folder
DB="$(setting database)"
[ -n "$DB" ] || die "server.ini has no 'database =' line (the default would land in the read-only settings folder)"
absolute "$DB" || die "database = $DB must be an absolute path (e.g. /var/lib/taskflow/taskflow.db)"
DATA_DIR="$(dirname "$DB")"
[ -d "$DATA_DIR" ] || die "data folder $DATA_DIR does not exist - is TASKFLOW_DATA_DIR mounted there?"
[ -w "$DATA_DIR" ] && [ -x "$DATA_DIR" ] ||
  die "data folder $DATA_DIR is not writable by uid $(id -u) - on the host run: sudo chown -R $(id -u):$(id -g) <TASKFLOW_DATA_DIR>"

BACKUP_DIR="$(setting backup_dir)"
[ -n "$BACKUP_DIR" ] || die "server.ini has no 'backup_dir =' line"
absolute "$BACKUP_DIR" || die "backup_dir = $BACKUP_DIR must be an absolute path"
[ -d "$BACKUP_DIR" ] || mkdir -p "$BACKUP_DIR" || die "cannot create backup folder $BACKUP_DIR"
[ -w "$BACKUP_DIR" ] || die "backup folder $BACKUP_DIR is not writable by uid $(id -u)"

if [ -e "$DB" ]; then
  [ -f "$DB" ] || die "$DB exists but is not a regular file"
  [ -r "$DB" ] && [ -w "$DB" ] || die "database $DB is not readable and writable by uid $(id -u)"
  for f in "$DB-wal" "$DB-shm"; do
    if [ -e "$f" ] && ! [ -w "$f" ]; then die "$f is not writable by uid $(id -u)"; fi
  done
  log "database: $DB (existing - opened, never replaced)"
else
  # no database, but backups exist: most likely a wrong mount or a deleted file - do not silently start empty
  if ls "$BACKUP_DIR"/taskflow-*.db >/dev/null 2>&1 && [ "${TASKFLOW_ALLOW_NEW_DB:-0}" != "1" ]; then
    die "no database at $DB, but $BACKUP_DIR contains backups. Restore one (see DOCKER.md) - or set TASKFLOW_ALLOW_NEW_DB=1 once to really start with an empty database."
  fi
  log "database: $DB does not exist yet - the server creates a new one (first start)"
fi

LOG_FILE="$(setting log_file)"
if [ -n "$LOG_FILE" ]; then
  absolute "$LOG_FILE" || die "log_file = $LOG_FILE must be an absolute path (or empty: log to the container output)"
  LOG_DIR="$(dirname "$LOG_FILE")"
  [ -d "$LOG_DIR" ] || mkdir -p "$LOG_DIR" || die "cannot create log folder $LOG_DIR"
  [ -w "$LOG_DIR" ] || die "log folder $LOG_DIR is not writable by uid $(id -u)"
fi

# ---------------------------------------------------------------- TLS
CERT="$(setting tls_cert)"
KEY="$(setting tls_key)"
if [ -n "$CERT" ] || [ -n "$KEY" ]; then
  [ -n "$CERT" ] && [ -n "$KEY" ] || die "set both tls_cert and tls_key in server.ini"
  [ -f "$CERT" ] || die "TLS certificate $CERT not found - is TASKFLOW_TLS_DIR mounted at /etc/taskflow/tls?"
  [ -f "$KEY" ] || die "TLS private key $KEY not found - is TASKFLOW_TLS_DIR mounted at /etc/taskflow/tls?"
  [ -r "$CERT" ] || die "TLS certificate $CERT is not readable by uid $(id -u)"
  [ -r "$KEY" ] || die "TLS private key $KEY is not readable by uid $(id -u) - on the host: sudo chown root:$(id -g) key.pem && sudo chmod 0640 key.pem"
  case "$(stat -c %a "$KEY" 2>/dev/null || echo 640)" in
    *[1-7]) log "warning: the TLS private key is readable by everyone on the host - chmod 0640 it" ;;
  esac
  openssl x509 -in "$CERT" -noout >/dev/null 2>&1 || die "TLS certificate $CERT is not a readable PEM certificate"
  # the name the desktop apps use (TASKFLOW_HOST) must be in the certificate
  if [ -n "${TASKFLOW_HOST:-}" ]; then
    case "$TASKFLOW_HOST" in
      *[!0-9.]*) check="-checkhost" ;;
      *) check="-checkip" ;;
    esac
    if ! openssl x509 -in "$CERT" -noout "$check" "$TASKFLOW_HOST" 2>/dev/null | grep -q "does match"; then
      die "the TLS certificate does not cover TASKFLOW_HOST=$TASKFLOW_HOST - create it with subjectAltName DNS:$TASKFLOW_HOST (see DOCKER.md), or correct TASKFLOW_HOST"
    fi
  else
    log "warning: TASKFLOW_HOST is not set - the certificate is not checked against the server name"
  fi
  openssl x509 -in "$CERT" -noout -checkend 2592000 >/dev/null 2>&1 ||
    log "warning: the TLS certificate expires within 30 days (or has expired): $(openssl x509 -in "$CERT" -noout -enddate 2>/dev/null)"
  FP="$(openssl x509 -in "$CERT" -outform DER 2>/dev/null | openssl dgst -sha256 -r | cut -c1-64 | tr 'a-f' 'A-F' | sed 's/../&:/g; s/:$//')"
  log "TLS: on (certificate $CERT)"
  [ -z "${TASKFLOW_HOST:-}" ] || log "desktop app server address : https://$TASKFLOW_HOST:${TASKFLOW_PUBLIC_PORT:-$CONTAINER_PORT}"
  log "certificate fingerprint     : $FP"
elif [ "${TASKFLOW_ALLOW_HTTP:-0}" = "1" ]; then
  log "warning: TLS is OFF - passwords and session tokens are sent unencrypted (TASKFLOW_ALLOW_HTTP=1)"
else
  die "tls_cert / tls_key are not set. The desktop app expects HTTPS with a pinned certificate. Set them (see DOCKER.md), or set TASKFLOW_ALLOW_HTTP=1 only behind a TLS reverse proxy."
fi

# ---------------------------------------------------------------- start
umask 027
[ -n "${TZ:-}" ] || log "warning: TZ is not set - due dates, reminders and the backup time use UTC"
exec "$BIN" --config "$CONFIG" "$@"
