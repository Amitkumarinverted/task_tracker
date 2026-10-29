// Inverted TaskFlow Server - shared helpers: JSON fields, dates, versions
#include "server.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <ctime>

namespace tf {

// ============================================================== utilities
int64_t jint(const json& j, const char* k, int64_t def) {
  if (!j.is_object() || !j.contains(k) || j[k].is_null()) return def;
  if (j[k].is_number_integer()) return j[k].get<int64_t>();
  if (j[k].is_number()) return (int64_t)j[k].get<double>();
  if (j[k].is_string()) {
    try { return std::stoll(j[k].get<std::string>()); } catch (...) { return def; }
  }
  if (j[k].is_boolean()) return j[k].get<bool>() ? 1 : 0;
  return def;
}
std::string jstr(const json& j, const char* k, const std::string& def) {
  if (!j.is_object() || !j.contains(k) || j[k].is_null()) return def;
  const json& v = j[k];
  if (v.is_string()) return v.get<std::string>();
  if (v.is_number() || v.is_boolean()) return v.dump();  // scalars only: never serialise (deep) objects or arrays
  return def;
}
bool jhas(const json& j, const char* k) { return j.is_object() && j.contains(k) && !j[k].is_null(); }

static void civil_from_days(int64_t z, int& y, unsigned& m, unsigned& d) {
  z += 719468;
  const int64_t era = (z >= 0 ? z : z - 146096) / 146097;
  const unsigned doe = (unsigned)(z - era * 146097);
  const unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
  const int64_t yy = (int64_t)yoe + era * 400;
  const unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
  const unsigned mp = (5 * doy + 2) / 153;
  d = doy - (153 * mp + 2) / 5 + 1;
  m = mp < 10 ? mp + 3 : mp - 9;
  y = (int)(yy + (m <= 2));
}
static int64_t days_from_civil(int y, unsigned m, unsigned d) {
  y -= m <= 2;
  const int64_t era = (y >= 0 ? y : y - 399) / 400;
  const unsigned yoe = (unsigned)(y - era * 400);
  const unsigned doy = (153 * (m > 2 ? m - 3 : m + 9) + 2) / 5 + d - 1;
  const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  return era * 146097 + (int64_t)doe - 719468;
}

bool valid_date(const std::string& s) {
  int y; unsigned m, d;
  if (s.size() != 10 || s[4] != '-' || s[7] != '-') return false;
  if (sscanf(s.c_str(), "%d-%u-%u", &y, &m, &d) != 3) return false;
  if (m < 1 || m > 12 || d < 1 || d > 31 || y < 2000 || y > 2100) return false;
  int y2; unsigned m2, d2;
  civil_from_days(days_from_civil(y, m, d), y2, m2, d2);
  return y2 == y && m2 == m && d2 == d;
}

std::string hdate(const std::string& ymd) {
  static const char* mon[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
  int y; unsigned m, d;
  if (!valid_date(ymd) || sscanf(ymd.c_str(), "%d-%u-%u", &y, &m, &d) != 3) return ymd;
  return std::to_string(d) + " " + mon[m - 1] + " " + std::to_string(y);
}

std::string add_days(const std::string& ymd, int days) {
  int y; unsigned m, d;
  if (sscanf(ymd.c_str(), "%d-%u-%u", &y, &m, &d) != 3) return ymd;
  civil_from_days(days_from_civil(y, m, d) + days, y, m, d);
  char buf[16];
  snprintf(buf, sizeof buf, "%04d-%02u-%02u", y, m, d);
  return buf;
}

static std::tm utc_tm(std::time_t t) {
  std::tm g{};
#ifdef _WIN32
  gmtime_s(&g, &t);
#else
  gmtime_r(&t, &g);
#endif
  return g;
}
std::tm local_tm(std::time_t t) {
  std::tm l{};
#ifdef _WIN32
  localtime_s(&l, &t);
#else
  localtime_r(&t, &l);
#endif
  return l;
}

std::string now_iso() { return iso_after_seconds(0); }
std::string iso_after_seconds(long long secs) {
  std::tm g = utc_tm(std::time(nullptr) + (std::time_t)secs);
  char buf[32];
  strftime(buf, sizeof buf, "%Y-%m-%dT%H:%M:%SZ", &g);
  return buf;
}
std::string today_local() {
  std::tm l = local_tm(std::time(nullptr));
  char buf[16];
  strftime(buf, sizeof buf, "%Y-%m-%d", &l);
  return buf;
}

int compare_versions(const std::string& a, const std::string& b) {
  int pa[3] = {0, 0, 0}, pb[3] = {0, 0, 0};
  sscanf(a.c_str(), "%d.%d.%d", &pa[0], &pa[1], &pa[2]);
  sscanf(b.c_str(), "%d.%d.%d", &pb[0], &pb[1], &pb[2]);
  for (int i = 0; i < 3; i++)
    if (pa[i] != pb[i]) return pa[i] < pb[i] ? -1 : 1;
  return 0;
}

}  // namespace tf
