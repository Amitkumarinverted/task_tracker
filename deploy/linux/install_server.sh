#!/bin/bash
# =============================================================================
#  Inverted TaskFlow Server - one-step installation on Ubuntu 22.04 / 24.04 LTS or Debian 12
#
#    sudo bash install_server.sh                    (asks a few questions)
#    sudo bash install_server.sh --host taskflow.inverted.local --admin it@inverted.in --yes
#
#  What it does (safe to run again - it never deletes data):
#    1. installs the build tools (gcc, cmake, OpenSSL headers)
#    2. builds the server from the source in this package
#    3. creates a locked-down service account "taskflow"
#    4. creates an HTTPS certificate (or uses yours: --cert FILE --key FILE)
#    5. writes /etc/taskflow/server.ini, installs a hardened systemd service and starts it
#    6. opens port 8443 in the firewall (ufw) and installs the "taskflowctl" admin tool
#  At the end it prints the server address, the certificate fingerprint and where the first
#  administrator password is.
# =============================================================================
set -euo pipefail

PORT=8443
HOST=""
ADMIN_EMAIL=""
CERT=""
KEY=""
ASSUME_YES=0
PORT_GIVEN=0
HERE="$(cd "$(dirname "$0")" && pwd)"
SRC="$(cd "$HERE/../.." && pwd)"   # package root (contains server/, common/, third_party/)

while [ $# -gt 0 ]; do
  case "$1" in
    --host) HOST="$2"; shift 2 ;;
    --port) PORT="$2"; PORT_GIVEN=1; shift 2 ;;
    --admin) ADMIN_EMAIL="$2"; shift 2 ;;
    --cert) CERT="$2"; shift 2 ;;
    --key) KEY="$2"; shift 2 ;;
    --yes|-y) ASSUME_YES=1; shift ;;
    -h|--help) sed -n '2,20p' "$0"; exit 0 ;;
    *) echo "Unknown option $1"; exit 2 ;;
  esac
done

say()  { printf '\n\033[1;34m==>\033[0m %s\n' "$*"; }
ok()   { printf '    \033[32mOK\033[0m %s\n' "$*"; }
warn() { printf '    \033[33m!!\033[0m %s\n' "$*"; }
die()  { printf '\n\033[31mERROR:\033[0m %s\n' "$*"; exit 1; }

[ "$(id -u)" -eq 0 ] || die "Run with sudo:  sudo bash install_server.sh"
[ -f "$SRC/server/CMakeLists.txt" ] || die "Server source not found next to this script ($SRC/server)"
command -v systemctl >/dev/null || die "systemd is required"
. /etc/os-release 2>/dev/null || true
case "${ID:-}" in ubuntu|debian) ;; *) warn "Tested on Ubuntu/Debian; continuing on ${PRETTY_NAME:-unknown}" ;; esac

DEFAULT_IP="$(hostname -I 2>/dev/null | awk '{print $1}' || true)"
DEFAULT_HOST="$DEFAULT_IP"
# running again: the existing certificate knows the name the PCs already use
if [ -f /etc/taskflow/cert.pem ]; then
  CN="$(openssl x509 -in /etc/taskflow/cert.pem -noout -subject -nameopt multiline 2>/dev/null | awk -F' = ' '/commonName/ {print $2}' || true)"
  [ -n "$CN" ] && DEFAULT_HOST="$CN"
fi
if [ -z "$HOST" ]; then
  if [ "$ASSUME_YES" -eq 1 ]; then HOST="$DEFAULT_HOST"
  else
    read -rp "Server name or IP address the PCs will use [$DEFAULT_HOST]: " HOST
    HOST="${HOST:-$DEFAULT_HOST}"
  fi
fi
if [ -z "$ADMIN_EMAIL" ]; then
  if [ "$ASSUME_YES" -eq 1 ]; then ADMIN_EMAIL="admin@taskflow.local"
  else
    read -rp "E-mail for the first administrator account [admin@taskflow.local]: " ADMIN_EMAIL
    ADMIN_EMAIL="${ADMIN_EMAIL:-admin@taskflow.local}"
  fi
