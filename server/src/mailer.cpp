// Inverted TaskFlow Server - e-mail notifications (SMTP client + outbox worker)
//
// Notifications are first written to the email_outbox table inside the same transaction as the change itself, so a
// mail is never lost when the mail server is down: this worker retries with back-off (1, 5, 15, 60, 240 minutes).
// Security modes: "starttls" (port 587), "ssl" (port 465) and "none" (internal relay on port 25).
// TLS needs the OpenSSL build (Linux); the Windows build supports "none" only.
#include <atomic>
#include <chrono>
#include <cstring>
#include <thread>

#include "crypto.h"
#include "server.h"

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
typedef SOCKET sock_t;
#define TF_BAD_SOCK INVALID_SOCKET
#define tf_close_sock ::closesocket
#else
#include <netdb.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>
typedef int sock_t;
#define TF_BAD_SOCK (-1)
#define tf_close_sock ::close
#endif

#ifdef CPPHTTPLIB_OPENSSL_SUPPORT
#include <openssl/err.h>
#include <openssl/ssl.h>
#include <openssl/x509v3.h>
#endif

namespace tf {
namespace {

std::string b64(const std::string& in) {
  static const char* t = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  std::string o;
  size_t i = 0;
  for (; i + 2 < in.size(); i += 3) {
    uint32_t v = ((uint8_t)in[i] << 16) | ((uint8_t)in[i + 1] << 8) | (uint8_t)in[i + 2];
    o += t[v >> 18]; o += t[(v >> 12) & 63]; o += t[(v >> 6) & 63]; o += t[v & 63];
  }
  if (i + 1 == in.size()) {
    uint32_t v = (uint8_t)in[i] << 16;
    o += t[v >> 18]; o += t[(v >> 12) & 63]; o += "==";
  } else if (i + 2 == in.size()) {
    uint32_t v = ((uint8_t)in[i] << 16) | ((uint8_t)in[i + 1] << 8);
    o += t[v >> 18]; o += t[(v >> 12) & 63]; o += t[(v >> 6) & 63]; o += '=';
  }
  return o;
}

// header values must never contain line breaks (header injection)
std::string header_safe(std::string s) {
  for (auto& c : s)
    if (c == '\r' || c == '\n' || c == '\0') c = ' ';
  return s;
}
std::string encode_header(const std::string& s) {
  bool ascii = true;
  for (unsigned char c : s)
    if (c >= 128) ascii = false;
  return ascii ? header_safe(s) : "=?UTF-8?B?" + b64(header_safe(s)) + "?=";
}
bool plain_address(const std::string& a) {
  if (a.empty() || a.size() > 200 || a.find('@') == std::string::npos) return false;
  for (unsigned char c : a)
    if (c <= 32 || c == '<' || c == '>' || c == '"' || c == ',' || c == ';' || c == '\\' || c >= 127) return false;
  return true;
}
std::string rfc2822_date() {
  std::time_t t = std::time(nullptr);
  std::tm g{};
#ifdef _WIN32
  gmtime_s(&g, &t);
#else
  gmtime_r(&t, &g);
#endif
  char buf[64];
  strftime(buf, sizeof buf, "%a, %d %b %Y %H:%M:%S +0000", &g);
  return buf;
}

class Conn {
 public:
  ~Conn() { close(); }
  std::string err;

