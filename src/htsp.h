#ifndef HTSP_CLIENT_H
#define HTSP_CLIENT_H

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <ctime>
#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

extern "C" {
#include "htsmsg.h"
#include "net.h"
}

struct HtspChannel {
  uint32_t id = 0;
  uint32_t number = 0;
  std::string name;
  std::string icon_url;       // http(s) URL served by tvheadend, may be empty
  uint32_t event_id = 0;      // current EPG event, 0 = unknown
  uint32_t next_event_id = 0; // next EPG event, 0 = unknown
};

// One EPG event, as pushed by eventAdd/eventUpdate. Times are unix seconds.
struct HtspEvent {
  uint32_t id = 0;
  uint32_t channel_id = 0;
  int64_t start = 0;
  int64_t stop = 0;
  std::string title;
  std::string subtitle;
  std::string summary;
  std::string description;
  uint32_t season = 0;  // 0 = not set
  uint32_t episode = 0; // 0 = not set
  uint32_t content_type = 0;
  uint32_t dvr_id = 0;  // recording scheduled for this event, 0 = none
};

// One DVR entry (a scheduled, running or finished recording), as pushed by
// dvrEntryAdd/dvrEntryUpdate. Times are unix seconds.
struct HtspDvrEntry {
  uint32_t id = 0;
  uint32_t channel_id = 0;
  uint32_t event_id = 0;
  int64_t start = 0;       // scheduled start (excluding start_extra)
  int64_t stop = 0;
  int64_t start_extra = 0; // pre-roll, minutes
  int64_t stop_extra = 0;  // post-roll, minutes
  std::string title;
  std::string subtitle;
  std::string summary;
  std::string description;
  std::string channel_name;
  std::string state; // "scheduled", "recording", "completed", "missed", "invalid"
  std::string error;
  int64_t data_size = 0;
  uint32_t season = 0;
  uint32_t episode = 0;
  bool enabled = true;

  bool IsScheduled() const { return state == "scheduled"; }
  bool IsRecording() const { return state == "recording"; }
  bool IsFinished() const { return state == "completed"; }
  // Whether a file exists that can be streamed with the HTSP file API. An
  // in-progress recording can be played too - the server keeps serving the
  // growing file, which is the main reason for preferring fileOpen over HTTP.
  bool HasFile() const { return IsFinished() || IsRecording(); }
  // Upcoming or in progress, i.e. cancelling it is meaningful.
  bool IsPending() const { return IsScheduled() || IsRecording(); }
};

// One elementary stream announced in subscriptionStart.
struct HtspStreamInfo {
  uint32_t index = 0;
  std::string type; // "H264", "HEVC", "MPEG2VIDEO", "AC3", "AAC", ...
  uint32_t width = 0, height = 0;   // video
  uint32_t channels = 0, rate = 0;  // audio
};

// One demuxed packet (muxpkt). Timestamps are in microseconds.
struct HtspMuxPacket {
  uint32_t stream = 0;
  int64_t pts = INT64_MIN; // INT64_MIN = unknown
  int64_t dts = INT64_MIN;
  int64_t duration = 0;
  char frametype = 0; // 'I', 'P', 'B' or 0
  std::vector<uint8_t> payload;
};

// HTSP (Home Tv Streaming Protocol) client built on libhts (htsmsg binary
// serialization, as used by Kodi's pvr.hts addon).
//
// Protocol flow:
//   hello                -> server capabilities + 32-byte auth challenge
//   authenticate         -> digest = SHA1(password + challenge)
//   enableAsyncMetadata  -> server pushes channelAdd/eventAdd/dvrEntryAdd/...
//                           then initialSyncCompleted
//   subscribe(channelId) -> subscriptionStart followed by a stream of muxpkt
//   fileOpen("/dvrfile/ID") + fileRead/fileSeek -> recording playback
class HtspClient {
public:
  ~HtspClient();

  // Connects to the HTSP port (default 9982) and performs hello +
  // authenticate. Blocking; returns false and fills 'error' on failure.
  // Safe to call again after a failed connection or a Disconnect().
  bool Connect(const std::string& host, int port, const std::string& user, const std::string& pass, std::string& error);

