// Connect view: the list of saved servers and the modal add/edit dialog,
// including the connection worker thread and the form's focus handling.
#include <algorithm>
#include <string>

#include <RmlUi/Core/Elements/ElementFormControlInput.h>

#include "app.h"
#include "app_internal.h"
#include "keymap.h"

using namespace appdetail;

namespace {
  // Connect view form rows: label on the left, input on the right. The last
  // row is the Save/Cancel button pair: Save sits in the label slot and
  // Cancel in the input slot, so left/right moves between the two buttons.
  // Up/down walks the rows and always lands on the label/button, so focus
  // never enters an <input> - and SDL never starts text input - unless the
  // user presses right on a row.
  struct FormRow { const char* label; const char* input; };
  constexpr FormRow kFormRows[] = {
    {"lbl-name", "in-name"},
    {"lbl-host", "in-host"},
    {"lbl-port", "in-port"},
    {"lbl-user", "in-user"},
    {"lbl-pass", "in-pass"},
    {"btn-save", "btn-cancel"},
  };
  constexpr int kFormRowCount = (int)(sizeof(kFormRows) / sizeof(kFormRows[0]));
}

void App::RebuildServerRows() {
  server_rows_.clear();
  server_rows_.reserve(config_.Count());
  for (const ServerProfile& p : config_.Servers()) {
    ServerRow row;
    row.name = p.Label();
    row.detail = p.Detail();
    server_rows_.push_back(std::move(row));
  }

  server_count_ = (int)server_rows_.size();
  sel_server_ = std::clamp(sel_server_, 0, std::max(0, server_count_ - 1));
  model_.DirtyVariable("servers");
  model_.DirtyVariable("server_count");
  model_.DirtyVariable("sel_server");
}

void App::LoadFormFromSelection() {
  const ServerProfile* p = config_.At(sel_server_);
  if (p) {
    cfg_name_ = p->name;
    cfg_host_ = p->host;
    cfg_port_ = p->port;
    cfg_user_ = p->user;
    cfg_pass_ = p->pass;
  } else {
    cfg_name_.clear();
    cfg_host_.clear();
    cfg_port_ = "9982";
    cfg_user_.clear();
    cfg_pass_.clear();
  }
  model_.DirtyVariable("name");
  model_.DirtyVariable("host");
  model_.DirtyVariable("port");
  model_.DirtyVariable("user");
  model_.DirtyVariable("pass");
}

ServerProfile App::FormToProfile() const {
  ServerProfile p;
  p.name = cfg_name_;
  p.host = cfg_host_;
  p.port = cfg_port_.empty() ? std::string("9982") : std::string(cfg_port_);
  p.user = cfg_user_;
  p.pass = cfg_pass_;
  return p;
}

// Opens the modal add/edit dialog. Adding starts from a blank profile and
// leaves sel_server_ past the end of the list, which is how SaveServer()
// knows to append rather than overwrite.
void App::OpenServerDialog(bool add_new) {
  if (add_new)
    sel_server_ = (int)config_.Count();
  else if (!config_.At(sel_server_))
    return; // nothing to edit

  LoadFormFromSelection();
  bind_dialog_ = add_new ? "add" : "edit";
  bind_dialog_title_ = add_new ? "Add server" : "Edit server";
  bind_status_.clear(); // don't carry a previous connection error into the dialog
  SetConnectZone(Zone::ServerForm);
  model_.DirtyVariable("sel_server");
  model_.DirtyVariable("dialog");
  model_.DirtyVariable("dialog_title");
  model_.DirtyVariable("status");

  if (Rml::Element* el = document_->GetElementById("lbl-name"))
    el->Focus();
}

void App::CloseServerDialog() {
  if (bind_dialog_.empty())
    return;

  bind_dialog_.clear();
  bind_dialog_title_.clear();
  model_.DirtyVariable("dialog");
  model_.DirtyVariable("dialog_title");

  // Adding was abandoned: put the cursor back on a real row.
  sel_server_ = std::clamp(sel_server_, 0, std::max(0, server_count_ - 1));
  model_.DirtyVariable("sel_server");

  SetConnectZone(Zone::ServerList);
  // Take focus off the text inputs so the on-screen keyboard closes.
  if (document_)
    document_->Focus();
}

