#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstdlib>
#include <cstring>

#include <sys/socket.h>

extern "C" {
#include "htsmsg_binary.h"
#include "sha1.h"
}

#include "htsp.h"

namespace {

  // Writes the whole buffer; returns 0 on success or errno.
  int WriteAll(socket_t fd, const void* data, size_t len)
  {
    const char* p = static_cast<const char*>(data);
    while (len > 0)
      {
	const ssize_t n = ::send(fd, p, len, MSG_NOSIGNAL);
	if (n < 0)
	  {
	    if (errno == EINTR)
	      continue;
	    return errno;
	  }
	p += n;
	len -= (size_t)n;
      }
    return 0;
  }

  constexpr int kHtspVersion = 35;      // protocol version we speak
  constexpr int kMaxMessageSize = 10 * 1024 * 1024;
  constexpr int kReadPollMs = 250;      // reader thread poll granularity
  constexpr int kRequestTimeoutMs = 5000;
  // fileRead can carry up to a megabyte of payload and may have to wait for
  // the server to flush an in-progress recording, so it gets more headroom.
  constexpr int kFileRequestTimeoutMs = 15000;
  constexpr int64_t kEpgWindowSec = 24 * 3600; // how far ahead we ask for EPG
  constexpr time_t kPruneIntervalSec = 600;    // drop expired events this often

  std::vector<HtspChannel> SortedChannels(const std::map<uint32_t, HtspChannel>& by_id)
  {
    std::vector<HtspChannel> out;
    out.reserve(by_id.size());
    for (const auto& [id, ch] : by_id)
      out.push_back(ch);
    std::sort(out.begin(), out.end(), [](const HtspChannel& a, const HtspChannel& b) {
      if (a.number != b.number)
	return a.number < b.number;
      return a.name < b.name;
    });
    return out;
  }

  // Extracts a human-readable error from an HTSP reply, if any.
  std::string ReplyError(htsmsg_t* reply)
  {
    if (!reply)
      return "no reply";
    if (const char* err = htsmsg_get_str(reply, "error"))
      return err;
    uint32_t noaccess = 0;
    if (!htsmsg_get_u32(reply, "noaccess", &noaccess) && noaccess)
      return "access denied (check username/password and streaming rights)";
    return {};
  }

} // namespace

HtspClient::~HtspClient()
{
  Disconnect();
}

// ---------------------------------------------------------------------------
// Wire I/O
// ---------------------------------------------------------------------------

bool HtspClient::SendMessage(htsmsg_t* msg, std::string& error)
{
  void* data = nullptr;
  size_t len = 0;
  if (htsmsg_binary_serialize(msg, &data, &len, kMaxMessageSize) < 0)
    {
      htsmsg_destroy(msg);
      error = "failed to serialize HTSP message";
      return false;
    }
  htsmsg_destroy(msg);

  std::lock_guard<std::mutex> lock(write_mutex);
  const int rc = WriteAll(sock, data, len);
  free(data);
  if (rc)
    {
      error = std::string("send failed: ") + std::strerror(rc);
      return false;
    }
  return true;
}

htsmsg_t* HtspClient::ReadMessage(int timeout_ms, std::string& error)
{
  uint8_t lenbuf[4];
  int rc = htsp_tcp_read_timeout(sock, lenbuf, 4, timeout_ms);
  if (rc)
    {
      error = (rc == ETIMEDOUT) ? "timeout" : std::string("read failed: ") + std::strerror(rc);
      return nullptr;
    }

  const uint32_t len = (uint32_t(lenbuf[0]) << 24) | (uint32_t(lenbuf[1]) << 16) | (uint32_t(lenbuf[2]) << 8) | uint32_t(lenbuf[3]);
  if (len == 0 || len > kMaxMessageSize)
    {
      error = "invalid HTSP message length";
      return nullptr;
    }

  void* buf = std::malloc(len);
  if (!buf)
    {
      error = "out of memory";
      return nullptr;
    }

  rc = htsp_tcp_read_timeout(sock, buf, len, kRequestTimeoutMs);
  if (rc)
    {
      std::free(buf);
      error = std::string("read failed: ") + std::strerror(rc);
      return nullptr;
    }

  // The message takes ownership of 'buf' and frees it on htsmsg_destroy.
  htsmsg_t* msg = htsmsg_binary_deserialize(buf, len, buf);
  if (!msg)
    error = "failed to deserialize HTSP message";
  return msg;
}

