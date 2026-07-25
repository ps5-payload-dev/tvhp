#pragma once

#include <atomic>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include <RmlUi/Core.h>

#include "config.h"
#include "htsp.h"
#include "player.h"

// Owns the HTSP client, the player and the RmlUi document, and mediates
// between them. All RmlUi access happens on the main thread; changes coming
// from the HTSP reader thread are picked up by polling client.Generation()
// once per frame in Update().
class App : public Rml::EventListener {
public:
  App();
  ~App() override;

  // Creates the data model and loads assets/main.rml. Must be called before
  // the first context update.
  bool Initialize(Rml::Context* context, std::string& error);
  void Shutdown();

  // Per-frame housekeeping; call before context->Update().
  void Update();

  // Hook for the player to draw video beneath the UI; call between
  // Backend::BeginFrame() and context->Render().
  void RenderVideo(int width, int height);

  // Rml::EventListener (document-level keydown, capture phase).
  void ProcessEvent(Rml::Event& event) override;

private:
  enum class View { Connect, Main };
  // The main view shows either the channel guide or the recordings library;
  // Square toggles between them.
  enum class Section { Channels, Recordings };
  enum class Zone {
    ServerList, // connect view: saved servers
    ServerForm, // connect view: the edit fields
    Channels,   // main view: channel list
    Epg,        // main view: programme guide
    Recordings, // main view: recordings list
  };

  // Rows exposed to the RmlUi data model.
  struct ServerRow {
    Rml::String name;
    Rml::String detail;
  };
  struct ChannelRow {
    Rml::String number;
    Rml::String name;
    Rml::String now_title;
    Rml::String now_time;
    Rml::String progress_style; // "42%", bound via data-style-width
    bool has_now = false;
  };
  struct EpgRow {
    Rml::String time;  // "20:00 - 21:30"
    Rml::String day;   // "Today", "Tomorrow", "Wed 15 Jul"
    Rml::String title;
    Rml::String rec;   // "REC", "SCHEDULED" or empty
    bool now = false;  // currently airing
    bool recording = false;
  };
  struct RecRow {
    Rml::String title;
    Rml::String meta;    // "SVT1  -  Today 20:00"
    Rml::String state;   // "Recording", "Scheduled", "Failed", duration...
    bool pending = false;
    bool failed = false;
  };
  bool SetupDataModel(Rml::Context* context, std::string& error);

  // --- Connect screen ----------------------------------------------------
  void RebuildServerRows();
  void LoadFormFromSelection();  // saved server -> dialog fields
  // The add/edit dialog is modal over the server list. 'add_new' starts from
  // a blank profile; otherwise the highlighted server is loaded for editing.
  void OpenServerDialog(bool add_new);
  void CloseServerDialog();
  ServerProfile FormToProfile() const;
  // Commits the edit fields to the profile list and closes the dialog.
  // Connecting is a separate step, done from the server list.
  void SaveServer();
  void StartConnect();
  void PollConnectState();
  void DeleteSelectedServer();
  void SetConnectZone(Zone zone);
  int CurrentFormRow(bool* in_input) const;
  void FocusFormField(int delta);
  bool FocusFormInput();  // right: label -> input
  bool FocusFormLabel();  // left: input -> label (only at caret start)
  void HandleKeyConnect(Rml::Event& event, int key);

  // --- Main screen -------------------------------------------------------
  void SwitchToMain();
  void SwitchToConnect(const std::string& status);
  // Drops the HTSP connection and goes back to the server list, so another
  // server can be picked.
  void DisconnectToLogin();
  void SetSection(Section section);
  void RefreshFromClient(bool force);
  void RebuildChannelRows();
  void RebuildEpgRows();
  void RebuildDetail();
  void RebuildRecordingRows();
  void RebuildRecordingDetail();
  void MoveSelection(int delta);
  void SetZone(Zone zone);
  void ActivateSelection();
  void PlayChannel(uint32_t channel_id);
  void StopPlayback();

  // --- Recording (DVR) ---------------------------------------------------
  void ToggleRecordSelectedEvent();
  void PlaySelectedRecording();
  // Triangle: cancels a pending recording or deletes a finished one.
  void RemoveSelectedRecording();
  void CancelOrDeleteRecording(const HtspDvrEntry& entry, bool destructive);
  const HtspDvrEntry* SelectedRecording() const;

