// Inverted TaskFlow Server - logging (console + rotating log file, written by a background thread)
#include "server.h"

#include <condition_variable>
#include <deque>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <mutex>
#include <thread>

namespace fs = std::filesystem;

namespace tf {

// ---- logging: lines go to a queue and a background thread writes them to the console and the log file
// (10 MB x 5 files rotation). Request threads never wait for the console - a paused or full console window
// (e.g. text selected in a Windows console) cannot freeze the server.
// The logger state is created once and intentionally never destroyed: the writer thread is still waiting on
// the condition variable when the process exits, and destroying it then would block exit forever.
namespace {
std::mutex& g_log_mu = *new std::mutex;
std::condition_variable& g_log_cv = *new std::condition_variable;
std::deque<std::string>& g_log_q = *new std::deque<std::string>;
std::string& g_log_path = *new std::string;
size_t g_log_dropped = 0;
bool g_log_busy = false;
std::once_flag g_log_once;

void log_writer() {
  for (;;) {
    std::deque<std::string> batch;
    size_t dropped;
    std::string path;
    {
      std::unique_lock<std::mutex> l(g_log_mu);
      g_log_cv.wait(l, [] { return !g_log_q.empty(); });
      batch.swap(g_log_q);
      dropped = g_log_dropped;
      g_log_dropped = 0;
      path = g_log_path;
      g_log_busy = true;
    }
    if (dropped) batch.push_back(now_iso() + "    (" + std::to_string(dropped) + " log lines dropped - logging was too slow)");
    for (auto& line : batch) std::cout << line.substr(22) << "\n";
    std::cout.flush();
    if (!path.empty()) {
      std::error_code ec;
      if (fs::exists(path, ec) && fs::file_size(path, ec) > 10u * 1024 * 1024) {
        for (int i = 4; i >= 1; i--) {
          std::string from = path + "." + std::to_string(i), to = path + "." + std::to_string(i + 1);
          if (fs::exists(from, ec)) fs::rename(from, to, ec);
        }
        fs::rename(path, path + ".1", ec);
      }
      std::ofstream f(path, std::ios::app);
      if (f)
        for (auto& line : batch) f << line << "\n";
    }
    {
      std::lock_guard<std::mutex> l(g_log_mu);
      g_log_busy = false;
    }
    g_log_cv.notify_all();
  }
}
}  // namespace

void set_log_file(const std::string& p) {
  std::lock_guard<std::mutex> l(g_log_mu);
  g_log_path = p;
}
void log_line(const std::string& line) {
  std::call_once(g_log_once, [] { std::thread(log_writer).detach(); });
  {
    std::lock_guard<std::mutex> l(g_log_mu);
    if (g_log_q.size() < 20000) g_log_q.push_back(now_iso() + "  " + line);
    else g_log_dropped++;
  }
  g_log_cv.notify_all();
}
void log_flush() {
  std::unique_lock<std::mutex> l(g_log_mu);
  g_log_cv.wait_for(l, std::chrono::seconds(3), [] { return g_log_q.empty() && !g_log_busy; });
}

}  // namespace tf
