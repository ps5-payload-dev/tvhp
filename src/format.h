#ifndef FORMAT_H
#define FORMAT_H

#include <cstdint>
#include <string>

#include "htsp.h"

// Presentation helpers shared by the guide, the recordings list and the
// playback overlay. Pure functions of their arguments (plus the local
// timezone and, for FmtDay, the current date), so they can be tested on
// their own - see tools/test_format.cpp.
namespace textfmt {

// Unix seconds -> "20:00" in local time.
std::string FmtHM(int64_t t);

// "20:00 - 21:30".
std::string FmtRange(int64_t start, int64_t stop);

// "Today", "Tomorrow", "Yesterday", or "Wed 15 Jul" for anything further out.
std::string FmtDay(int64_t t);

// "45 min", "1 h", "1 h 30 min".
std::string FmtDuration(int64_t start, int64_t stop);

// Microseconds -> "1:23:45" or "23:45". Negative input gives "--:--".
std::string FmtPosition(int64_t us);

// Human-readable state of a DVR entry, e.g. "Recording now", "Scheduled",
// or the running time once it has finished.
std::string DvrStateLabel(const HtspDvrEntry& e);

} // namespace textfmt

#endif