// ---------------------------------------------------------------------------
// Handshake
// ---------------------------------------------------------------------------

bool HtspClient::Connect(const std::string& host, int port, const std::string& user, const std::string& pass, std::string& error)
{
  Disconnect(); // joins any previous reader thread and closes the socket

  {
    // Fresh session: forget everything from a previous connection.
    std::lock_guard<std::mutex> lock(channels_mutex);
    channels.clear();
    epg.clear();
    event_index.clear();
    dvr.clear();
    dvr_by_event.clear();
    initial_sync_done = false;
  }
  generation++;

  char errbuf[256] = {};
  sock = htsp_tcp_connect(host.c_str(), port, errbuf, sizeof(errbuf), 5000);
  if (sock < 0)
    {
      error = std::string("connect failed: ") + errbuf;
      return false;
    }

  // Replies are paired to requests via the echoed "seq" field; we tag every
  // request with one (as Kodi's pvr.hts does), including the handshake.
  auto read_reply = [this](uint32_t want_seq, std::string& err) -> htsmsg_t* {
    for (;;)
      {
	htsmsg_t* m = ReadMessage(kRequestTimeoutMs, err);
	if (!m)
	  return nullptr;
	uint32_t seq = 0;
	if (!htsmsg_get_u32(m, "seq", &seq) && seq == want_seq)
	  return m;
	htsmsg_destroy(m); // unrelated async message; ignore during handshake
      }
  };

  // --- hello ---
  htsmsg_t* hello = htsmsg_create_map();
  htsmsg_add_str(hello, "method", "hello");
  htsmsg_add_str(hello, "clientname", "tvhp");
  htsmsg_add_str(hello, "clientversion", "0.1");
  htsmsg_add_u32(hello, "htspversion", kHtspVersion);
  uint32_t seq = next_seq++;
  htsmsg_add_u32(hello, "seq", seq);
  if (!SendMessage(hello, error))
    return false;

  htsmsg_t* reply = read_reply(seq, error);
  if (!reply)
    {
      error = "hello failed: " + error;
      return false;
    }

  uint32_t version = 0;
  htsmsg_get_u32(reply, "htspversion", &version);
  htsp_version = (int)version;
  const char* server_name = htsmsg_get_str(reply, "servername");
  const char* server_ver = htsmsg_get_str(reply, "serverversion");
  server_info = std::string(server_name ? server_name : "Tvheadend") + " " + (server_ver ? server_ver : "") +
    " (HTSP v" + std::to_string(htsp_version) + ")";

  const void* challenge = nullptr;
  size_t challenge_len = 0;
  htsmsg_get_bin(reply, "challenge", &challenge, &challenge_len);

  // --- authenticate ---
  htsmsg_t* auth = htsmsg_create_map();
  htsmsg_add_str(auth, "method", "authenticate");
  if (!user.empty())
    {
      htsmsg_add_str(auth, "username", user.c_str());
      if (challenge && challenge_len > 0)
	{
	  // digest = SHA1(password + challenge)
	  HTSSHA1* ctx = (HTSSHA1*)std::malloc(hts_sha1_size);
	  uint8_t digest[20];
	  hts_sha1_init(ctx);
	  hts_sha1_update(ctx, (const uint8_t*)pass.data(), (unsigned)pass.size());
	  hts_sha1_update(ctx, (const uint8_t*)challenge, (unsigned)challenge_len);
	  hts_sha1_final(ctx, digest);
	  std::free(ctx);
	  htsmsg_add_bin(auth, "digest", digest, sizeof(digest));
	}
    }
  htsmsg_destroy(reply);

  seq = next_seq++;
  htsmsg_add_u32(auth, "seq", seq);
  if (!SendMessage(auth, error))
    return false;

  reply = read_reply(seq, error);
  if (!reply)
    {
      error = "authenticate failed: " + error;
      return false;
    }
  const std::string auth_error = ReplyError(reply);
  htsmsg_destroy(reply);
  if (!auth_error.empty())
    {
      error = "authenticate failed: " + auth_error;
      return false;
    }
  return true;
}

