// Application shell: owns the RmlUi document and data model, drives the
// per-frame update, switches between views, and routes input. The views
// themselves live in app_connect.cpp, app_guide.cpp, app_recordings.cpp and
// app_watch.cpp.
#include <algorithm>
#include <cstdio>
#include <ctime>

#include "app.h"
#include "app_internal.h"
#include "keymap.h"

using namespace appdetail;


App::App() : player_(std::make_unique<Player>()) {}

App::~App() = default;

bool App::Initialize(Rml::Context* context, std::string& error) {
  config_.Load(kConfigPath);
  sel_server_ = config_.LastUsed();

  if (!SetupDataModel(context, error))
    return false;

  if (!(document_ = context->LoadDocument("assets/main.rml"))) {
    error = "failed to load assets/main.rml";
    return false;
  }

  document_->Show();
  document_->AddEventListener(Rml::EventId::Keydown, this, true);

  if (!player_->Initialize(error))
    return false;

  RebuildServerRows();
  LoadFormFromSelection();

  SetConnectZone(Zone::ServerList);
  // First run: there is nothing to select, so go straight to adding a server.
  if (config_.Empty())
    OpenServerDialog(true);

  return true;
}

void App::Shutdown() {
  if (connect_thread_.joinable())
    connect_thread_.join();

  if (player_) {
    player_->Stop(client_);
    player_->Shutdown();
  }

  client_.Disconnect();

  if (document_) {
    document_->RemoveEventListener(Rml::EventId::Keydown, this, true);
    document_->Close();
    document_ = nullptr;
  }
}

