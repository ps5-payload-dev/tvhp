// Watch view: full-screen playback of a channel or a recording, the
// auto-hiding info bar, and the transport controls.
#include <algorithm>
#include <ctime>

#include "app.h"
#include "app_internal.h"
#include "format.h"

using namespace appdetail;
using namespace textfmt;

void App::PlayChannel(uint32_t channel_id)
{
  std::string error;
  if (!player_->PlayChannel(client_, channel_id, error))
    {
      ShowToast(error.empty() ? "Unable to start playback" : error);
      return;
    }
  playing_channel_id_ = channel_id;
  playing_dvr_id_ = 0;
  EnterWatch();
}

void App::StopPlayback()
{
  if (player_)
    player_->Stop(client_);
  playing_channel_id_ = 0;
  playing_dvr_id_ = 0;
  ExitWatch();
  // Reflect the cleared status right away instead of waiting for the next
  // periodic refresh.
  const Rml::String status = player_ ? Rml::String(player_->StatusText()) : Rml::String();
  if (bind_player_status_ != status)
    {
      bind_player_status_ = status;
      model_.DirtyVariable("player_status");
    }
}

void App::EnterWatch()
{
  if (!bind_watching_)
    {
      bind_watching_ = true;
      model_.DirtyVariable("watching");
    }
  const bool recorded = player_ && player_->IsRecordingPlayback();
  if (bind_watch_recorded_ != recorded)
    {
      bind_watch_recorded_ = recorded;
      model_.DirtyVariable("watch_recorded");
    }
  UpdateWatchOverlay();
  ShowWatchInfo(kWatchInfoSec);
}

void App::ExitWatch()
{
  if (bind_watching_)
    {
      bind_watching_ = false;
      model_.DirtyVariable("watching");
    }
  if (bind_info_visible_)
    {
      bind_info_visible_ = false;
      model_.DirtyVariable("info_visible");
    }
  if (bind_watch_paused_)
    {
      bind_watch_paused_ = false;
      model_.DirtyVariable("watch_paused");
    }
}

void App::ShowWatchInfo(double seconds)
{
  info_deadline_ = Now() + seconds;
  if (!bind_info_visible_)
    {
      bind_info_visible_ = true;
      model_.DirtyVariable("info_visible");
    }
}

void App::UpdateWatchOverlay()
{
  Rml::String num, name, now_title, now_time, progress = "0%";

  if (player_ && player_->IsRecordingPlayback())
    {
      // Recording: the "channel number" slot becomes a play/pause marker and
      // the progress bar tracks the position in the file.
      HtspDvrEntry entry;
      if (client_.GetDvrEntry(playing_dvr_id_, &entry))
	{
	  name = entry.title.empty() ? "Recording" : entry.title;
	  now_title = entry.channel_name;
	  if (!entry.subtitle.empty() && entry.subtitle != entry.title)
	    now_title += (now_title.empty() ? "" : "  ·  ") + entry.subtitle;
	}
      else
	{
	  name = "Recording";
	}

      const int64_t pos = player_->PositionUs();
      const int64_t dur = player_->DurationUs();
      now_time = FmtPosition(pos) + (dur > 0 ? " / " + FmtPosition(dur) : "");
      if (dur > 0 && pos >= 0)
	{
	  const int pct = (int)std::clamp<int64_t>(pos * 100 / dur, 0, 100);
	  progress = std::to_string(pct) + "%";
	}
      num = player_->IsPaused() ? "II" : "";
    }
  else
    {
      const HtspChannel* ch = nullptr;
      for (const HtspChannel& c : channels_)
	if (c.id == playing_channel_id_)
	  {
	    ch = &c;
	    break;
	  }

      if (ch)
	{
	  num = std::to_string(ch->number);
	  name = ch->name;
	  const HtspClient::NowNext nn = client_.GetNowNext(ch->id);
	  if (nn.has_now)
	    {
	      now_title = nn.now.title;
	      now_time = FmtRange(nn.now.start, nn.now.stop);
	      const time_t now = time(nullptr);
	      if (nn.now.stop > nn.now.start)
		{
		  const int pct = (int)std::clamp<int64_t>(
		      (now - nn.now.start) * 100 / (nn.now.stop - nn.now.start), 0, 100);
		  progress = std::to_string(pct) + "%";
		}
	    }
	}
    }

  auto set = [&](Rml::String& bind, const Rml::String& value, const char* var) {
    if (bind != value)
      {
	bind = value;
	model_.DirtyVariable(var);
      }
  };
  set(bind_watch_num_, num, "watch_num");
  set(bind_watch_name_, name, "watch_name");
  set(bind_watch_now_, now_title, "watch_now");
  set(bind_watch_time_, now_time, "watch_time");
  set(bind_watch_progress_, progress, "watch_progress");
  set(bind_player_status_, player_ ? Rml::String(player_->StatusText()) : Rml::String(), "player_status");

  const bool paused = player_ && player_->IsPaused();
  if (bind_watch_paused_ != paused)
    {
      bind_watch_paused_ = paused;
      model_.DirtyVariable("watch_paused");
    }
}