// ---------------------------------------------------------------------------
// Async metadata + reader thread
// ---------------------------------------------------------------------------

void HtspClient::StartAsync(std::function<void(std::vector<HtspChannel>)> on_sync,
			    std::function<void(std::vector<HtspChannel>)> on_update)
{
  sync_cb = std::move(on_sync);
  update_cb = std::move(on_update);

  running = true;
  reader = std::thread(&HtspClient::ReaderLoop, this);

  // Channels plus a rolling EPG window (eventAdd/eventUpdate/eventDelete).
  htsmsg_t* msg = htsmsg_create_map();
  htsmsg_add_str(msg, "method", "enableAsyncMetadata");
  htsmsg_add_u32(msg, "epg", 1);
  htsmsg_add_s64(msg, "epgMaxTime", (int64_t)time(nullptr) + kEpgWindowSec);
  std::string error;
  if (htsmsg_t* reply = SendRequest(msg, kRequestTimeoutMs, error))
    htsmsg_destroy(reply);
}

void HtspClient::ReaderLoop()
{
  while (running)
    {
      std::string error;
      htsmsg_t* msg = ReadMessage(kReadPollMs, error);
      if (!msg)
	{
	  if (error == "timeout")
	    continue; // poll granularity; check running flag and retry
	  running = false;
	  generation++; // let pollers notice the connection loss
	  // Fail all waiters so callers don't block until timeout.
	  std::lock_guard<std::mutex> lock(pending_mutex);
	  for (auto& [seq, slot] : pending)
	    slot->done = true;
	  pending_cv.notify_all();
	  break;
	}

      uint32_t seq = 0;
      if (!htsmsg_get_u32(msg, "seq", &seq))
	{
	  // Reply to a pending request.
	  std::lock_guard<std::mutex> lock(pending_mutex);
	  auto it = pending.find(seq);
	  if (it != pending.end())
	    {
	      it->second->reply = msg;
	      it->second->done = true;
	      pending_cv.notify_all();
	      continue; // ownership moved to the waiter
	    }
	  htsmsg_destroy(msg);
	  continue;
	}

      HandleAsyncMessage(msg);
      htsmsg_destroy(msg);
    }
}

