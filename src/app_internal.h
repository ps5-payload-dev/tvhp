#ifndef APP_INTERNAL_H
#define APP_INTERNAL_H

#include <cstddef>
#include <cstdint>

#include <RmlUi/Core.h>

// Constants and helpers shared between the App translation units
// (app.cpp, app_connect.cpp, app_guide.cpp, app_recordings.cpp,
// app_watch.cpp). Not part of the public interface of App.
namespace appdetail {

inline constexpr const char* kConfigPath = "tvhp.cfg";

// Row geometry; must match the stylesheets (.channel-row / .epg-row /
// .rec-row / .server-row height + margin-bottom) so keyboard scrolling can be
// computed without layout queries on the generated rows.
inline constexpr float kChannelRowPitch = 96.0f + 12.0f;
inline constexpr float kEpgRowPitch = 76.0f + 10.0f;
inline constexpr float kRecRowPitch = 104.0f + 12.0f;
inline constexpr float kServerRowPitch = 96.0f + 12.0f;

inline constexpr size_t kMaxEpgRows = 80;
inline constexpr double kRefreshIntervalSec = 20.0; // progress bars, clock drift
inline constexpr double kToastSec = 4.0;
inline constexpr double kConfirmWindowSec = 5.0;    // destructive double-press
inline constexpr double kWatchInfoSec = 5.0;        // watch info bar auto-hide

inline constexpr int64_t kSeekSmallUs = 30 * 1000000LL;
inline constexpr int64_t kSeekLargeUs = 300 * 1000000LL;

// Seconds since startup, from RmlUi's system interface.
inline double Now()
{
  Rml::SystemInterface* sys = Rml::GetSystemInterface();
  return sys ? sys->GetElapsedTime() : 0.0;
}

} // namespace appdetail

#endif
