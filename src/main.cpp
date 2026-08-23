#include <filesystem>
#include <iostream>

#include <RmlUi/Core.h>

#include "RmlUi/Backend.h"
#include "app.h"

#define SCREEN_WIDTH  1920
#define SCREEN_HEIGHT 1080


int SDL_main(int argc, char* args[]) {
  const std::string cwd = std::filesystem::current_path();
  Rml::Context* ctx;
  std::string err;
  App app;

  if (!Backend::Initialize("tvhp", SCREEN_WIDTH, SCREEN_HEIGHT, false)) {
    return -1;
  }

  Rml::SetSystemInterface(Backend::GetSystemInterface());
  Rml::SetRenderInterface(Backend::GetRenderInterface());
  Rml::Initialise();

  if (!(ctx=Rml::CreateContext("main", Rml::Vector2i(SCREEN_WIDTH, SCREEN_HEIGHT)))) {
    Rml::Log::Message(Rml::Log::LT_ERROR, "Failed to create main context");
    Rml::Shutdown();
    Backend::Shutdown();
    return -1;
  }

  Rml::LoadFontFace(cwd + "/fonts/LatoLatin-Bold.ttf", false);
  Rml::LoadFontFace(cwd + "/fonts/LatoLatin-BoldItalic.ttf", false);
  Rml::LoadFontFace(cwd + "/fonts/LatoLatin-Italic.ttf", false);
  Rml::LoadFontFace(cwd + "/fonts/LatoLatin-Regular.ttf", false);
  Rml::LoadFontFace(cwd + "/fonts/NotoEmoji-VariableFont_wght.ttf", true);

  if (!app.Initialize(ctx, err)) {
    Rml::Log::Message(Rml::Log::LT_ERROR, "Failed to initialize app: %s", err.c_str());
    Rml::Shutdown();
    Backend::Shutdown();
    return -1;
  }

  while (Backend::ProcessEvents(ctx)) {
    app.Update();
    ctx->Update();

    Backend::BeginFrame();
    app.RenderVideo(SCREEN_WIDTH, SCREEN_HEIGHT);
    ctx->Render();
    Backend::PresentFrame();
  }

  app.Shutdown();
  Rml::Shutdown();
  Backend::Shutdown();

  return 0;
}


#ifndef __SCE__
int main(int argc, char** argv) {
  return SDL_main(argc, argv);
}
#endif
