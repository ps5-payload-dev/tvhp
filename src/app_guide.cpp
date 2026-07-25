// Guide view: turns the client's channel and EPG snapshots into the rows and
// detail text bound to the data model.
#include <algorithm>
#include <ctime>
#include <sstream>

#include "app.h"
#include "app_internal.h"
#include "format.h"

using namespace appdetail;
using namespace textfmt;

void App::RebuildChannelRows()
{
  channels_ = client_.GetChannels();
  channel_count_ = (int)channels_.size();

  // Keep the same channel selected across rebuilds if possible.
  if (selected_channel_id_)
    {
      for (size_t i = 0; i < channels_.size(); ++i)
	if (channels_[i].id == selected_channel_id_)
	  {
	    sel_channel_ = (int)i;
	    break;
	  }
    }
  sel_channel_ = std::clamp(sel_channel_, 0, std::max(0, channel_count_ - 1));
  selected_channel_id_ = channels_.empty() ? 0 : channels_[sel_channel_].id;

  const time_t now = time(nullptr);
  channel_rows_.clear();
  channel_rows_.reserve(channels_.size());
  for (const HtspChannel& ch : channels_)
    {
      ChannelRow row;
      row.number = ch.number ? std::to_string(ch.number) : "-";
      row.name = ch.name.empty() ? ("Channel " + std::to_string(ch.id)) : ch.name;

      const HtspClient::NowNext nn = client_.GetNowNext(ch.id);
      if (nn.has_now)
	{
	  row.has_now = true;
	  row.now_title = nn.now.title.empty() ? "Untitled programme" : nn.now.title;
	  row.now_time = FmtRange(nn.now.start, nn.now.stop);
	  int pct = 0;
	  if (nn.now.stop > nn.now.start)
	    pct = (int)(100 * (now - nn.now.start) / (nn.now.stop - nn.now.start));
	  row.progress_style = std::to_string(std::clamp(pct, 0, 100)) + "%";
	}
      else
	{
	  row.now_title = "No programme information";
	  row.progress_style = "0%";
	}
      channel_rows_.push_back(std::move(row));
    }

  model_.DirtyVariable("channels");
  model_.DirtyVariable("channel_count");
  model_.DirtyVariable("sel_channel");
}

void App::RebuildEpgRows()
{
  epg_rows_.clear();
  epg_events_.clear();
  bind_epg_channel_.clear();

  if (sel_channel_ >= 0 && sel_channel_ < (int)channels_.size())
    {
      const HtspChannel& ch = channels_[sel_channel_];
      selected_channel_id_ = ch.id;
      bind_epg_channel_ = ch.name;
      epg_events_ = client_.GetChannelEvents(ch.id, kMaxEpgRows);
    }

  const time_t now = time(nullptr);
  std::string last_day;
  for (const HtspEvent& ev : epg_events_)
    {
      EpgRow row;
      row.time = FmtRange(ev.start, ev.stop);
      row.title = ev.title.empty() ? "Untitled programme" : ev.title;
      row.now = (ev.start <= now && now < ev.stop);

      // Recording marker, so the guide shows at a glance what is set to record.
      HtspDvrEntry entry;
      if (client_.GetDvrEntryForEvent(ev.id, &entry) && entry.IsPending())
	{
	  row.recording = true;
	  row.rec = entry.IsRecording() ? "REC" : "SCHEDULED";
	}

      if (!row.now)
	{
	  // The airing row shows only its "Now" badge; the day label first
	  // appears on the next event (e.g. "Today" if it's the same day).
	  std::string day = FmtDay(ev.start);
	  if (day != last_day)
	    {
	      row.day = day; // only shown when the day changes
	      last_day = day;
	    }
	}
      epg_rows_.push_back(std::move(row));
    }

  sel_epg_ = std::clamp(sel_epg_, 0, std::max(0, (int)epg_rows_.size() - 1));

  model_.DirtyVariable("epg");
  model_.DirtyVariable("epg_channel");
  model_.DirtyVariable("sel_epg");
}

void App::RebuildDetail()
{
  bind_detail_title_.clear();
  bind_detail_meta_.clear();
  bind_detail_desc_.clear();
  bind_detail_rec_.clear();

  const HtspEvent* ev = nullptr;
  if (zone_ == Zone::Epg && sel_epg_ >= 0 && sel_epg_ < (int)epg_events_.size())
    ev = &epg_events_[sel_epg_];
  else if (!epg_events_.empty())
    ev = &epg_events_[0]; // channel zone: show what's airing / up next

  if (ev)
    {
      bind_detail_title_ = ev->title.empty() ? "Untitled programme" : ev->title;

      std::ostringstream meta;
      meta << FmtDay(ev->start) << "  ·  " << FmtRange(ev->start, ev->stop)
	   << "  ·  " << FmtDuration(ev->start, ev->stop);
      if (ev->season || ev->episode)
	{
	  meta << "  ·  ";
	  if (ev->season)
	    meta << "S" << ev->season;
	  if (ev->episode)
	    meta << "E" << ev->episode;
	}
      bind_detail_meta_ = meta.str();

      if (!ev->subtitle.empty() && ev->subtitle != ev->title)
	bind_detail_desc_ = ev->subtitle + "\n";
      if (!ev->description.empty())
	bind_detail_desc_ += ev->description;
      else if (!ev->summary.empty())
	bind_detail_desc_ += ev->summary;
      if (bind_detail_desc_.empty())
	bind_detail_desc_ = "No description available.";

      HtspDvrEntry entry;
      if (client_.GetDvrEntryForEvent(ev->id, &entry))
	{
	  if (entry.IsRecording())
	    bind_detail_rec_ = "Recording now";
	  else if (entry.IsScheduled())
	    bind_detail_rec_ = "Recording scheduled";
	  else if (entry.IsFinished())
	    bind_detail_rec_ = "Recorded";
	}
    }
  else if (view_ == View::Main)
    {
      bind_detail_title_ = client_.SyncCompleted() ? "No guide data" : "Loading guide...";
      bind_detail_desc_ = client_.SyncCompleted()
	? "This channel has no programme information."
	: "Fetching channels and programme guide from the server.";
    }

  model_.DirtyVariable("detail_title");
  model_.DirtyVariable("detail_meta");
  model_.DirtyVariable("detail_desc");
  model_.DirtyVariable("detail_rec");
}