  // Sends enableAsyncMetadata (channels + EPG + DVR entries) and starts the
  // reader thread. on_sync is invoked (from the reader thread) once the
  // initial sync completes; on_update is invoked on any later channel
  // add/update/delete with the full refreshed list. Callbacks must be
  // thread-safe, and both may be null: polling Generation() from the UI
  // thread is the recommended way to observe channel/EPG/DVR changes.
  void StartAsync(std::function<void(std::vector<HtspChannel>)> on_sync = {},
		  std::function<void(std::vector<HtspChannel>)> on_update = {});

  // --- Thread-safe snapshots for the UI --------------------------------
  // Monotonic counter bumped on every channel, EPG or DVR mutation (and on
  // initial sync / disconnect). Poll it once per frame and refresh views
  // when it changes.
  uint64_t Generation() const { return generation.load(); }

  // True once StartAsync() has run and the connection is still alive.
  bool Connected() const { return running.load(); }

  // True once the server has finished the initial channel/EPG dump.
  bool SyncCompleted() const;

  // Channels sorted by number, then name.
  std::vector<HtspChannel> GetChannels() const;

  // Upcoming (and currently airing) events for a channel, sorted by start
  // time. max_events = 0 means no limit.
  std::vector<HtspEvent> GetChannelEvents(uint32_t channel_id, size_t max_events = 0) const;

  // Fills the currently airing and next event for a channel. Either output
  // may be left untouched; the return values indicate what was found.
  struct NowNext {
    bool has_now = false;
    bool has_next = false;
    HtspEvent now;
    HtspEvent next;
  };
  NowNext GetNowNext(uint32_t channel_id) const;

  // --- DVR (recordings) --------------------------------------------------
  // All known DVR entries, sorted with pending recordings first (soonest
  // first) and finished ones after them (most recent first).
  std::vector<HtspDvrEntry> GetDvrEntries() const;

  // Looks up the recording scheduled for an EPG event. Returns false when
  // the event is not being recorded.
  bool GetDvrEntryForEvent(uint32_t event_id, HtspDvrEntry* out) const;
  bool GetDvrEntry(uint32_t dvr_id, HtspDvrEntry* out) const;

  // Schedules a recording for an EPG event (addDvrEntry). On success the new
  // entry id is stored in *out_id when out_id is non-null. The entry itself
  // arrives asynchronously as a dvrEntryAdd.
  bool RecordEvent(uint32_t event_id, std::string& error, uint32_t* out_id = nullptr);

  // Schedules a manual (time based) recording on a channel.
  bool RecordTimespan(uint32_t channel_id, int64_t start, int64_t stop,
		      const std::string& title, std::string& error, uint32_t* out_id = nullptr);

  // cancelDvrEntry keeps the entry (and any partial file) but stops or
  // unschedules it; deleteDvrEntry removes the entry and its file.
  bool CancelDvrEntry(uint32_t dvr_id, std::string& error);
  bool DeleteDvrEntry(uint32_t dvr_id, std::string& error);

  // --- Live streaming (subscribe / muxpkt) --------------------------------
  // Callbacks fire on the reader thread and must not block for long.
  struct StreamCallbacks {
    std::function<void(std::vector<HtspStreamInfo>)> on_start;
    std::function<void(HtspMuxPacket&&)> on_packet;
    std::function<void(std::string)> on_status; // subscriptionStatus / stop reason
  };
  void SetStreamCallbacks(StreamCallbacks cbs);

  // Subscribes to a channel (replacing any active subscription).
  bool Subscribe(uint32_t channel_id, std::string& error);
  void Unsubscribe();