fi
# running again: keep the port of the existing settings
if [ "$PORT_GIVEN" -eq 0 ] && [ -f /etc/taskflow/server.ini ]; then
  P="$(grep -E '^\s*port\s*=' /etc/taskflow/server.ini | tail -1 | sed -E 's/^[^=]*=\s*//; s/\s+$//' || true)"
  [ -n "$P" ] && PORT="$P"
fi
[[ "$PORT" =~ ^[0-9]+$ ]] || die "Invalid port"

VERSION="$(grep -oE 'TF_VERSION "[0-9.]+"' "$SRC/common/version.h" | grep -oE '[0-9]+\.[0-9]+\.[0-9]+')"
say "Installing Inverted TaskFlow Server $VERSION for https://$HOST:$PORT"

# ---------------------------------------------------------------- 1. packages
say "Installing build tools"
export DEBIAN_FRONTEND=noninteractive
apt-get update -qq
apt-get install -y -qq build-essential cmake libssl-dev openssl sqlite3 curl unzip ca-certificates >/dev/null
ok "build tools ready"

# ---------------------------------------------------------------- 2. build
say "Building the server (takes 1-3 minutes)"
BUILD="$(mktemp -d)"
trap 'rm -rf "$BUILD"' EXIT
# parallel jobs: one per CPU, but at most one per GB of memory (each compile needs up to ~0.7 GB)
MEM_GB="$(awk '/MemTotal/ {print int($2 / 1048576)}' /proc/meminfo)"
JOBS="$(nproc)"; [ "$MEM_GB" -lt "$JOBS" ] && JOBS="$MEM_GB"; [ "$JOBS" -lt 1 ] && JOBS=1
if ! { cmake -S "$SRC/server" -B "$BUILD" -DCMAKE_BUILD_TYPE=Release && cmake --build "$BUILD" -j"$JOBS"; } >"$BUILD/build.log" 2>&1; then
  tail -n 30 "$BUILD/build.log"
  die "building the server failed (last lines of the build log above)"
fi
"$BUILD/TaskFlowServer" --version >/dev/null || die "build failed"
ok "built version $("$BUILD/TaskFlowServer" --version)"

# ---------------------------------------------------------------- 3. account and folders
say "Creating the service account and folders"
id taskflow >/dev/null 2>&1 || useradd --system --home-dir /var/lib/taskflow --shell /usr/sbin/nologin taskflow
install -d -o root -g taskflow -m 0750 /etc/taskflow
install -d -o taskflow -g taskflow -m 0750 /var/lib/taskflow /var/lib/taskflow/backups /var/log/taskflow
install -d -o root -g root -m 0755 /opt/taskflow
if [ -f /opt/taskflow/TaskFlowServer ]; then
  # running the installer again: back up the database with the current version before replacing it
  if [ -f /var/lib/taskflow/taskflow.db ] && [ -f /etc/taskflow/server.ini ]; then
    if runuser -u taskflow -- /opt/taskflow/TaskFlowServer --config /etc/taskflow/server.ini --backup-now >/dev/null 2>&1; then
      ok "database backed up before the update"
    else
      warn "could not back up the database first - continuing"
    fi
  fi
  cp -p /opt/taskflow/TaskFlowServer /opt/taskflow/TaskFlowServer.previous
fi
# copy next to it and rename: works even while the old version is running
install -o root -g root -m 0755 "$BUILD/TaskFlowServer" /opt/taskflow/TaskFlowServer.new
mv -f /opt/taskflow/TaskFlowServer.new /opt/taskflow/TaskFlowServer
ok "/opt/taskflow (program), /etc/taskflow (settings), /var/lib/taskflow (data + backups), /var/log/taskflow"

# ---------------------------------------------------------------- 4. certificate
say "HTTPS certificate"
if [ -n "$CERT" ] && [ -n "$KEY" ]; then
  install -o root -g taskflow -m 0640 "$CERT" /etc/taskflow/cert.pem
  install -o root -g taskflow -m 0640 "$KEY" /etc/taskflow/key.pem
  ok "using your certificate"
elif [ -f /etc/taskflow/cert.pem ] && [ -f /etc/taskflow/key.pem ]; then
  ok "keeping the existing certificate"
