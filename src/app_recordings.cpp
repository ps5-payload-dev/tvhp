// Recordings view: the DVR list and detail, plus the scheduling, cancelling
// and deleting actions that act on the highlighted entry.
#include <algorithm>
#include <sstream>

#include "app.h"
#include "app_internal.h"
#include "format.h"

using namespace appdetail;
using namespace textfmt;

void App::RebuildRecordingRows()
{
  recordings_ = client_.GetDvrEntries();
  rec_count_ = (int)recordings_.size();

  // Keep the same recording selected across rebuilds if possible.
  if (selected_dvr_id_)
    {
      for (size_t i = 0; i < recordings_.size(); ++i)
	if (recordings_[i].id == selected_dvr_id_)
	  {
	    sel_rec_ = (int)i;
	    break;
	  }
    }
  sel_rec_ = std::clamp(sel_rec_, 0, std::max(0, rec_count_ - 1));
  selected_dvr_id_ = recordings_.empty() ? 0 : recordings_[sel_rec_].id;

  rec_rows_.clear();
  rec_rows_.reserve(recordings_.size());
  for (const HtspDvrEntry& e : recordings_)
    {
      RecRow row;
      row.title = e.title.empty() ? "Untitled recording" : e.title;

      std::ostringstream meta;
      if (!e.channel_name.empty())
	meta << e.channel_name << "  ·  ";
      meta << FmtDay(e.start) << " " << FmtHM(e.start);
      if (e.season || e.episode)
	{
	  meta << "  ·  ";
	  if (e.season)
	    meta << "S" << e.season;
	  if (e.episode)
	    meta << "E" << e.episode;
	}
      row.meta = meta.str();
      row.state = DvrStateLabel(e);
      row.pending = e.IsPending();
      row.failed = !e.error.empty() || e.state == "missed" || e.state == "invalid";
      rec_rows_.push_back(std::move(row));
    }

  model_.DirtyVariable("recordings");
  model_.DirtyVariable("rec_count");
  model_.DirtyVariable("sel_rec");
}

const HtspDvrEntry* App::SelectedRecording() const
{
  if (sel_rec_ < 0 || sel_rec_ >= (int)recordings_.size())
    return nullptr;
  return &recordings_[sel_rec_];
}

void App::RebuildRecordingDetail()
{
  bind_detail_title_.clear();
  bind_detail_meta_.clear();
  bind_detail_desc_.clear();
  bind_detail_rec_.clear();

  const HtspDvrEntry* e = SelectedRecording();
  if (!e)
    {
      bind_detail_title_ = client_.SyncCompleted() ? "No recordings" : "Loading recordings...";
      bind_detail_desc_ = client_.SyncCompleted()
	? "Recordings you schedule from the guide show up here."
	: "Fetching the recording list from the server.";
    }
  else
    {
      selected_dvr_id_ = e->id;
      bind_detail_title_ = e->title.empty() ? "Untitled recording" : e->title;

      std::ostringstream meta;
      if (!e->channel_name.empty())
	meta << e->channel_name << "  ·  ";
      meta << FmtDay(e->start) << "  ·  " << FmtRange(e->start, e->stop)
	   << "  ·  " << FmtDuration(e->start, e->stop);
      if (e->data_size > 0)
	meta << "  ·  " << (e->data_size / (1024 * 1024)) << " MB";
      bind_detail_meta_ = meta.str();

      if (!e->subtitle.empty() && e->subtitle != e->title)
	bind_detail_desc_ = e->subtitle + "\n";
      if (!e->description.empty())
	bind_detail_desc_ += e->description;
      else if (!e->summary.empty())
	bind_detail_desc_ += e->summary;
      if (bind_detail_desc_.empty())
	bind_detail_desc_ = "No description available.";

      bind_detail_rec_ = DvrStateLabel(*e);
    }

  model_.DirtyVariable("detail_title");
  model_.DirtyVariable("detail_meta");
  model_.DirtyVariable("detail_desc");
  model_.DirtyVariable("detail_rec");
}

void App::ToggleRecordSelectedEvent()
{
  if (sel_epg_ < 0 || sel_epg_ >= (int)epg_events_.size())
    return;
  const HtspEvent& ev = epg_events_[sel_epg_];

  HtspDvrEntry entry;
  const bool have = client_.GetDvrEntryForEvent(ev.id, &entry);

  if (have && entry.IsPending())
    {
      CancelOrDeleteRecording(entry, false);
      return;
    }
  if (have && entry.IsFinished())
    {
      ShowToast("Already recorded - see Recordings");
      return;
    }

  std::string error;
  if (!client_.RecordEvent(ev.id, error))
    {
      ShowToast(error.empty() ? "The server refused to schedule it" : error);
      return;
    }

  const std::string title = ev.title.empty() ? std::string("programme") : ev.title;
  ShowToast("Recording scheduled: " + title);
  // The dvrEntryAdd push will refresh the list, but update the badge now so
  // the press feels immediate.
  RefreshFromClient(true);
}

// destructive == true means deleteDvrEntry (removes the file); false means
// cancelDvrEntry (unschedules or stops, keeping whatever was recorded).
void App::CancelOrDeleteRecording(const HtspDvrEntry& entry, bool destructive)
{
  const double now = Now();
  if (!(confirm_dvr_id_ == entry.id && now < confirm_deadline_))
    {
      confirm_dvr_id_ = entry.id;
      confirm_deadline_ = now + kConfirmWindowSec;
      ShowToast(destructive ? "Press again to delete this recording"
			    : "Press again to cancel this recording");
      return;
    }
  confirm_dvr_id_ = 0;
  confirm_deadline_ = 0.0;

  std::string error;
  const bool ok = destructive ? client_.DeleteDvrEntry(entry.id, error)
			      : client_.CancelDvrEntry(entry.id, error);
  if (!ok)
    {
      ShowToast(error.empty() ? "The server refused the request" : error);
      return;
    }

  // If the entry we just removed is the one playing, stop first.
  if (playing_dvr_id_ == entry.id)
    StopPlayback();

  ShowToast(destructive ? "Recording deleted" : "Recording cancelled");
  RefreshFromClient(true);
}

void App::PlaySelectedRecording()
{
  const HtspDvrEntry* e = SelectedRecording();
  if (!e)
    return;
  if (!e->HasFile())
    {
      ShowToast(e->IsScheduled() ? "This recording hasn't started yet"
				 : "There is no file for this recording");
      return;
    }
  if (!client_.SupportsFileApi())
    {
      ShowToast("This server is too old to stream recordings over HTSP");
      return;
    }

  const uint32_t dvr_id = e->id;
  std::string error;
  if (!player_->PlayRecording(client_, dvr_id, error))
    {
      ShowToast(error.empty() ? "Unable to play this recording" : error);
      return;
    }
  playing_dvr_id_ = dvr_id;
  playing_channel_id_ = 0;
  EnterWatch();
}

// Triangle in the recordings list: cancels an upcoming or running recording,
// deletes a finished one. Both need a second press to confirm.
void App::RemoveSelectedRecording()
{
  const HtspDvrEntry* e = SelectedRecording();
  if (!e)
    return;
  CancelOrDeleteRecording(*e, !e->IsPending());
}
