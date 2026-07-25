#pragma once

#include <SDL.h>
#include <RmlUi/Core/RenderInterface.h>


class RenderInterface_SDL : public Rml::RenderInterface {
public:
  RenderInterface_SDL(SDL_Renderer* renderer);

  void BeginFrame();
  void EndFrame();

  Rml::CompiledGeometryHandle CompileGeometry(Rml::Span<const Rml::Vertex> vertices, Rml::Span<const int> indices) override;
  void ReleaseGeometry(Rml::CompiledGeometryHandle geometry) override;
  void RenderGeometry(Rml::CompiledGeometryHandle handle, Rml::Vector2f translation, Rml::TextureHandle texture) override;

  Rml::TextureHandle LoadTexture(Rml::Vector2i& texture_dimensions, const Rml::String& source) override;
  Rml::TextureHandle GenerateTexture(Rml::Span<const Rml::byte> source, Rml::Vector2i source_dimensions) override;
  void ReleaseTexture(Rml::TextureHandle texture_handle) override;

  void EnableScissorRegion(bool enable) override;
  void SetScissorRegion(Rml::Rectanglei region) override;

private:
  struct GeometryView {
    Rml::Span<const Rml::Vertex> vertices;
    Rml::Span<const int> indices;
  };

  SDL_Renderer* renderer;
  SDL_BlendMode blend_mode = {};

  // False when the renderer does not support custom blend modes (notably
  // the software renderer): colors/textures are then converted back to
  // straight alpha and composited with SDL_BLENDMODE_BLEND.
  bool premultiplied_alpha = true;

  // SDL_RenderGeometry merges consecutive triangles that share an edge and
  // suppresses the shared edge; on the thin sliver triangles produced for
  // rounded borders/backgrounds this eats whole pixel rows (dark cracks).
  // Per-triangle submission rasterizes each triangle independently with a
  // watertight fill rule. Textured geometry (font glyphs: true rectangle
  // pairs) stays batched, where the merged path is correct and faster.
  bool split_untextured_triangles = false;
  // The software renderer's triangle rasterizer (SDL_triangle.c)
  // interpolates texture coordinates at integer pixel coordinates instead
  // of pixel centers. On glyph quads this duplicates one texel row/column
  // at the triangle seam and never samples the last one: the bottom (and
  // right) pixel row of every glyph is cut off. Axis-aligned textured
  // quads are therefore detected and blitted with SDL_RenderCopyF, which
  // samples the exact texel rectangle.
  bool blit_textured_quads = false;

  SDL_Rect rect_scissor = {};
  bool scissor_region_enabled = false;
  Rml::UniquePtr<SDL_Vertex[]> sdl_vertices;
  size_t sdl_vertices_size = 0;
};