else
  SAN="DNS:localhost,IP:127.0.0.1"
  if [[ "$HOST" =~ ^[0-9.]+$ ]]; then SAN="IP:$HOST,$SAN"; else SAN="DNS:$HOST,$SAN"; fi
  [ -n "$DEFAULT_IP" ] && [ "$DEFAULT_IP" != "$HOST" ] && SAN="$SAN,IP:$DEFAULT_IP"
  openssl req -x509 -newkey ec -pkeyopt ec_paramgen_curve:prime256v1 -nodes -days 3650 \
    -keyout /etc/taskflow/key.pem -out /etc/taskflow/cert.pem \
    -subj "/O=Inverted Energy/CN=$HOST" -addext "subjectAltName=$SAN" >/dev/null 2>&1
  chown root:taskflow /etc/taskflow/key.pem /etc/taskflow/cert.pem
  chmod 0640 /etc/taskflow/key.pem /etc/taskflow/cert.pem
  ok "created a company certificate valid for 10 years ($SAN)"
fi
FP="$(openssl x509 -in /etc/taskflow/cert.pem -outform DER | sha256sum | awk '{print toupper($1)}' | sed 's/../&:/g; s/:$//')"

# ---------------------------------------------------------------- 5. settings
say "Settings"
if [ ! -f /etc/taskflow/server.ini ]; then
  sed -e "s|^port = .*|port = $PORT|" -e "s|^admin_email = .*|admin_email = $ADMIN_EMAIL|" \
      "$SRC/server/config/server.ini.linux" > /etc/taskflow/server.ini
  chown root:taskflow /etc/taskflow/server.ini
  chmod 0640 /etc/taskflow/server.ini
  ok "wrote /etc/taskflow/server.ini"
else
  ok "keeping your /etc/taskflow/server.ini"
fi

install -o root -g root -m 0644 "$HERE/taskflow.service" /etc/systemd/system/taskflow.service
install -o root -g root -m 0755 "$HERE/taskflowctl" /usr/local/bin/taskflowctl
mkdir -p /usr/local/share/taskflow
cp -r "$SRC/server" "$SRC/common" /usr/local/share/taskflow/ 2>/dev/null || true
cp "$SRC"/IT_Admin_Guide*.md /usr/local/share/taskflow/IT_Admin_Guide.md 2>/dev/null || true
mkdir -p /usr/local/share/taskflow/third_party
cp -r "$SRC/third_party/sqlite" "$SRC/third_party/httplib.h" "$SRC/third_party/json.hpp" /usr/local/share/taskflow/third_party/ 2>/dev/null || true
systemctl daemon-reload
systemctl enable taskflow >/dev/null 2>&1
systemctl restart taskflow

# ---------------------------------------------------------------- 6. firewall
if command -v ufw >/dev/null 2>&1; then
  ufw allow "$PORT"/tcp comment 'Inverted TaskFlow' >/dev/null 2>&1 || true
  if ufw status | grep -q "Status: active"; then ok "firewall: port $PORT open"; else warn "ufw is installed but not active - see the guide (section Security)"; fi
else
  warn "no ufw firewall - make sure port $PORT/tcp is reachable from the office network only"
fi

say "Waiting for the server to start"
for i in $(seq 1 30); do
  if curl -fsk "https://127.0.0.1:$PORT/api/v1/health" >/dev/null 2>&1; then break; fi
  sleep 1
done
curl -fsk "https://127.0.0.1:$PORT/api/v1/health" >/dev/null 2>&1 || { journalctl -u taskflow -n 30 --no-pager; die "the service did not start - see the log above"; }
ok "server is running"

cat <<EOF

=============================================================================
 Inverted TaskFlow Server $VERSION is installed and running.

   Server address (give this to the desktop app build):
       https://$HOST:$PORT

   Certificate fingerprint (SHA-256) - the app uses it to recognise the server:
       $FP

   First administrator account: $ADMIN_EMAIL
EOF
if [ -f /var/lib/taskflow/initial_admin_password.txt ]; then
  echo "   Its password is in:  /var/lib/taskflow/initial_admin_password.txt"
  echo "   (sudo cat it, sign in, choose a new password, then delete the file)"
fi
cat <<EOF

   Daily backups go to /var/lib/taskflow/backups (02:00, 30 kept).
   Admin tool: taskflowctl status | logs | backup | restore | check | fingerprint ...
=============================================================================
EOF
