#pragma once

#include <RmlUi/Core/RenderInterface.h>
#include <RmlUi/Core/SystemInterface.h>
#include <RmlUi/Core/Types.h>

struct SDL_Renderer;

namespace Backend {
  /**
   * Initialize the backend, including the system and render interfaces, and
   * open a window for rendering the RmlUi context.
   **/
  bool Initialize(const char* window_name, int width, int height, bool allow_resize);

  /**
   * Close the window and release all resources owned by the backend, including
   * the system and render interfaces.
   **/
  void Shutdown();

  /**
   * Return a pointer to the system interface which should be provided to RmlUi.
   **/
  Rml::SystemInterface* GetSystemInterface();

  /**
   * Return a pointer to the render interface which should be provided to RmlUi.
   **/
  Rml::RenderInterface* GetRenderInterface();

  /**
   * Poll and process events from the current platform, and apply any
   * relevant events to the provided RmlUi context. Return false when
   * the application should be closed.
   **/
  bool ProcessEvents(Rml::Context* context);

  /**
   * Prepare the render state to accept rendering commands from RmlUi (call
   * before rendering the RmlUi context).
   **/
  void BeginFrame();

  /**
   * Present the rendered frame to the screen (call after rendering the RmlUi
   * context).
   **/
  void PresentFrame();

  /**
   * The SDL_Renderer the UI is drawn with, for compositing video beneath it.
   * Only valid between Initialize() and Shutdown().
   **/
  SDL_Renderer* GetSDLRenderer();
}