void HtspClient::HandleAsyncMessage(htsmsg_t* msg)
{
  const char* method = htsmsg_get_str(msg, "method");
  if (!method)
    return;

  if (!std::strcmp(method, "channelAdd") || !std::strcmp(method, "channelUpdate"))
    {
      uint32_t id = 0;
      if (htsmsg_get_u32(msg, "channelId", &id))
	return;

      std::lock_guard<std::mutex> lock(channels_mutex);
      HtspChannel& ch = channels[id];
      ch.id = id;
      uint32_t u32 = 0;
      if (!htsmsg_get_u32(msg, "channelNumber", &u32))
	ch.number = u32;
      if (const char* name = htsmsg_get_str(msg, "channelName"))
	ch.name = name;
      if (!htsmsg_get_u32(msg, "eventId", &u32))
	ch.event_id = u32;
      generation++;
      if (initial_sync_done && update_cb)
	update_cb(SortedChannels(channels));
    }
  else if (!std::strcmp(method, "channelDelete"))
    {
      uint32_t id = 0;
      if (htsmsg_get_u32(msg, "channelId", &id))
	return;
      std::lock_guard<std::mutex> lock(channels_mutex);
      channels.erase(id);
      // Drop this channel's EPG too.
      if (auto it = epg.find(id); it != epg.end())
	{
	  for (const auto& [start, ev] : it->second)
	    event_index.erase(ev.id);
	  epg.erase(it);
	}
      generation++;
      if (initial_sync_done && update_cb)
	update_cb(SortedChannels(channels));
    }
  else if (!std::strcmp(method, "eventAdd") || !std::strcmp(method, "eventUpdate"))
    {
      std::lock_guard<std::mutex> lock(channels_mutex);
      UpsertEventLocked(msg, !std::strcmp(method, "eventUpdate"));
      const time_t now = time(nullptr);
      if (now - last_prune > kPruneIntervalSec)
	PruneExpiredLocked(now);
      generation++;
    }
  else if (!std::strcmp(method, "eventDelete"))
    {
      uint32_t id = 0;
      if (htsmsg_get_u32(msg, "eventId", &id))
	return;
      std::lock_guard<std::mutex> lock(channels_mutex);
      RemoveEventLocked(id);
      generation++;
    }
  else if (!std::strcmp(method, "dvrEntryAdd") || !std::strcmp(method, "dvrEntryUpdate"))
    {
      std::lock_guard<std::mutex> lock(channels_mutex);
      UpsertDvrEntryLocked(msg);
      generation++;
    }
  else if (!std::strcmp(method, "dvrEntryDelete"))
    {
      uint32_t id = 0;
      if (htsmsg_get_u32(msg, "id", &id))
	return;
      std::lock_guard<std::mutex> lock(channels_mutex);
      RemoveDvrEntryLocked(id);
      generation++;
    }
  else if (!std::strcmp(method, "initialSyncCompleted"))
    {
      std::lock_guard<std::mutex> lock(channels_mutex);
      initial_sync_done = true;
      generation++;
      if (sync_cb)
	sync_cb(SortedChannels(channels));
    }
  else if (!std::strcmp(method, "subscriptionStart"))
    {
      uint32_t sid = 0;
      htsmsg_get_u32(msg, "subscriptionId", &sid);
      if (sid != subscription_id || !stream_cbs.on_start)
	return;

      std::vector<HtspStreamInfo> streams;
      if (htsmsg_t* list = htsmsg_get_list(msg, "streams"))
	{
	  htsmsg_field_t* f;
	  HTSMSG_FOREACH(f, list)
	    {
	      htsmsg_t* s = htsmsg_get_map_by_field(f);
	      if (!s)
		continue;
	      HtspStreamInfo info;
	      htsmsg_get_u32(s, "index", &info.index);
	      if (const char* type = htsmsg_get_str(s, "type"))
		info.type = type;
	      htsmsg_get_u32(s, "width", &info.width);
	      htsmsg_get_u32(s, "height", &info.height);
	      htsmsg_get_u32(s, "channels", &info.channels);
	      htsmsg_get_u32(s, "rate", &info.rate);
	      streams.push_back(std::move(info));
	    }
	}
      stream_cbs.on_start(std::move(streams));
    }
  else if (!std::strcmp(method, "muxpkt"))
    {
      uint32_t sid = 0;
      htsmsg_get_u32(msg, "subscriptionId", &sid);
      if (sid != subscription_id || !stream_cbs.on_packet)
	return;

      HtspMuxPacket pkt;
      htsmsg_get_u32(msg, "stream", &pkt.stream);
      int64_t s64 = 0;
      if (!htsmsg_get_s64(msg, "pts", &s64))
	pkt.pts = s64;
      if (!htsmsg_get_s64(msg, "dts", &s64))
	pkt.dts = s64;
      if (!htsmsg_get_s64(msg, "duration", &s64))
	pkt.duration = s64;
      uint32_t frametype = 0;
      if (!htsmsg_get_u32(msg, "frametype", &frametype))
	pkt.frametype = (char)frametype;

      const void* bin = nullptr;
      size_t bin_len = 0;
      if (!htsmsg_get_bin(msg, "payload", &bin, &bin_len) && bin_len > 0)
	{
	  pkt.payload.assign((const uint8_t*)bin, (const uint8_t*)bin + bin_len);
	  stream_cbs.on_packet(std::move(pkt));
	}
    }
  else if (!std::strcmp(method, "subscriptionStatus") || !std::strcmp(method, "subscriptionStop"))
    {
      uint32_t sid = 0;
      htsmsg_get_u32(msg, "subscriptionId", &sid);
      if (sid != subscription_id || !stream_cbs.on_status)
	return;
      const char* status = htsmsg_get_str(msg, "status");
      if (!status)
	status = htsmsg_get_str(msg, "reason");
      if (!std::strcmp(method, "subscriptionStop"))
	stream_cbs.on_status(std::string("Stream stopped") + (status ? std::string(": ") + status : ""));
      else if (status)
	stream_cbs.on_status(status);
    }
}

// ---------------------------------------------------------------------------
// EPG storage
// ---------------------------------------------------------------------------