  // --- File access (recording playback) -----------------------------------
  // Server-side file handles, used to stream recordings over the same
  // connection. Requires HTSP v8+. Paths are "/dvrfile/<id>" for recordings
  // and "/imagecache/<id>" for cached artwork.
  //
  // Safe to call from a decoder thread while the UI thread issues other
  // requests.
  bool FileOpen(const std::string& path, uint32_t* handle, int64_t* size, std::string& error);
  // Reads up to 'size' bytes. offset < 0 reads from the server-side current
  // position. Returns the number of bytes read (may be less than requested,
  // 0 at end of file) or -1 on error.
  int64_t FileRead(uint32_t handle, void* buf, int64_t size, int64_t offset, std::string& error);
  // whence is "SEEK_SET", "SEEK_CUR" or "SEEK_END". Returns the new absolute
  // position, or -1 on error.
  int64_t FileSeek(uint32_t handle, int64_t offset, const char* whence, std::string& error);
  bool FileStat(uint32_t handle, int64_t* size, std::string& error);
  void FileClose(uint32_t handle);

  void Disconnect();

  std::string ServerInfo() const { return server_info; }
  int ProtocolVersion() const { return htsp_version; }
  // Recording playback needs fileOpen, which arrived in HTSP v8.
  bool SupportsFileApi() const { return htsp_version >= 8; }

private:
  // Sends a request and waits for the seq-matched reply (reader thread
  // dispatches it). Takes ownership of 'msg'. Returns nullptr on timeout /
  // disconnect; caller owns the returned message.
  htsmsg_t* SendRequest(htsmsg_t* msg, int timeout_ms, std::string& error);

  // Sends a request whose reply only carries success/error (and optionally
  // the id of a created object). Takes ownership of 'msg'.
  bool SimpleRequest(htsmsg_t* msg, std::string& error, uint32_t* out_id = nullptr);

  // Synchronous send/receive used during the handshake, before the reader
  // thread exists.
  bool SendMessage(htsmsg_t* msg, std::string& error); // takes ownership
  htsmsg_t* ReadMessage(int timeout_ms, std::string& error);

  void ReaderLoop();
  void HandleAsyncMessage(htsmsg_t* msg); // does not take ownership

  // EPG helpers; caller must hold channels_mutex.
  void UpsertEventLocked(htsmsg_t* msg, bool is_update);
  void RemoveEventLocked(uint32_t event_id);
  void PruneExpiredLocked(time_t now);

  // DVR helpers; caller must hold channels_mutex.
  void UpsertDvrEntryLocked(htsmsg_t* msg);
  void RemoveDvrEntryLocked(uint32_t dvr_id);

  socket_t sock = -1;
  std::mutex write_mutex;

  std::thread reader;
  std::atomic<bool> running{false};

  // seq -> slot for the pending reply
  struct Pending {
    htsmsg_t* reply = nullptr;
    bool done = false;
  };
  std::mutex pending_mutex;
  std::condition_variable pending_cv;
  std::map<uint32_t, Pending*> pending;
  uint32_t next_seq = 1;

  // Guards channels, EPG/DVR storage and initial_sync_done.
  mutable std::mutex channels_mutex;
  std::map<uint32_t, HtspChannel> channels; // keyed by channelId
  // EPG: per-channel events keyed by start time, plus a global index for
  // eventUpdate/eventDelete which only carry the eventId.
  std::map<uint32_t, std::map<int64_t, HtspEvent>> epg; // channelId -> start -> event
  struct EventLocator { uint32_t channel_id = 0; int64_t start = 0; };
  std::map<uint32_t, EventLocator> event_index; // eventId -> locator
  std::map<uint32_t, HtspDvrEntry> dvr;         // dvrId -> entry
  std::map<uint32_t, uint32_t> dvr_by_event;    // eventId -> dvrId
  time_t last_prune = 0;
  bool initial_sync_done = false;
  std::function<void(std::vector<HtspChannel>)> sync_cb;
  std::function<void(std::vector<HtspChannel>)> update_cb;

  std::atomic<uint64_t> generation{0};

  std::string server_info;
  int htsp_version = 0;

  StreamCallbacks stream_cbs;
  std::atomic<uint32_t> subscription_id{0}; // 0 = no active subscription
  uint32_t next_subscription_id = 1;
};

#endif
