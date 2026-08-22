#pragma once

#include <RmlUi/Core/Input.h>

// Every key the UI reacts to, in one place.
//
// Three input devices have to end up on the same set of commands:
//
//   * a DualSense pad, whose buttons are turned into key identifiers by
//     RmlSDL::ConvertControllerButton(),
//   * the HDMI-CEC remote control, which the PS5 port of SDL2 reports as
//     plain keyboard keys (remote_map[] in src/joystick/ps5/SDL_ps5joystick.c),
//   * an ordinary USB keyboard.
//
// The remote's table is fixed and is not ours to change, so it dictates the
// choice of identifiers here; the pad and the keyboard are then mapped onto
// the same keys. Two of its entries in particular decide the layout:
//
//   * "back" arrives as Escape, so Escape - not Backspace - is the cancel
//     key, and Circle is mapped to Escape to match,
//   * "play/pause" arrives as Space, so Space is a transport key and Square
//     has moved to Tab.
//
// The consequence is that nothing here is reachable by a single device only:
// every command below can be given from the pad, from the remote, or from a
// keyboard, and the button hints in main.rml stay accurate for the pad.

namespace keymap {

  // SDL reports the remote's rewind and fast-forward buttons as
  // SDLK_AUDIOREWIND and SDLK_AUDIOFASTFORWARD. RmlUi has no identifiers for
  // those, so take two of the slots it reserves for applications.
  constexpr int KI_MEDIA_REWIND = Rml::Input::KI_FIRST_CUSTOM_KEY + 0;
  constexpr int KI_MEDIA_FAST_FORWARD = Rml::Input::KI_FIRST_CUSTOM_KEY + 1;
  static_assert(KI_MEDIA_FAST_FORWARD <= Rml::Input::KI_LAST_CUSTOM_KEY,
                "custom key identifiers must stay inside RmlUi's range");

  // What a key means to the application. Each key maps to exactly one
  // command; what a command does is up to the view that receives it.
  enum class Cmd {
    None,

    // Navigation.
    Up,
    Down,
    Left,
    Right,
    PageUp,   // L1        | remote "p+"
    PageDown, // R1        | remote "p-"
    Home,
    End,

    // Actions. The comments name the pad button, then the remote button.
    Ok,        // Cross     | enter          | Enter
    Back,      // Circle    | back, prev     | Escape, Backspace
    Secondary, // Square    | display        | Tab
    Remove,    // Triangle  | red            | Delete
    Options,   // Options   | menu, context  | Insert
    Guide,     //           | guide

    // Transport. Pad users reach these through Cross and the d-pad; the
    // remote has dedicated buttons and they should do the obvious thing.
    PlayPause,   // play/pause, pause
    Stop,        // stop
    SeekBack,    // rewind
    SeekForward, // fast forward
    SkipBack,    // |<
    SkipForward, // >|
  };

  constexpr Cmd Command(int key) {
    switch (key) {
    case Rml::Input::KI_UP:                 return Cmd::Up;
    case Rml::Input::KI_DOWN:               return Cmd::Down;
    case Rml::Input::KI_LEFT:               return Cmd::Left;
    case Rml::Input::KI_RIGHT:              return Cmd::Right;
    case Rml::Input::KI_PRIOR:              return Cmd::PageUp;
    case Rml::Input::KI_NEXT:               return Cmd::PageDown;
    case Rml::Input::KI_HOME:               return Cmd::Home;
    case Rml::Input::KI_END:                return Cmd::End;

    case Rml::Input::KI_RETURN:             return Cmd::Ok;
    case Rml::Input::KI_NUMPADENTER:        return Cmd::Ok;

    // Backspace is a second cancel key for keyboards, and catches the
    // remote's "prev" button, which SDL reports as Backspace. Views that
    // contain a text field must therefore test for Escape directly rather
    // than for Cmd::Back, or typing would close them.
    case Rml::Input::KI_ESCAPE:             return Cmd::Back;
    case Rml::Input::KI_BACK:               return Cmd::Back;

    case Rml::Input::KI_TAB:                return Cmd::Secondary;
    case Rml::Input::KI_F4:                 return Cmd::Secondary;

    case Rml::Input::KI_DELETE:             return Cmd::Remove;
    case Rml::Input::KI_F5:                 return Cmd::Remove;

    case Rml::Input::KI_APPS:               return Cmd::Options;
    case Rml::Input::KI_INSERT:             return Cmd::Options;

    case Rml::Input::KI_F9:                 return Cmd::Guide;

    case Rml::Input::KI_SPACE:              return Cmd::PlayPause;
    case Rml::Input::KI_PAUSE:              return Cmd::PlayPause;
    case Rml::Input::KI_MEDIA_PLAY_PAUSE:   return Cmd::PlayPause;
    case Rml::Input::KI_MEDIA_STOP:         return Cmd::Stop;
    case Rml::Input::KI_MEDIA_PREV_TRACK:   return Cmd::SkipBack;
    case Rml::Input::KI_MEDIA_NEXT_TRACK:   return Cmd::SkipForward;
    case KI_MEDIA_REWIND:                   return Cmd::SeekBack;
    case KI_MEDIA_FAST_FORWARD:             return Cmd::SeekForward;

    default:                                return Cmd::None;
    }
  }

} // namespace keymap