void HtspClient::UpsertEventLocked(htsmsg_t* msg, bool is_update)
{
  uint32_t event_id = 0;
  if (htsmsg_get_u32(msg, "eventId", &event_id))
    return;

  // Start from the previous version of the event: eventUpdate only carries
  // the fields that changed.
  HtspEvent ev;
  if (auto idx = event_index.find(event_id); idx != event_index.end())
    {
      auto& by_start = epg[idx->second.channel_id];
      if (auto it = by_start.find(idx->second.start); it != by_start.end())
	{
	  ev = it->second;
	  by_start.erase(it); // re-inserted below (start time may change)
	}
      event_index.erase(idx);
    }
  else if (is_update)
    {
      // Update for an event we never saw; treat it as an add if it carries a
      // channelId, otherwise there is nothing we can do with it.
      uint32_t channel_id = 0;
      if (htsmsg_get_u32(msg, "channelId", &channel_id))
	return;
    }

  ev.id = event_id;
  uint32_t u32 = 0;
  int64_t s64 = 0;
  if (!htsmsg_get_u32(msg, "channelId", &u32))
    ev.channel_id = u32;
  if (!htsmsg_get_s64(msg, "start", &s64))
    ev.start = s64;
  if (!htsmsg_get_s64(msg, "stop", &s64))
    ev.stop = s64;
  if (const char* s = htsmsg_get_str(msg, "title"))
    ev.title = s;
  if (const char* s = htsmsg_get_str(msg, "subtitle"))
    ev.subtitle = s;
  if (const char* s = htsmsg_get_str(msg, "summary"))
    ev.summary = s;
  if (const char* s = htsmsg_get_str(msg, "description"))
    ev.description = s;
  if (!htsmsg_get_u32(msg, "seasonNumber", &u32))
    ev.season = u32;
  if (!htsmsg_get_u32(msg, "episodeNumber", &u32))
    ev.episode = u32;

  if (!ev.channel_id || !ev.start)
    return;

  epg[ev.channel_id][ev.start] = ev;
  event_index[event_id] = EventLocator{ev.channel_id, ev.start};
}

void HtspClient::RemoveEventLocked(uint32_t event_id)
{
  auto idx = event_index.find(event_id);
  if (idx == event_index.end())
    return;
  if (auto ch = epg.find(idx->second.channel_id); ch != epg.end())
    ch->second.erase(idx->second.start);
  event_index.erase(idx);
}

void HtspClient::PruneExpiredLocked(time_t now)
{
  last_prune = now;
  for (auto& [channel_id, by_start] : epg)
    {
      for (auto it = by_start.begin(); it != by_start.end();)
	{
	  if (it->second.stop && it->second.stop < now)
	    {
	      event_index.erase(it->second.id);
	      it = by_start.erase(it);
	    }
	  else
	    ++it;
	}
    }
}

// ---------------------------------------------------------------------------
// Thread-safe snapshots for the UI
// ---------------------------------------------------------------------------

bool HtspClient::SyncCompleted() const
{
  std::lock_guard<std::mutex> lock(channels_mutex);
  return initial_sync_done;
}

std::vector<HtspChannel> HtspClient::GetChannels() const
{
  std::lock_guard<std::mutex> lock(channels_mutex);
  return SortedChannels(channels);
}

std::vector<HtspEvent> HtspClient::GetChannelEvents(uint32_t channel_id, size_t max_events) const
{
  std::vector<HtspEvent> out;
  const time_t now = time(nullptr);

  std::lock_guard<std::mutex> lock(channels_mutex);
  auto ch = epg.find(channel_id);
  if (ch == epg.end())
    return out;

  for (const auto& [start, ev] : ch->second)
    {
      if (ev.stop && ev.stop < now)
	continue; // already over
      out.push_back(ev);
      if (max_events && out.size() >= max_events)
	break;
    }
  return out;
}

HtspClient::NowNext HtspClient::GetNowNext(uint32_t channel_id) const
{
  NowNext result;
  const time_t now = time(nullptr);

  std::lock_guard<std::mutex> lock(channels_mutex);
  auto ch = epg.find(channel_id);
  if (ch == epg.end())
    return result;

  const auto& by_start = ch->second;
  // First event starting after 'now'; the one before it (if any) may still
  // be airing.
  auto after = by_start.upper_bound(now);
  if (after != by_start.begin())
    {
      auto cur = std::prev(after);
      if (cur->second.stop > now)
	{
	  result.has_now = true;
	  result.now = cur->second;
	}
    }
  return result;
}