// Commits the edit fields into the profile list, adding a new entry when the
// dialog was opened with Options rather than Square. Connecting is left to
// the server list, so editing a server does not disturb the current session.
void App::SaveServer() {
  const ServerProfile p = FormToProfile();
  if (!p.Valid()) {
    bind_status_ = "Enter a hostname first";
    model_.DirtyVariable("status");
    return;
  }

  if (sel_server_ >= (int)config_.Count())
    sel_server_ = config_.Add(p);
  else
    config_.Update(sel_server_, p);

  // last_used_ is deliberately not touched here: it records the server that
  // last connected successfully, which PollConnectState() sets.
  config_.Save(kConfigPath);
  RebuildServerRows();
  LoadFormFromSelection();
  CloseServerDialog();
}

void App::DeleteSelectedServer() {
  const ServerProfile* p = config_.At(sel_server_);
  if (!p)
    return;

  // Destructive: require a second press on the same row within the window.
  const double now = Now();
  if (confirm_server_index_ != sel_server_ || now >= confirm_deadline_) {
    confirm_server_index_ = sel_server_;
    confirm_deadline_ = now + kConfirmWindowSec;
    ShowToast("Press Triangle again to delete " + p->Label());
    return;
  }
  confirm_server_index_ = -1;
  confirm_deadline_ = 0.0;

  const std::string name = p->Label();
  config_.Remove(sel_server_);
  config_.Save(kConfigPath);
  sel_server_ = std::min(sel_server_, (int)config_.Count());
  RebuildServerRows();
  LoadFormFromSelection();
}

void App::SetConnectZone(Zone zone) {
  zone_ = zone;
  bind_connect_zone_ = (zone == Zone::ServerList) ? "list" : "form";
  model_.DirtyVariable("connect_zone");
}

void App::StartConnect() {
  if (connect_state_ == ConnectState::Busy)
    return;

  const ServerProfile* p = config_.At(sel_server_);
  if (!p) {
    ShowToast("Press Options to add a server");
    return;
  }

  if (connect_thread_.joinable())
    connect_thread_.join();

  bind_status_ = "Connecting to " + p->host + "...";
  connecting_ = true;
  connecting_index_ = sel_server_;
  model_.DirtyVariable("status");

  const std::string host = p->host;
  const std::string user = p->user;
  const std::string pass = p->pass;
  const int port = p->PortNumber();

  connect_state_ = ConnectState::Busy;
  connect_thread_ = std::thread([this, host, port, user, pass]() {
    std::string error;
    if (!client_.Connect(host, port, user, pass, error))
      {
	connect_error_ = error;
	connect_state_ = ConnectState::Failed;
	return;
      }
    client_.StartAsync(); // UI observes progress via Generation()
    connect_state_ = ConnectState::Success;
  });
}

void App::PollConnectState() {
  const ConnectState state = connect_state_.load();
  if (state != ConnectState::Success && state != ConnectState::Failed)
    return;

  if (connect_thread_.joinable())
    connect_thread_.join();

  connect_state_ = ConnectState::Idle;
  connecting_ = false;

  if (state == ConnectState::Failed) {
    bind_status_ = connect_error_;
    model_.DirtyVariable("status");
    return;
  }

  // Remember which server worked, so the next launch preselects it.
  if (connecting_index_ >= 0) {
    config_.SetLastUsed(connecting_index_);
    config_.Save(kConfigPath);
  }
  SwitchToMain();
}

// Returns the row index of the currently focused element, and whether the
// focus sits on the row's input (as opposed to its label/button).
int App::CurrentFormRow(bool* in_input) const
{
  if (in_input)
    *in_input = false;

  Rml::Element* focused = document_->GetFocusLeafNode();
  if (!focused)
    return 0;

  for (int i = 0; i < kFormRowCount; ++i)
    {
      if (Rml::Element* el = document_->GetElementById(kFormRows[i].label))
	if (el == focused || el->GetFirstChild() == focused || focused->GetParentNode() == el)
	  return i;

      if (!kFormRows[i].input)
	continue;

      if (Rml::Element* el = document_->GetElementById(kFormRows[i].input))
	if (el == focused || el->GetFirstChild() == focused || focused->GetParentNode() == el)
	  {
	    if (in_input)
	      *in_input = true;
	    return i;
	  }
    }

  return 0;
}

