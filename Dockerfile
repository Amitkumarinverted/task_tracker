# syntax=docker/dockerfile:1
# =============================================================================
#  Inverted TaskFlow Server - container image
#    docker compose build                      (or: docker build --target runtime -t inverted/taskflow-server:1.0.1 .)
#    docker build --target test .              builds and runs tests/api_test.py against the compiled server
# =============================================================================
ARG DEBIAN_RELEASE=bookworm

# ---------------------------------------------------------------- build
FROM debian:${DEBIAN_RELEASE}-slim AS build
# the same tools install_server.sh installs: compiler, CMake, OpenSSL headers (TLS + e-mail)
RUN apt-get update \
 && apt-get install -y --no-install-recommends build-essential cmake libssl-dev \
 && rm -rf /var/lib/apt/lists/*
WORKDIR /src
# CMakeLists.txt reads ../common/version.h and the bundled ../third_party (httplib, json, sqlite)
COPY common/ common/
COPY third_party/ third_party/
COPY server/ server/
# each compile needs up to ~0.7 GB of memory (see install_server.sh) - raise BUILD_JOBS on bigger build machines
ARG BUILD_JOBS=2
RUN cmake -S server -B build -DCMAKE_BUILD_TYPE=Release -DTF_TLS=ON \
 && cmake --build build -j"${BUILD_JOBS}" \
 && ./build/TaskFlowServer --version

# ---------------------------------------------------------------- test (optional, never part of the runtime image)
FROM build AS test
RUN apt-get update \
 && apt-get install -y --no-install-recommends python3 \
 && rm -rf /var/lib/apt/lists/*
COPY tests/ tests/
RUN python3 tests/api_test.py build/TaskFlowServer

# ---------------------------------------------------------------- runtime
FROM debian:${DEBIAN_RELEASE}-slim AS runtime
ARG TASKFLOW_UID=10001
ARG TASKFLOW_GID=10001
# libssl3: the server links OpenSSL dynamically; ca-certificates: SMTP certificate checks;
# tzdata: reminders and the daily backup use local time (TZ); curl: health check;
# openssl (command line): entrypoint.sh checks the certificate against TASKFLOW_HOST and prints its fingerprint
RUN apt-get update \
 && apt-get install -y --no-install-recommends libssl3 ca-certificates tzdata curl openssl \
 && rm -rf /var/lib/apt/lists/* \
 && groupadd --system --gid "${TASKFLOW_GID}" taskflow \
 && useradd --system --uid "${TASKFLOW_UID}" --gid taskflow --home-dir /var/lib/taskflow --no-create-home \
            --shell /usr/sbin/nologin taskflow \
 && install -d -o root -g taskflow -m 0750 /etc/taskflow /etc/taskflow/conf /etc/taskflow/tls \
 && install -d -o taskflow -g taskflow -m 0750 /var/lib/taskflow /var/lib/taskflow/backups

COPY --from=build /src/build/TaskFlowServer /opt/taskflow/TaskFlowServer
COPY entrypoint.sh /usr/local/bin/entrypoint.sh
COPY deploy/docker/server.ini /usr/local/share/taskflow/server.ini.docker
# strip Windows line endings in case the script was edited on a Windows PC
RUN sed -i 's/\r$//' /usr/local/bin/entrypoint.sh \
 && chmod 0755 /usr/local/bin/entrypoint.sh /opt/taskflow/TaskFlowServer \
 && chmod 0644 /usr/local/share/taskflow/server.ini.docker

ENV SQLITE_TMPDIR=/tmp
USER taskflow:taskflow
WORKDIR /var/lib/taskflow
VOLUME ["/var/lib/taskflow"]
EXPOSE 8443
STOPSIGNAL SIGTERM
# /api/v1/health never waits for the database lock (api_core.cpp). -k: loopback liveness check only;
# the desktop app still verifies the pinned certificate.
HEALTHCHECK --interval=30s --timeout=6s --start-period=20s --retries=3 \
  CMD curl -fsS -k --max-time 5 https://127.0.0.1:8443/api/v1/health >/dev/null \
      || curl -fsS --max-time 5 http://127.0.0.1:8443/api/v1/health >/dev/null || exit 1
ENTRYPOINT ["/usr/local/bin/entrypoint.sh"]
