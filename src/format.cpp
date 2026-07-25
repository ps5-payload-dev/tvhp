#include <cstdio>
#include <ctime>

#include "format.h"

namespace textfmt {

std::string FmtHM(int64_t t) {
  char buf[16] = {};
  time_t tt = (time_t)t;
  struct tm tm;
  localtime_r(&tt, &tm);
  std::snprintf(buf, sizeof(buf), "%02d:%02d", tm.tm_hour, tm.tm_min);
  return buf;
}

std::string FmtRange(int64_t start, int64_t stop) {
  return FmtHM(start) + " - " + FmtHM(stop);
}

// "Today", "Tomorrow", "Yesterday" or "Wed 15 Jul".
std::string FmtDay(int64_t t) {
  const time_t now = time(nullptr);
  time_t tt = (time_t)t;
  struct tm tm_now, tm_ev;
  localtime_r(&now, &tm_now);
  localtime_r(&tt, &tm_ev);

  const int day_now = tm_now.tm_yday + tm_now.tm_year * 366;
  const int day_ev = tm_ev.tm_yday + tm_ev.tm_year * 366;
  if (day_ev == day_now)
    return "Today";
  if (day_ev == day_now + 1)
    return "Tomorrow";
  if (day_ev == day_now - 1)
    return "Yesterday";
  char buf[32] = {};
  strftime(buf, sizeof(buf), "%a %e %b", &tm_ev);
  return buf;
}

std::string FmtDuration(int64_t start, int64_t stop) {
  const int64_t mins = std::max<int64_t>(0, (stop - start) / 60);
  if (mins < 60)
    return std::to_string(mins) + " min";
  std::string out = std::to_string(mins / 60) + " h";
  if (mins % 60)
    out += " " + std::to_string(mins % 60) + " min";
  return out;
}

// Microseconds -> "1:23:45" / "23:45", for the playback position.
std::string FmtPosition(int64_t us) {
  if (us < 0)
    return "--:--";
  const int64_t total = us / 1000000;
  const int64_t h = total / 3600, m = (total / 60) % 60, s = total % 60;
  char buf[24] = {};
  if (h > 0)
    std::snprintf(buf, sizeof(buf), "%lld:%02lld:%02lld", (long long)h, (long long)m, (long long)s);
  else
    std::snprintf(buf, sizeof(buf), "%lld:%02lld", (long long)m, (long long)s);
  return buf;
}

// Human-readable state for a DVR entry.
std::string DvrStateLabel(const HtspDvrEntry& e) {
  if (e.IsRecording())
    return "Recording now";
  if (e.IsScheduled())
    return e.enabled ? "Scheduled" : "Disabled";
  if (e.IsFinished())
    return FmtDuration(e.start, e.stop);
  if (e.state == "missed")
    return "Missed";
  if (!e.error.empty())
    return e.error;
  return e.state.empty() ? "Unknown" : e.state;
}

} // namespace textfmt