// ---------------------------------------------------------------------------
// Requests
// ---------------------------------------------------------------------------

htsmsg_t* HtspClient::SendRequest(htsmsg_t* msg, int timeout_ms, std::string& error)
{
  if (!running)
    {
      htsmsg_destroy(msg);
      error = "not connected";
      return nullptr;
    }

  Pending slot;
  uint32_t seq;
  {
    std::lock_guard<std::mutex> lock(pending_mutex);
    seq = next_seq++;
    pending[seq] = &slot;
  }
  htsmsg_add_u32(msg, "seq", seq);

  if (!SendMessage(msg, error))
    {
      std::lock_guard<std::mutex> lock(pending_mutex);
      pending.erase(seq);
      return nullptr;
    }

  std::unique_lock<std::mutex> lock(pending_mutex);
  const bool ok = pending_cv.wait_for(lock, std::chrono::milliseconds(timeout_ms), [&] { return slot.done; });
  pending.erase(seq);
  if (!ok || !slot.reply)
    {
      error = ok ? "connection lost" : "request timed out";
      return nullptr;
    }
  return slot.reply;
}

void HtspClient::SetStreamCallbacks(StreamCallbacks cbs)
{
  stream_cbs = std::move(cbs);
}

bool HtspClient::Subscribe(uint32_t channel_id, std::string& error)
{
  Unsubscribe();

  const uint32_t sid = next_subscription_id++;

  htsmsg_t* msg = htsmsg_create_map();
  htsmsg_add_str(msg, "method", "subscribe");
  htsmsg_add_u32(msg, "subscriptionId", sid);
  htsmsg_add_u32(msg, "channelId", channel_id);
  htsmsg_add_str(msg, "profile", "htsp"); // server-side HTSP stream profile

  // Set before the reply arrives: subscriptionStart may beat it.
  subscription_id = sid;

  htsmsg_t* reply = SendRequest(msg, kRequestTimeoutMs, error);
  if (!reply)
    {
      subscription_id = 0;
      return false;
    }
  const std::string reply_error = ReplyError(reply);
  htsmsg_destroy(reply);
  if (!reply_error.empty())
    {
      subscription_id = 0;
      error = reply_error;
      return false;
    }
  return true;
}

void HtspClient::Unsubscribe()
{
  const uint32_t sid = subscription_id.exchange(0);
  if (!sid || !running)
    return;

  htsmsg_t* msg = htsmsg_create_map();
  htsmsg_add_str(msg, "method", "unsubscribe");
  htsmsg_add_u32(msg, "subscriptionId", sid);
  std::string error;
  if (htsmsg_t* reply = SendRequest(msg, kRequestTimeoutMs, error))
    htsmsg_destroy(reply);
}


// ---------------------------------------------------------------------------
// DVR storage
// ---------------------------------------------------------------------------

void HtspClient::UpsertDvrEntryLocked(htsmsg_t* msg)
{
  uint32_t id = 0;
  if (htsmsg_get_u32(msg, "id", &id))
    return;

  // dvrEntryUpdate only carries the fields that changed, so start from the
  // previous version of the entry.
  HtspDvrEntry& e = dvr[id];
  const uint32_t old_event = e.event_id;
  e.id = id;

  uint32_t u32 = 0;
  int64_t s64 = 0;
  if (!htsmsg_get_u32(msg, "channel", &u32))
    e.channel_id = u32;
  if (!htsmsg_get_u32(msg, "eventId", &u32))
    e.event_id = u32;
  if (!htsmsg_get_s64(msg, "start", &s64))
    e.start = s64;
  if (!htsmsg_get_s64(msg, "stop", &s64))
    e.stop = s64;
  if (!htsmsg_get_s64(msg, "dataSize", &s64))
    e.data_size = s64;
  if (!htsmsg_get_u32(msg, "seasonNumber", &u32))
    e.season = u32;
  if (!htsmsg_get_u32(msg, "episodeNumber", &u32))
    e.episode = u32;
  if (!htsmsg_get_u32(msg, "enabled", &u32))
    e.enabled = u32 != 0;
  if (const char* s = htsmsg_get_str(msg, "title"))
    e.title = s;
  if (const char* s = htsmsg_get_str(msg, "subtitle"))
    e.subtitle = s;
  if (const char* s = htsmsg_get_str(msg, "summary"))
    e.summary = s;
  if (const char* s = htsmsg_get_str(msg, "description"))
    e.description = s;
  if (const char* s = htsmsg_get_str(msg, "channelName"))
    e.channel_name = s;
  if (const char* s = htsmsg_get_str(msg, "state"))
    e.state = s;
  if (const char* s = htsmsg_get_str(msg, "error"))
    e.error = s;

  // Keep the event -> recording index in sync (the association can change,
  // e.g. when a scheduled entry loses its EPG event).
  if (old_event && old_event != e.event_id)
    dvr_by_event.erase(old_event);
  if (e.event_id)
    dvr_by_event[e.event_id] = id;
}