bool App::SetupDataModel(Rml::Context* context, std::string& error) {
  Rml::DataModelConstructor ctor = context->CreateDataModel("tv");
  if (!ctor) {
    error = "failed to create data model";
    return false;
  }

  if (auto row = ctor.RegisterStruct<ServerRow>()) {
    row.RegisterMember("name", &ServerRow::name);
    row.RegisterMember("detail", &ServerRow::detail);
  }
  ctor.RegisterArray<std::vector<ServerRow>>();

  if (auto row = ctor.RegisterStruct<ChannelRow>()) {
    row.RegisterMember("number", &ChannelRow::number);
    row.RegisterMember("name", &ChannelRow::name);
    row.RegisterMember("now_title", &ChannelRow::now_title);
    row.RegisterMember("now_time", &ChannelRow::now_time);
    row.RegisterMember("progress_style", &ChannelRow::progress_style);
    row.RegisterMember("has_now", &ChannelRow::has_now);
  }
  ctor.RegisterArray<std::vector<ChannelRow>>();

  if (auto row = ctor.RegisterStruct<EpgRow>()) {
    row.RegisterMember("time", &EpgRow::time);
    row.RegisterMember("day", &EpgRow::day);
    row.RegisterMember("title", &EpgRow::title);
    row.RegisterMember("rec", &EpgRow::rec);
    row.RegisterMember("now", &EpgRow::now);
    row.RegisterMember("recording", &EpgRow::recording);
  }
  ctor.RegisterArray<std::vector<EpgRow>>();

  if (auto row = ctor.RegisterStruct<RecRow>()) {
    row.RegisterMember("title", &RecRow::title);
    row.RegisterMember("meta", &RecRow::meta);
    row.RegisterMember("state", &RecRow::state);
    row.RegisterMember("pending", &RecRow::pending);
    row.RegisterMember("failed", &RecRow::failed);
  }
  ctor.RegisterArray<std::vector<RecRow>>();

  ctor.Bind("view", &bind_view_);
  ctor.Bind("section", &bind_section_);
  ctor.Bind("zone", &bind_zone_);
  ctor.Bind("dialog", &bind_dialog_);
  ctor.Bind("dialog_title", &bind_dialog_title_);
  ctor.Bind("status", &bind_status_);
  ctor.Bind("server", &bind_server_);
  ctor.Bind("clock", &bind_clock_);
  ctor.Bind("toast", &bind_toast_);
  ctor.Bind("servers", &server_rows_);
  ctor.Bind("sel_server", &sel_server_);
  ctor.Bind("server_count", &server_count_);
  ctor.Bind("name", &cfg_name_);
  ctor.Bind("host", &cfg_host_);
  ctor.Bind("port", &cfg_port_);
  ctor.Bind("user", &cfg_user_);
  ctor.Bind("pass", &cfg_pass_);
  ctor.Bind("channels", &channel_rows_);
  ctor.Bind("epg", &epg_rows_);
  ctor.Bind("recordings", &rec_rows_);
  ctor.Bind("sel_channel", &sel_channel_);
  ctor.Bind("sel_epg", &sel_epg_);
  ctor.Bind("sel_rec", &sel_rec_);
  ctor.Bind("channel_count", &channel_count_);
  ctor.Bind("rec_count", &rec_count_);
  ctor.Bind("epg_channel", &bind_epg_channel_);
  ctor.Bind("detail_title", &bind_detail_title_);
  ctor.Bind("detail_meta", &bind_detail_meta_);
  ctor.Bind("detail_desc", &bind_detail_desc_);
  ctor.Bind("detail_rec", &bind_detail_rec_);
  ctor.Bind("player_status", &bind_player_status_);
  ctor.Bind("watching", &bind_watching_);
  ctor.Bind("info_visible", &bind_info_visible_);
  ctor.Bind("watch_num", &bind_watch_num_);
  ctor.Bind("watch_name", &bind_watch_name_);
  ctor.Bind("watch_now", &bind_watch_now_);
  ctor.Bind("watch_time", &bind_watch_time_);
  ctor.Bind("watch_progress", &bind_watch_progress_);
  ctor.Bind("watch_recorded", &bind_watch_recorded_);

  ctor.BindEventCallback("save", [this](Rml::DataModelHandle, Rml::Event&, const Rml::VariantList&) {
    SaveServer();
  });
  ctor.BindEventCallback("cancel", [this](Rml::DataModelHandle, Rml::Event&, const Rml::VariantList&) {
    CloseServerDialog();
  });
  ctor.BindEventCallback("select_server", [this](Rml::DataModelHandle, Rml::Event&, const Rml::VariantList& args) {
    if (args.empty())
      return;
    const int index = args[0].Get<int>();
    SetConnectZone(Zone::ServerList);
    if (index == sel_server_) {
      StartConnect(); // second click on the highlighted row connects
      return;
    }
    sel_server_ = index;
    scroll_server_pending_ = true;
    LoadFormFromSelection();
    model_.DirtyVariable("sel_server");
  });
  ctor.BindEventCallback("select_channel", [this](Rml::DataModelHandle, Rml::Event&, const Rml::VariantList& args) {
    if (args.empty())
      return;
    const int index = args[0].Get<int>();
    SetZone(Zone::Channels);
    if (index == sel_channel_)
      {
	ActivateSelection(); // second click on the selected row = watch
	return;
      }
    sel_channel_ = index;
    scroll_channels_pending_ = true;
    RebuildEpgRows();
    RebuildDetail();
    model_.DirtyVariable("sel_channel");
  });
  ctor.BindEventCallback("select_epg", [this](Rml::DataModelHandle, Rml::Event&, const Rml::VariantList& args) {
    if (args.empty())
      return;
    SetZone(Zone::Epg);
    sel_epg_ = args[0].Get<int>();
    scroll_epg_pending_ = true;
    RebuildDetail();
    model_.DirtyVariable("sel_epg");
  });
  ctor.BindEventCallback("select_rec", [this](Rml::DataModelHandle, Rml::Event&, const Rml::VariantList& args) {
    if (args.empty())
      return;
    const int index = args[0].Get<int>();
    SetZone(Zone::Recordings);
    if (index == sel_rec_)
      {
	ActivateSelection();
	return;
      }
    sel_rec_ = index;
    scroll_rec_pending_ = true;
    RebuildRecordingDetail();
    model_.DirtyVariable("sel_rec");
  });
  model_ = ctor.GetModelHandle();
  return true;
}

// ---------------------------------------------------------------------------
// Connect screen: saved servers
// ---------------------------------------------------------------------------


// ---------------------------------------------------------------------------
// View switching
// ---------------------------------------------------------------------------