  bool open(const std::string& host, int port, int timeout_s) {
    addrinfo hints{}, *res = nullptr;
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    if (getaddrinfo(host.c_str(), std::to_string(port).c_str(), &hints, &res) != 0 || !res) {
      err = "cannot resolve " + host;
      return false;
    }
    for (addrinfo* a = res; a; a = a->ai_next) {
      s_ = socket(a->ai_family, a->ai_socktype, a->ai_protocol);
      if (s_ == TF_BAD_SOCK) continue;
#ifdef _WIN32
      DWORD tv = timeout_s * 1000;
      setsockopt(s_, SOL_SOCKET, SO_RCVTIMEO, (const char*)&tv, sizeof tv);
      setsockopt(s_, SOL_SOCKET, SO_SNDTIMEO, (const char*)&tv, sizeof tv);
#else
      timeval tv{timeout_s, 0};
      setsockopt(s_, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
      setsockopt(s_, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
#endif
      if (connect(s_, a->ai_addr, (int)a->ai_addrlen) == 0) break;
      tf_close_sock(s_);
      s_ = TF_BAD_SOCK;
    }
    freeaddrinfo(res);
    if (s_ == TF_BAD_SOCK) err = "cannot connect to " + host + ":" + std::to_string(port);
    return s_ != TF_BAD_SOCK;
  }

  bool start_tls(const std::string& host, bool verify) {
#ifdef CPPHTTPLIB_OPENSSL_SUPPORT
    ctx_ = SSL_CTX_new(TLS_client_method());
    if (!ctx_) return fail_tls("TLS init failed");
    SSL_CTX_set_min_proto_version(ctx_, TLS1_2_VERSION);
    if (verify) {
      SSL_CTX_set_default_verify_paths(ctx_);
      SSL_CTX_set_verify(ctx_, SSL_VERIFY_PEER, nullptr);
    }
    ssl_ = SSL_new(ctx_);
    SSL_set_fd(ssl_, (int)s_);
    SSL_set_tlsext_host_name(ssl_, host.c_str());
    if (verify) SSL_set1_host(ssl_, host.c_str());
    if (SSL_connect(ssl_) != 1) return fail_tls("TLS handshake with the mail server failed");
    return true;
#else
    (void)host; (void)verify;
    err = "this server build has no TLS support - use smtp_security = none (internal relay) or the Linux build";
    return false;
#endif
  }

  bool send_all(const std::string& data) {
    size_t off = 0;
    while (off < data.size()) {
      int n;
#ifdef CPPHTTPLIB_OPENSSL_SUPPORT
      if (ssl_) n = SSL_write(ssl_, data.data() + off, (int)(data.size() - off));
      else
#endif
        n = (int)::send(s_, data.data() + off, (int)(data.size() - off), 0);
      if (n <= 0) { err = "connection to the mail server was lost"; return false; }
      off += (size_t)n;
    }
    return true;
  }

  // reads one complete (possibly multi-line) SMTP reply; returns the status code or 0
  int reply(std::string* text = nullptr) {
    std::string all;
    for (;;) {
      std::string line;
      if (!read_line(line)) { err = "no reply from the mail server"; return 0; }
      all += line + "\n";
      if (line.size() < 4 || line[3] != '-') break;
      if (all.size() > 64 * 1024) break;
    }
    if (text) *text = all;
    last_ = all;
    return all.size() >= 3 ? std::atoi(all.substr(0, 3).c_str()) : 0;
  }

  bool cmd(const std::string& line, int expect1, int expect2 = -1, std::string* text = nullptr) {
    if (!line.empty() && !send_all(line + "\r\n")) return false;
    int code = reply(text);
    if (code == expect1 || code == expect2) return true;
    if (err.empty()) err = "mail server said: " + header_safe(last_.substr(0, 200));
    return false;
  }

  void close() {
#ifdef CPPHTTPLIB_OPENSSL_SUPPORT
    if (ssl_) { SSL_shutdown(ssl_); SSL_free(ssl_); ssl_ = nullptr; }
    if (ctx_) { SSL_CTX_free(ctx_); ctx_ = nullptr; }
#endif
    if (s_ != TF_BAD_SOCK) { tf_close_sock(s_); s_ = TF_BAD_SOCK; }
  }

 private:
  sock_t s_ = TF_BAD_SOCK;
  std::string buf_, last_;
#ifdef CPPHTTPLIB_OPENSSL_SUPPORT
  SSL_CTX* ctx_ = nullptr;
  SSL* ssl_ = nullptr;
  bool fail_tls(const std::string& what) {
    unsigned long e = ERR_get_error();
    char b[256] = {0};
    if (e) ERR_error_string_n(e, b, sizeof b);
    err = what + (e ? std::string(" (") + b + ")" : "");
    return false;
  }
#endif
  bool read_line(std::string& line) {
    for (;;) {
      auto p = buf_.find('\n');
      if (p != std::string::npos) {
        line = buf_.substr(0, p);
        if (!line.empty() && line.back() == '\r') line.pop_back();
        buf_.erase(0, p + 1);
        return true;
      }
      char tmp[2048];
      int n;
#ifdef CPPHTTPLIB_OPENSSL_SUPPORT
      if (ssl_) n = SSL_read(ssl_, tmp, sizeof tmp);
      else
#endif
        n = (int)::recv(s_, tmp, sizeof tmp, 0);
      if (n <= 0) return false;
      buf_.append(tmp, (size_t)n);
      if (buf_.size() > 256 * 1024) return false;
    }
  }
};

}  // namespace

// Sends one plain-text e-mail. Returns false with a readable reason in `err`.
bool smtp_send(const Config& c, const std::string& to, const std::string& subject, const std::string& body, std::string& err) {
  if (c.smtp_host.empty()) { err = "smtp_host is not set"; return false; }
  std::string from = c.smtp_from.empty() ? c.smtp_user : c.smtp_from;
  if (!plain_address(from)) { err = "smtp_from is not a valid e-mail address"; return false; }
  if (!plain_address(to)) { err = "invalid recipient address"; return false; }
  const std::string sec = c.smtp_security;
  Conn k;
  auto out = [&](bool ok) {
    if (!ok) err = k.err.empty() ? "mail error" : k.err;
    return ok;
  };
  if (!k.open(c.smtp_host, c.smtp_port, 20)) return out(false);
  if (sec == "ssl" && !k.start_tls(c.smtp_host, c.smtp_verify_cert)) return out(false);
  if (!k.cmd("", 220)) return out(false);
  std::string caps;
  if (!k.cmd("EHLO taskflow", 250, -1, &caps)) return out(false);
  if (sec == "starttls") {
    if (!k.cmd("STARTTLS", 220)) return out(false);
    if (!k.start_tls(c.smtp_host, c.smtp_verify_cert)) return out(false);
    if (!k.cmd("EHLO taskflow", 250, -1, &caps)) return out(false);
  }
  if (!c.smtp_user.empty()) {
    std::string upcaps = caps;
    for (auto& ch : upcaps) ch = (char)toupper((unsigned char)ch);
    if (upcaps.find("PLAIN") != std::string::npos) {
      if (!k.cmd("AUTH PLAIN " + b64(std::string("\0", 1) + c.smtp_user + std::string("\0", 1) + c.smtp_password), 235))
        return out(false);
    } else {
      if (!k.cmd("AUTH LOGIN", 334)) return out(false);
      if (!k.cmd(b64(c.smtp_user), 334)) return out(false);
      if (!k.cmd(b64(c.smtp_password), 235)) return out(false);
    }
  }
  if (!k.cmd("MAIL FROM:<" + from + ">", 250)) return out(false);
  if (!k.cmd("RCPT TO:<" + to + ">", 250, 251)) return out(false);
  if (!k.cmd("DATA", 354)) return out(false);

  std::string msg;
  msg += "From: " + encode_header(c.smtp_from_name) + " <" + from + ">\r\n";
  msg += "To: <" + to + ">\r\n";
  msg += "Subject: " + encode_header(subject) + "\r\n";
  msg += "Date: " + rfc2822_date() + "\r\n";
  msg += "Message-ID: <" + std::to_string(std::time(nullptr)) + "." + to_hex(secure_random(8)) + "@taskflow>\r\n";
  msg += "MIME-Version: 1.0\r\nContent-Type: text/plain; charset=UTF-8\r\nContent-Transfer-Encoding: 8bit\r\n";
  msg += "Auto-Submitted: auto-generated\r\n\r\n";
  // normalise line endings and dot-stuff
  std::string line;
  for (size_t i = 0; i <= body.size(); i++) {
    if (i == body.size() || body[i] == '\n') {
      if (!line.empty() && line.back() == '\r') line.pop_back();
      if (!line.empty() && line[0] == '.') line = "." + line;
      msg += line + "\r\n";
      line.clear();
    } else {
      line += body[i];
    }
  }
  msg += ".";
  if (!k.cmd(msg, 250)) return out(false);
  k.cmd("QUIT", 221);
  return true;
}

namespace {
std::atomic<bool> g_mail_run{false};
std::thread g_mail_thread;
const int kBackoffMinutes[] = {1, 5, 15, 60, 240, 240};
}  // namespace

void start_mailer(App& app) {
  if (!app.cfg.email_enabled) return;
#ifndef CPPHTTPLIB_OPENSSL_SUPPORT
  if (app.cfg.smtp_security != "none") {
    log_line("  ! E-mail disabled: this build has no TLS support for smtp_security = " + app.cfg.smtp_security +
             " (use 'none' with an internal relay, or the Linux server build)");
    return;
  }
#endif
  g_mail_run = true;
  g_mail_thread = std::thread([&app] {
    log_line("  E-mail notifications: on (" + app.cfg.smtp_host + ":" + std::to_string(app.cfg.smtp_port) + ", " +
             app.cfg.smtp_security + ")");
    int idle = 0;
    while (g_mail_run) {
      if (idle > 0) {
        std::this_thread::sleep_for(std::chrono::seconds(1));
        idle--;
        continue;
      }
      json batch;
      try {
        std::lock_guard<std::recursive_mutex> lock(app.mu);
        batch = app.db.query("SELECT * FROM email_outbox WHERE sent_at='' AND attempts<6 AND next_try<=? ORDER BY id LIMIT 20",
                             {now_iso()});
      } catch (const std::exception& e) {
        log_line(std::string("  ! mail queue: ") + e.what());
      }
      for (auto& m : batch) {
        if (!g_mail_run) break;
        std::string err;
        bool ok = smtp_send(app.cfg, jstr(m, "to_email"), jstr(m, "subject"), jstr(m, "body"), err);
        int attempts = (int)jint(m, "attempts") + 1;
        try {
          std::lock_guard<std::recursive_mutex> lock(app.mu);
          if (ok)
            app.db.exec("UPDATE email_outbox SET sent_at=?, attempts=?, last_error='' WHERE id=?", {now_iso(), attempts, m["id"]});
          else
            app.db.exec("UPDATE email_outbox SET attempts=?, next_try=?, last_error=? WHERE id=?",
                        {attempts, iso_after_seconds(60LL * kBackoffMinutes[std::min(attempts - 1, 5)]), err, m["id"]});
          // keep the outbox small: sent mail older than 30 days is removed
          app.db.exec("DELETE FROM email_outbox WHERE sent_at<>'' AND sent_at<?", {iso_after_seconds(-30LL * 86400)});
        } catch (const std::exception& e) {
          log_line(std::string("  ! mail queue: ") + e.what());
        }
        if (!ok) {
          log_line("  ! e-mail to " + jstr(m, "to_email") + " failed (attempt " + std::to_string(attempts) + "): " + err);
          if (attempts >= 6) log_line("  ! giving up on e-mail #" + std::to_string(jint(m, "id")) + " after 6 attempts");
        }
      }
      idle = batch.size() >= 20 ? 0 : 10;  // poll every 10 s; drain quickly when busy
    }
  });
}

void stop_mailer() {
  g_mail_run = false;
  if (g_mail_thread.joinable()) g_mail_thread.join();
}

}  // namespace tf