void HtspClient::RemoveDvrEntryLocked(uint32_t dvr_id)
{
  auto it = dvr.find(dvr_id);
  if (it == dvr.end())
    return;
  if (it->second.event_id)
    dvr_by_event.erase(it->second.event_id);
  dvr.erase(it);
}

std::vector<HtspDvrEntry> HtspClient::GetDvrEntries() const
{
  std::lock_guard<std::mutex> lock(channels_mutex);
  std::vector<HtspDvrEntry> out;
  out.reserve(dvr.size());
  for (const auto& [id, e] : dvr)
    out.push_back(e);

  // Pending recordings first (soonest first), then everything else with the
  // most recent at the top - which is the order a "Recordings" list wants.
  std::sort(out.begin(), out.end(), [](const HtspDvrEntry& a, const HtspDvrEntry& b) {
    const bool ap = a.IsPending(), bp = b.IsPending();
    if (ap != bp)
      return ap;
    if (ap)
      return a.start < b.start;
    if (a.start != b.start)
      return a.start > b.start;
    return a.id > b.id;
  });
  return out;
}

bool HtspClient::GetDvrEntryForEvent(uint32_t event_id, HtspDvrEntry* out) const
{
  if (!event_id)
    return false;
  std::lock_guard<std::mutex> lock(channels_mutex);
  auto idx = dvr_by_event.find(event_id);
  if (idx == dvr_by_event.end())
    return false;
  auto it = dvr.find(idx->second);
  if (it == dvr.end())
    return false;
  if (out)
    *out = it->second;
  return true;
}

bool HtspClient::GetDvrEntry(uint32_t dvr_id, HtspDvrEntry* out) const
{
  std::lock_guard<std::mutex> lock(channels_mutex);
  auto it = dvr.find(dvr_id);
  if (it == dvr.end())
    return false;
  if (out)
    *out = it->second;
  return true;
}

// ---------------------------------------------------------------------------
// DVR requests
// ---------------------------------------------------------------------------

bool HtspClient::SimpleRequest(htsmsg_t* msg, std::string& error, uint32_t* out_id)
{
  htsmsg_t* reply = SendRequest(msg, kRequestTimeoutMs, error);
  if (!reply)
    return false;

  // addDvrEntry and friends answer with success=0 plus an error string
  // rather than the generic "error" field, so check both.
  uint32_t success = 1;
  htsmsg_get_u32(reply, "success", &success);
  std::string err = ReplyError(reply);
  if (out_id)
    {
      uint32_t id = 0;
      if (!htsmsg_get_u32(reply, "id", &id))
	*out_id = id;
    }
  htsmsg_destroy(reply);

  if (!success)
    {
      error = err.empty() ? "the server rejected the request" : err;
      return false;
    }
  if (!err.empty())
    {
      error = err;
      return false;
    }
  return true;
}

bool HtspClient::RecordEvent(uint32_t event_id, std::string& error, uint32_t* out_id)
{
  htsmsg_t* msg = htsmsg_create_map();
  htsmsg_add_str(msg, "method", "addDvrEntry");
  htsmsg_add_u32(msg, "eventId", event_id);
  return SimpleRequest(msg, error, out_id);
}

bool HtspClient::CancelDvrEntry(uint32_t dvr_id, std::string& error)
{
  htsmsg_t* msg = htsmsg_create_map();
  htsmsg_add_str(msg, "method", "cancelDvrEntry");
  htsmsg_add_u32(msg, "id", dvr_id);
  return SimpleRequest(msg, error);
}