void App::SwitchToMain() {
  view_ = View::Main;
  bind_view_ = "main";
  bind_status_.clear();
  bind_server_ = client_.ServerInfo();
  section_ = Section::Channels;
  bind_section_ = "channels";
  zone_ = Zone::Channels;
  bind_zone_ = "channels";
  model_.DirtyVariable("view");
  model_.DirtyVariable("status");
  model_.DirtyVariable("server");
  model_.DirtyVariable("section");
  model_.DirtyVariable("zone");
  RefreshFromClient(true);
}

// Drops the connection so a different server can be chosen. Safe to call
// when already disconnected.
void App::DisconnectToLogin() {
  StopPlayback();
  client_.Disconnect();
  seen_generation_ = 0;
  channels_.clear();
  epg_events_.clear();
  recordings_.clear();
  channel_rows_.clear();
  epg_rows_.clear();
  rec_rows_.clear();
  selected_channel_id_ = 0;
  selected_dvr_id_ = 0;
  model_.DirtyVariable("channels");
  model_.DirtyVariable("epg");
  model_.DirtyVariable("recordings");
  SwitchToConnect({});
}

void App::SwitchToConnect(const std::string& status) {
  StopPlayback();
  view_ = View::Connect;
  bind_view_ = "connect";
  bind_status_ = status;
  model_.DirtyVariable("view");
  model_.DirtyVariable("status");
  RebuildServerRows();
  CloseServerDialog();
  SetConnectZone(Zone::ServerList);
}

void App::SetSection(Section section) {
  if (section_ == section)
    return;
  section_ = section;
  bind_section_ = (section == Section::Channels) ? "channels" : "recordings";
  zone_ = (section == Section::Channels) ? Zone::Channels : Zone::Recordings;
  bind_zone_ = (section == Section::Channels) ? "channels" : "recordings";
  model_.DirtyVariable("section");
  model_.DirtyVariable("zone");

  if (section == Section::Recordings) {
    RebuildRecordingRows();
    RebuildRecordingDetail();
    scroll_rec_pending_ = true;
  } else {
    RebuildEpgRows();
    RebuildDetail();
    scroll_channels_pending_ = true;
  }
}

void App::Update() {
  PollConnectState();

  // Clock (topbar).
  {
    const time_t now = time(nullptr);
    struct tm tm;
    localtime_r(&now, &tm);
    char buf[8] = {};
    std::snprintf(buf, sizeof(buf), "%02d:%02d", tm.tm_hour, tm.tm_min);
    if (bind_clock_ != buf) {
      bind_clock_ = buf;
      model_.DirtyVariable("clock");
    }
  }

  if (view_ == View::Main) {
    if (!client_.Connected()) {
      const ServerProfile* p = config_.At(config_.LastUsed());
      SwitchToConnect("Connection to " + (p ? p->host : std::string("the server")) + " lost");
      return;
    }
    RefreshFromClient(false);
  }

  // Watch info bar: refresh while visible (cheap; only dirties on change),
  // then auto-hide. The bar stays up while a recording is paused so the
  // transport controls remain visible.
  if (bind_watching_ && bind_info_visible_)
    {
      UpdateWatchOverlay();
      if (Now() > info_deadline_ && !(player_ && player_->IsPaused()))
	{
	  bind_info_visible_ = false;
	  model_.DirtyVariable("info_visible");
	}
    }

  // Toast expiry.
  if (!bind_toast_.empty() && Now() > toast_deadline_)
    {
      bind_toast_.clear();
      model_.DirtyVariable("toast");
    }

  // Deferred scrolling; uses layout data from the previous frame, which is
  // fine because row heights are fixed.
  if (scroll_server_pending_)
    {
      EnsureRowVisible("server-list", sel_server_, kServerRowPitch);
      scroll_server_pending_ = false;
    }
  if (scroll_channels_pending_)
    {
      EnsureRowVisible("channel-list", sel_channel_, kChannelRowPitch);
      scroll_channels_pending_ = false;
    }
  if (scroll_epg_pending_)
    {
      EnsureRowVisible("epg-list", sel_epg_, kEpgRowPitch);
      scroll_epg_pending_ = false;
    }
  if (scroll_rec_pending_)
    {
      EnsureRowVisible("rec-list", sel_rec_, kRecRowPitch);
      scroll_rec_pending_ = false;
    }
}

void App::RenderVideo(int width, int height)
{
  if (player_)
    player_->RenderVideo(width, height);
}

