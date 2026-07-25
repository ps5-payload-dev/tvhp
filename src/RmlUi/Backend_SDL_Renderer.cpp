#include <RmlUi/Core/Context.h>
#include <RmlUi/Core/Core.h>
#include <RmlUi/Core/Log.h>
#include <RmlUi/Core/Profiling.h>

#include "Backend.h"
#include "Platform_SDL.h"
#include "Renderer_SDL.h"


struct BackendData {
  BackendData(SDL_Window* window, SDL_Renderer* renderer) : system_interface(window), render_interface(renderer) {}

  SystemInterface_SDL system_interface;
  RenderInterface_SDL render_interface;
  TextInputMethodEditor_SDL text_input_method_editor;

  SDL_Window* window = nullptr;
  SDL_Renderer* renderer = nullptr;

  bool running = true;
};

static Rml::UniquePtr<BackendData> data;

bool Backend::Initialize(const char* window_name, int width, int height, bool allow_resize) {
  if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS | SDL_INIT_TIMER | SDL_INIT_AUDIO | SDL_INIT_GAMECONTROLLER) != 0) {
    return false;
  }

  const Uint32 window_flags = (allow_resize ? SDL_WINDOW_RESIZABLE : 0);
  SDL_Window* window = SDL_CreateWindow(window_name, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, width, height, window_flags);
  SDL_StopTextInput();

  if (!window) {
    Rml::Log::Message(Rml::Log::LT_ERROR, "SDL error on create window: %s", SDL_GetError());
    return false;
  }

  // Software rendering only: no OpenGL (or any other GPU API) is used.
  SDL_SetHint(SDL_HINT_RENDER_DRIVER, "software");
  SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "linear");
  SDL_Renderer* renderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_SOFTWARE | SDL_RENDERER_PRESENTVSYNC);
  if (!renderer) {
    Rml::Log::Message(Rml::Log::LT_ERROR, "SDL error on create renderer: %s", SDL_GetError());
    return false;
  }

  SDL_RendererInfo info;
  if (SDL_GetRendererInfo(renderer, &info) == 0) {
    Rml::Log::Message(Rml::Log::LT_INFO, "Using SDL renderer: %s", info.name);
  }

  data = Rml::MakeUnique<BackendData>(window, renderer);
  data->window = window;
  data->renderer = renderer;

  Rml::SetTextInputHandler(&data->text_input_method_editor);

  return true;
}

void Backend::Shutdown() {
  SDL_DestroyRenderer(data->renderer);
  SDL_DestroyWindow(data->window);
  data.reset();
  SDL_Quit();
}

Rml::SystemInterface* Backend::GetSystemInterface() {
  return &data->system_interface;
}

Rml::RenderInterface* Backend::GetRenderInterface() {
  return &data->render_interface;
}

SDL_Renderer* Backend::GetSDLRenderer() {
  return data ? data->renderer : nullptr;
}

bool Backend::ProcessEvents(Rml::Context* context) {
  SDL_GameController* controller;
  SDL_Event ev;

  while (SDL_PollEvent(&ev)) {
    switch (ev.type) {
    case SDL_QUIT:
      return false;

    case SDL_TEXTEDITING:
      data->text_input_method_editor.HandleEdit(ev.edit);
      break;

    case SDL_CONTROLLERDEVICEADDED:
      SDL_GameControllerOpen(ev.cdevice.which);
      break;

    case SDL_CONTROLLERDEVICEREMOVED:
      if ((controller=SDL_GameControllerFromInstanceID(ev.cdevice.which))) {
        SDL_GameControllerClose(controller);
      }
      break;

    default:
      RmlSDL::InputEventHandler(context, data->window, ev);
      break;
    }
  }

  return true;
}

void Backend::BeginFrame() {
  SDL_SetRenderDrawColor(data->renderer, 0, 0, 0, 0);
  SDL_RenderClear(data->renderer);
  data->render_interface.BeginFrame();
}

void Backend::PresentFrame() {
  data->render_interface.EndFrame();
  SDL_RenderPresent(data->renderer);
}