bool HtspClient::DeleteDvrEntry(uint32_t dvr_id, std::string& error)
{
  htsmsg_t* msg = htsmsg_create_map();
  htsmsg_add_str(msg, "method", "deleteDvrEntry");
  htsmsg_add_u32(msg, "id", dvr_id);
  return SimpleRequest(msg, error);
}

// ---------------------------------------------------------------------------
// File access (recording playback)
// ---------------------------------------------------------------------------

bool HtspClient::FileOpen(const std::string& path, uint32_t* handle, int64_t* size, std::string& error)
{
  if (!SupportsFileApi())
    {
      error = "the server's HTSP version does not support file access";
      return false;
    }

  htsmsg_t* msg = htsmsg_create_map();
  htsmsg_add_str(msg, "method", "fileOpen");
  htsmsg_add_str(msg, "file", path.c_str());

  htsmsg_t* reply = SendRequest(msg, kFileRequestTimeoutMs, error);
  if (!reply)
    return false;

  const std::string err = ReplyError(reply);
  uint32_t id = 0;
  const bool have_id = htsmsg_get_u32(reply, "id", &id) == 0;
  int64_t s64 = 0;
  const bool have_size = htsmsg_get_s64(reply, "size", &s64) == 0;
  htsmsg_destroy(reply);

  if (!err.empty() || !have_id)
    {
      error = err.empty() ? "the server did not return a file handle" : err;
      return false;
    }
  if (handle)
    *handle = id;
  if (size)
    *size = have_size ? s64 : -1;
  return true;
}

int64_t HtspClient::FileRead(uint32_t handle, void* buf, int64_t size, int64_t offset, std::string& error)
{
  if (size <= 0)
    return 0;

  htsmsg_t* msg = htsmsg_create_map();
  htsmsg_add_str(msg, "method", "fileRead");
  htsmsg_add_u32(msg, "id", handle);
  htsmsg_add_s64(msg, "size", size);
  if (offset >= 0)
    htsmsg_add_s64(msg, "offset", offset);

  htsmsg_t* reply = SendRequest(msg, kFileRequestTimeoutMs, error);
  if (!reply)
    return -1;

  const std::string err = ReplyError(reply);
  int64_t got = -1;
  const void* bin = nullptr;
  size_t bin_len = 0;
  if (err.empty())
    {
      if (!htsmsg_get_bin(reply, "data", &bin, &bin_len))
	{
	  // The server may return less than requested; never more.
	  got = (int64_t)bin_len;
	  if (got > size)
	    got = size;
	  if (got > 0)
	    std::memcpy(buf, bin, (size_t)got);
	}
      else
	{
	  got = 0; // no data field: treat as end of file
	}
    }
  htsmsg_destroy(reply);

  if (!err.empty())
    {
      error = err;
      return -1;
    }
  return got;
}

bool HtspClient::FileStat(uint32_t handle, int64_t* size, std::string& error)
{
  htsmsg_t* msg = htsmsg_create_map();
  htsmsg_add_str(msg, "method", "fileStat");
  htsmsg_add_u32(msg, "id", handle);

  htsmsg_t* reply = SendRequest(msg, kFileRequestTimeoutMs, error);
  if (!reply)
    return false;

  const std::string err = ReplyError(reply);
  int64_t s64 = 0;
  const bool have_size = htsmsg_get_s64(reply, "size", &s64) == 0;
  htsmsg_destroy(reply);

  if (!err.empty())
    {
      error = err;
      return false;
    }
  if (size)
    *size = have_size ? s64 : -1;
  return true;
}

void HtspClient::FileClose(uint32_t handle)
{
  if (!running)
    return;
  htsmsg_t* msg = htsmsg_create_map();
  htsmsg_add_str(msg, "method", "fileClose");
  htsmsg_add_u32(msg, "id", handle);
  std::string error;
  if (htsmsg_t* reply = SendRequest(msg, kRequestTimeoutMs, error))
    htsmsg_destroy(reply);
}

void HtspClient::Disconnect()
{
  running = false;
  if (reader.joinable())
    reader.join();
  if (sock >= 0)
    {
      htsp_tcp_close(sock);
      sock = -1;
    }
}