void App::RefreshFromClient(bool force)
{
  const uint64_t gen = client_.Generation();
  const double now = Now();
  if (!force && gen == seen_generation_ && now - last_refresh_time_ < kRefreshIntervalSec)
    return;
  seen_generation_ = gen;
  last_refresh_time_ = now;

  if (section_ == Section::Channels)
    {
      RebuildChannelRows();
      RebuildEpgRows();
      RebuildDetail();
    }
  else
    {
      RebuildRecordingRows();
      RebuildRecordingDetail();
    }

  const std::string player_status = player_ ? player_->StatusText() : std::string();
  if (bind_player_status_ != player_status)
    {
      bind_player_status_ = player_status;
      model_.DirtyVariable("player_status");
    }
}

// ---------------------------------------------------------------------------
// Model rebuilding: channels and guide
// ---------------------------------------------------------------------------


// ---------------------------------------------------------------------------
// Model rebuilding: recordings
// ---------------------------------------------------------------------------


// ---------------------------------------------------------------------------
// Navigation
// ---------------------------------------------------------------------------

void App::SetZone(Zone zone)
{
  if (zone_ == zone)
    return;
  zone_ = zone;
  switch (zone)
    {
    case Zone::Channels:   bind_zone_ = "channels"; break;
    case Zone::Epg:        bind_zone_ = "epg"; break;
    case Zone::Recordings: bind_zone_ = "recordings"; break;
    default:               bind_zone_ = "channels"; break;
    }
  model_.DirtyVariable("zone");

  if (zone == Zone::Channels || zone == Zone::Epg)
    RebuildDetail();
}

void App::MoveSelection(int delta)
{
  switch (zone_)
    {
    case Zone::ServerList:
      {
	if (server_rows_.empty())
	  return;
	const int next = std::clamp(sel_server_ + delta, 0, (int)server_rows_.size() - 1);
	if (next == sel_server_)
	  return;
	sel_server_ = next;
	scroll_server_pending_ = true;
	model_.DirtyVariable("sel_server");
	LoadFormFromSelection();
	break;
      }
    case Zone::Channels:
      {
	if (channel_rows_.empty())
	  return;
	const int next = std::clamp(sel_channel_ + delta, 0, (int)channel_rows_.size() - 1);
	if (next == sel_channel_)
	  return;
	sel_channel_ = next;
	sel_epg_ = 0;
	scroll_channels_pending_ = true;
	model_.DirtyVariable("sel_channel");
	RebuildEpgRows();
	RebuildDetail();
	break;
      }
    case Zone::Epg:
      {
	if (epg_rows_.empty())
	  return;
	const int next = std::clamp(sel_epg_ + delta, 0, (int)epg_rows_.size() - 1);
	if (next == sel_epg_)
	  return;
	sel_epg_ = next;
	scroll_epg_pending_ = true;
	model_.DirtyVariable("sel_epg");
	RebuildDetail();
	break;
      }
    case Zone::Recordings:
      {
	if (rec_rows_.empty())
	  return;
	const int next = std::clamp(sel_rec_ + delta, 0, (int)rec_rows_.size() - 1);
	if (next == sel_rec_)
	  return;
	sel_rec_ = next;
	scroll_rec_pending_ = true;
	model_.DirtyVariable("sel_rec");
	RebuildRecordingDetail();
	break;
      }
    default:
      break;
    }
}

void App::ActivateSelection()
{
  switch (zone_)
    {
    case Zone::Channels:
      if (sel_channel_ >= 0 && sel_channel_ < (int)channels_.size())
	PlayChannel(channels_[sel_channel_].id);
      break;
    case Zone::Epg:
      ToggleRecordSelectedEvent();
      break;
    case Zone::Recordings:
      PlaySelectedRecording();
      break;
    default:
      break;
    }
}


// ---------------------------------------------------------------------------
// Recording (DVR)
// ---------------------------------------------------------------------------


// ---------------------------------------------------------------------------
// Watch mode (full-screen video)
// ---------------------------------------------------------------------------