void App::FocusFormField(int delta)
{
  const int current = CurrentFormRow(nullptr);
  const int next = std::clamp(current + delta, 0, kFormRowCount - 1);
  if (Rml::Element* el = document_->GetElementById(kFormRows[next].label))
    el->Focus();
}

// Right pressed: move from a label into its input. Returns false when the
// key should instead be handled by whoever has focus (e.g. caret movement
// inside a text input).
bool App::FocusFormInput()
{
  bool in_input = false;
  const int row = CurrentFormRow(&in_input);
  if (in_input || !kFormRows[row].input)
    return false;

  if (Rml::Element* el = document_->GetElementById(kFormRows[row].input))
    {
      el->Focus();
      return true;
    }
  return false;
}

// Left pressed: move from an input back to its label, but only when the
// caret is already at the start of the text so left/right still work for
// editing inside the field.
bool App::FocusFormLabel()
{
  bool in_input = false;
  const int row = CurrentFormRow(&in_input);
  if (!in_input)
    return false;

  auto* input = dynamic_cast<Rml::ElementFormControlInput*>(
      document_->GetElementById(kFormRows[row].input));
  if (input)
    {
      int sel_start = 0, sel_end = 0;
      Rml::String sel_text;
      input->GetSelection(&sel_start, &sel_end, &sel_text);
      if (sel_start != 0 || sel_end != 0)
	return false; // caret not at the start; let the input handle it
    }

  if (Rml::Element* el = document_->GetElementById(kFormRows[row].label))
    {
      el->Focus();
      return true;
    }
  return false;
}

void App::HandleKeyConnect(Rml::Event& event, int key)
{
  if (zone_ == Zone::ServerList)
    {
      switch (keymap::Command(key))
	{
	case keymap::Cmd::Up:
	  MoveSelection(-1);
	  break;
	case keymap::Cmd::Down:
	  MoveSelection(+1);
	  break;
	case keymap::Cmd::Options: // options / the remote's menu button
	  OpenServerDialog(true);
	  break;
	case keymap::Cmd::Secondary: // square: edit this server
	  OpenServerDialog(false);
	  break;
	case keymap::Cmd::Remove: // triangle: forget this server
	  DeleteSelectedServer();
	  break;
	case keymap::Cmd::Ok:
	case keymap::Cmd::PlayPause: // the remote's play button connects too
	  StartConnect();
	  break;
	default:
	  return;
	}
      event.StopPropagation();
      return;
    }

  // Form zone. This is the one place with text fields, so Cmd::Back is not
  // used here: it also covers Backspace, which has to reach the inputs.
  // Escape alone - circle on the pad, back on the remote - closes the dialog.
  if (key == Rml::Input::KI_ESCAPE)
    {
      CloseServerDialog();
      event.StopPropagation();
      return;
    }

  switch (key)
    {
    case Rml::Input::KI_UP:
      FocusFormField(-1);
      event.StopPropagation();
      break;
    case Rml::Input::KI_DOWN:
      FocusFormField(+1);
      event.StopPropagation();
      break;
    case Rml::Input::KI_RIGHT:
      if (FocusFormInput())
	event.StopPropagation();
      break; // otherwise: caret movement inside an input
    case Rml::Input::KI_LEFT:
      // Only meaningful inside a text field; the dialog is modal, so a left
      // press on a label does nothing.
      if (FocusFormLabel())
	event.StopPropagation();
      break;
    default:
      // Everything else - including KI_RETURN - propagates. RmlUi's document
      // default action clicks the focused element on Enter/Space, so the
      // Save and Cancel buttons activate via their data-event-click handlers
      // when focused. There is no longer a global save key.
      break;
    }
}