void App::HandleKeyWatch(Rml::Event& event, int key)
{
  const bool recorded = player_ && player_->IsRecordingPlayback();

  if (recorded)
    {
      switch (key)
	{
	case Rml::Input::KI_LEFT:
	case Rml::Input::KI_RIGHT:
	case Rml::Input::KI_UP:
	case Rml::Input::KI_DOWN:
	case Rml::Input::KI_PRIOR:
	case Rml::Input::KI_NEXT:
	  {
	    // Left/right step by 30s; up/down and the shoulder buttons step by
	    // 5 minutes (up is forwards, as in most media players).
	    int64_t delta = 0;
	    if (key == Rml::Input::KI_LEFT)  delta = -kSeekSmallUs;
	    if (key == Rml::Input::KI_RIGHT) delta = +kSeekSmallUs;
	    if (key == Rml::Input::KI_UP)    delta = +kSeekLargeUs;
	    if (key == Rml::Input::KI_DOWN)  delta = -kSeekLargeUs;
	    if (key == Rml::Input::KI_PRIOR) delta = -kSeekLargeUs;
	    if (key == Rml::Input::KI_NEXT)  delta = +kSeekLargeUs;
	    if (player_->CanSeek())
	      {
		player_->SeekRelative(delta);
		UpdateWatchOverlay();
		ShowWatchInfo(kWatchInfoSec);
	      }
	    else
	      {
		ShowToast("This recording can't be seeked");
	      }
	    event.StopPropagation();
	    return;
	  }
	case Rml::Input::KI_RETURN:
	case Rml::Input::KI_NUMPADENTER: // cross: play / pause
	  player_->TogglePause();
	  UpdateWatchOverlay();
	  ShowWatchInfo(kWatchInfoSec);
	  event.StopPropagation();
	  return;
	case Rml::Input::KI_SPACE: // square: toggle the info bar
	  if (bind_info_visible_ && !player_->IsPaused())
	    {
	      bind_info_visible_ = false;
	      model_.DirtyVariable("info_visible");
	    }
	  else
	    ShowWatchInfo(kWatchInfoSec);
	  event.StopPropagation();
	  return;
	case Rml::Input::KI_BACK: // circle: back to the list
	  StopPlayback();
	  event.StopPropagation();
	  return;
	default:
	  return;
	}
    }

  switch (key)
    {
    case Rml::Input::KI_UP: // zap: previous channel in the list
    case Rml::Input::KI_DOWN:
    case Rml::Input::KI_PRIOR:
    case Rml::Input::KI_NEXT:
      {
	const int delta = (key == Rml::Input::KI_UP || key == Rml::Input::KI_PRIOR) ? +1 : -1;
	const int prev = sel_channel_;
	SetZone(Zone::Channels);
	MoveSelection(delta);
	if (sel_channel_ != prev && sel_channel_ >= 0 && sel_channel_ < (int)channels_.size())
	  PlayChannel(channels_[sel_channel_].id); // re-enters watch, shows info
	else
	  ShowWatchInfo(kWatchInfoSec);
	break;
      }
    case Rml::Input::KI_RETURN: // toggle the info bar
    case Rml::Input::KI_NUMPADENTER:
      if (bind_info_visible_)
	{
	  bind_info_visible_ = false;
	  model_.DirtyVariable("info_visible");
	}
      else
	ShowWatchInfo(kWatchInfoSec);
      break;
    case Rml::Input::KI_LEFT:
    case Rml::Input::KI_BACK: // back to the channel list, keep watching? No:
      StopPlayback();         // Back means back. OK from the list resumes.
      break;
    default:
      return;
    }
  event.StopPropagation();
}