void App::EnsureRowVisible(const char* list_id, int index, float row_pitch)
{
  Rml::Element* list = document_ ? document_->GetElementById(list_id) : nullptr;
  if (!list || index < 0)
    return;

  const float view_h = list->GetClientHeight();
  if (view_h <= 0.0f)
    return;

  // Keep the selected row centered where possible.
  float target = index * row_pitch - (view_h - row_pitch) * 0.5f;
  const float max_scroll = std::max(0.0f, list->GetScrollHeight() - view_h);
  target = std::clamp(target, 0.0f, max_scroll);
  list->SetScrollTop(target);
}

void App::ShowToast(const std::string& text)
{
  bind_toast_ = text;
  toast_deadline_ = Now() + kToastSec;
  model_.DirtyVariable("toast");
}

// ---------------------------------------------------------------------------
// Input
// ---------------------------------------------------------------------------

void App::ProcessEvent(Rml::Event& event)
{
  if (event.GetId() != Rml::EventId::Keydown)
    return;

  const int key = event.GetParameter<int>("key_identifier", 0);

  if (key == Rml::Input::KI_F8)
    {
      event.StopPropagation();
      return;
    }

  if (view_ == View::Connect)
    HandleKeyConnect(event, key);
  else
    HandleKeyMain(event, key);
}


void App::HandleKeyMain(Rml::Event& event, int key)
{
  if (bind_watching_)
    {
      HandleKeyWatch(event, key);
      return;
    }

  switch (keymap::Command(key))
    {
    case keymap::Cmd::Up:
      MoveSelection(-1);
      break;
    case keymap::Cmd::Down:
      MoveSelection(+1);
      break;
    case keymap::Cmd::PageUp: // left shoulder / "p+"
      MoveSelection(-8);
      break;
    case keymap::Cmd::PageDown: // right shoulder / "p-"
      MoveSelection(+8);
      break;
    case keymap::Cmd::Home:
      MoveSelection(-1000000);
      break;
    case keymap::Cmd::End:
      MoveSelection(+1000000);
      break;
    case keymap::Cmd::Right:
      if (section_ == Section::Channels && !epg_rows_.empty())
	{
	  SetZone(Zone::Epg);
	  scroll_epg_pending_ = true;
	  model_.DirtyVariable("sel_epg");
	}
      break;
    case keymap::Cmd::Left:
      if (zone_ == Zone::Epg)
	SetZone(Zone::Channels);
      break;

    // Square switches between the guide and the recordings library.
    case keymap::Cmd::Secondary:
      SetSection(section_ == Section::Channels ? Section::Recordings : Section::Channels);
      break;

    // The remote has a dedicated Guide button, which always means "show me
    // the channels", and steps into the programme list if it is already there.
    case keymap::Cmd::Guide:
      if (section_ != Section::Channels)
	SetSection(Section::Channels);
      else if (zone_ == Zone::Channels && !epg_rows_.empty())
	{
	  SetZone(Zone::Epg);
	  scroll_epg_pending_ = true;
	  model_.DirtyVariable("sel_epg");
	}
      break;

    // Triangle removes the highlighted recording. It does nothing anywhere
    // else, so a stray press from the couch is harmless.
    case keymap::Cmd::Remove:
      if (section_ != Section::Recordings)
	return;
      RemoveSelectedRecording();
      break;

    case keymap::Cmd::Back:
      // Back steps up one level, and from the top of either section it
      // returns to the server list so another server can be picked.
      if (zone_ == Zone::Epg)
	SetZone(Zone::Channels);
      else if (player_ && player_->IsPlaying())
	{
	  StopPlayback();
	  ShowToast("Playback stopped");
	}
      else if (section_ == Section::Recordings)
	SetSection(Section::Channels);
      else
	DisconnectToLogin();
      break;

    // Stop only ever means stop; it never navigates.
    case keymap::Cmd::Stop:
      if (!player_ || !player_->IsPlaying())
	return;
      StopPlayback();
      ShowToast("Playback stopped");
      break;

    // Play starts whatever is highlighted, but only where that is what the
    // cross button would do: in the guide, cross schedules a recording, and
    // the play button should not.
    case keymap::Cmd::PlayPause:
      if (zone_ == Zone::Epg)
	return;
      ActivateSelection();
      break;

    case keymap::Cmd::Ok:
      ActivateSelection();
      break;

    default:
      return; // unhandled: let it propagate
    }
  event.StopPropagation();
}