  // --- Full-screen playback ("watching") ---------------------------------
  void EnterWatch();
  void ExitWatch();
  void ShowWatchInfo(double seconds);
  void UpdateWatchOverlay();
  void EnsureRowVisible(const char* list_id, int index, float row_pitch);
  void ShowToast(const std::string& text);

  void HandleKeyMain(Rml::Event& event, int key);
  void HandleKeyWatch(Rml::Event& event, int key);

  // Data model bound state (main thread only).
  Rml::DataModelHandle model_;
  Rml::String bind_view_ = "connect";
  Rml::String bind_section_ = "channels";
  Rml::String bind_zone_ = "channels";
  Rml::String bind_connect_zone_ = "list";
  Rml::String bind_dialog_;        // "", "add" or "edit"
  Rml::String bind_dialog_title_;
  Rml::String bind_status_;       // connect screen status line
  Rml::String bind_server_;       // topbar server info
  Rml::String bind_clock_;        // topbar clock
  Rml::String bind_toast_;
  Rml::String bind_epg_channel_;  // heading of the guide pane
  Rml::String bind_detail_title_;
  Rml::String bind_detail_meta_;
  Rml::String bind_detail_desc_;
  Rml::String bind_detail_rec_;   // "Recording scheduled" banner
  Rml::String bind_player_status_;
  bool bind_watching_ = false;     // full-screen video, UI chrome hidden
  bool bind_info_visible_ = false; // watch info bar shown (auto-hides)
  Rml::String bind_watch_num_;
  Rml::String bind_watch_name_;
  Rml::String bind_watch_now_;
  Rml::String bind_watch_time_;
  Rml::String bind_watch_progress_ = "0%"; // data-style-width; never empty
  bool bind_watch_recorded_ = false;       // playing a recording, not live
  bool bind_watch_paused_ = false;

  // Connect screen bindings.
  std::vector<ServerRow> server_rows_;
  int sel_server_ = 0;
  int server_count_ = 0;
  Rml::String cfg_name_;
  Rml::String cfg_host_;
  Rml::String cfg_port_ = "9982";
  Rml::String cfg_user_;
  Rml::String cfg_pass_;

  // Main screen bindings.
  std::vector<ChannelRow> channel_rows_;
  std::vector<EpgRow> epg_rows_;
  std::vector<RecRow> rec_rows_;
  int sel_channel_ = 0;
  int sel_epg_ = 0;
  int sel_rec_ = 0;
  int channel_count_ = 0;
  int rec_count_ = 0;
  bool connecting_ = false;

  // Backing data (main thread copies of client state).
  std::vector<HtspChannel> channels_;
  std::vector<HtspEvent> epg_events_;
  std::vector<HtspDvrEntry> recordings_;
  uint32_t selected_channel_id_ = 0;
  uint32_t selected_dvr_id_ = 0;

  ServerConfig config_;
  HtspClient client_;
  std::unique_ptr<Player> player_;
  Rml::Context* context_ = nullptr;
  Rml::ElementDocument* document_ = nullptr;

  View view_ = View::Connect;
  Section section_ = Section::Channels;
  Zone zone_ = Zone::ServerList;
  uint32_t playing_channel_id_ = 0;
  uint32_t playing_dvr_id_ = 0;

  // Connect worker.
  enum class ConnectState { Idle, Busy, Success, Failed };
  std::thread connect_thread_;
  std::atomic<ConnectState> connect_state_{ConnectState::Idle};
  std::string connect_error_; // only touched by worker until state != Busy
  int connecting_index_ = -1; // profile being connected to

  uint64_t seen_generation_ = 0;
  double last_refresh_time_ = 0.0;
  double toast_deadline_ = 0.0;
  double info_deadline_ = 0.0; // watch info bar auto-hide
  double confirm_deadline_ = 0.0; // double-press-to-delete window
  uint32_t confirm_dvr_id_ = 0;
  int confirm_server_index_ = -1;
  bool scroll_channels_pending_ = false;
  bool scroll_epg_pending_ = false;
  bool scroll_rec_pending_ = false;
  bool scroll_server_pending_ = false;
};
