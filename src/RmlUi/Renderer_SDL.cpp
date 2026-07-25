#include "Renderer_SDL.h"
#include <RmlUi/Core/Core.h>
#include <RmlUi/Core/FileInterface.h>
#include <RmlUi/Core/Types.h>

#if SDL_MAJOR_VERSION >= 3
	#include <SDL3_image/SDL_image.h>
#else
	#include <SDL_image.h>
#endif


static void SetRenderClipRect(SDL_Renderer* renderer, const SDL_Rect* rect)
{
#if SDL_MAJOR_VERSION >= 3
	SDL_SetRenderClipRect(renderer, rect);
#else
	SDL_RenderSetClipRect(renderer, rect);
#endif
}
static void SetRenderViewport(SDL_Renderer* renderer, const SDL_Rect* rect)
{
#if SDL_MAJOR_VERSION >= 3
	SDL_SetRenderViewport(renderer, rect);
#else
	SDL_RenderSetViewport(renderer, rect);
#endif
}

RenderInterface_SDL::RenderInterface_SDL(SDL_Renderer* renderer) : renderer(renderer)
{
	// RmlUi serves vertex colors and textures with premultiplied alpha, set the blend mode accordingly.
	// Equivalent to glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA).
	blend_mode = SDL_ComposeCustomBlendMode(SDL_BLENDFACTOR_ONE, SDL_BLENDFACTOR_ONE_MINUS_SRC_ALPHA, SDL_BLENDOPERATION_ADD, SDL_BLENDFACTOR_ONE,
		SDL_BLENDFACTOR_ONE_MINUS_SRC_ALPHA, SDL_BLENDOPERATION_ADD);

	// Custom blend modes are not available everywhere; in particular the
	// software renderer rejects them, which would leave translucent geometry
	// rendered without any blending (dark boxes and lines over the UI).
	// Fall back to standard straight-alpha blending and un-premultiply all
	// colors and textures on the way in.
	if (SDL_SetRenderDrawBlendMode(renderer, blend_mode) != 0)
	{
		blend_mode = SDL_BLENDMODE_BLEND;
		premultiplied_alpha = false;
		SDL_SetRenderDrawBlendMode(renderer, blend_mode);
	}

	// Always on for SDL2: rounded borders/backgrounds tessellate into thin
	// sliver triangles, and SDL_RenderGeometry's batched path suppresses
	// shared edges between consecutive triangles, dropping whole pixel rows
	// (dark cracks across the element). Submitting one triangle per call
	// rasterizes each independently with a watertight fill rule.
	split_untextured_triangles = true;

	// Only the software renderer uses the flawed SDL_triangle.c rasterizer;
	// GPU backends interpolate texture coordinates correctly.
	{
#if SDL_MAJOR_VERSION >= 3
		const char* name = SDL_GetRendererName(renderer);
		blit_textured_quads = (name && SDL_strcmp(name, SDL_SOFTWARE_RENDERER) == 0);
#else
		SDL_RendererInfo info;
		blit_textured_quads = (SDL_GetRendererInfo(renderer, &info) == 0 && info.name && SDL_strcmp(info.name, "software") == 0);
#endif
	}
}

// If the six indices starting at `indices` describe an axis-aligned,
// uniformly colored, non-flipped textured quad, fill out the equivalent
// texel source rectangle, destination rectangle and modulation color and
// return true. Otherwise return false so the caller can fall back to
// SDL_RenderGeometry.
static bool GetTexturedQuad(float texture_w, float texture_h, const SDL_Vertex* vertices, const int* indices, SDL_Rect& out_src, SDL_FRect& out_dst,
	SDL_Color& out_color)
{
	// Collect the (up to) four unique vertices referenced by the two triangles.
	int unique[4];
	int num_unique = 0;
	for (int i = 0; i < 6; i++)
	{
		const int index = indices[i];
		bool found = false;
		for (int j = 0; j < num_unique; j++)
			found |= (unique[j] == index);
		if (found)
			continue;
		if (num_unique == 4)
			return false;
		unique[num_unique++] = index;
	}
	if (num_unique != 4)
		return false;

	const SDL_Vertex& first = vertices[unique[0]];
	float x0 = first.position.x, x1 = first.position.x;
	float y0 = first.position.y, y1 = first.position.y;
	for (int i = 1; i < 4; i++)
	{
		const SDL_FPoint& p = vertices[unique[i]].position;
		x0 = SDL_min(x0, p.x); x1 = SDL_max(x1, p.x);
		y0 = SDL_min(y0, p.y); y1 = SDL_max(y1, p.y);
	}
	if (x1 - x0 < 0.5f || y1 - y0 < 0.5f)
		return false;

	// Each vertex must sit on a distinct corner of the bounding rectangle,
	// texture coordinates must follow the same corners (no rotation/flip),
	// and the color must be uniform.
	float u0 = 0, u1 = 0, v0 = 0, v1 = 0;
	int corners_seen = 0;
	for (int i = 0; i < 4; i++)
	{
		const SDL_Vertex& vert = vertices[unique[i]];
		const bool right = (vert.position.x == x1);
		const bool bottom = (vert.position.y == y1);
		if ((vert.position.x != x0 && !right) || (vert.position.y != y0 && !bottom))
			return false;
		const int corner = (right ? 1 : 0) | (bottom ? 2 : 0);
		if (corners_seen & (1 << corner))
			return false;
		corners_seen |= (1 << corner);

		if (vert.color.r != first.color.r || vert.color.g != first.color.g || vert.color.b != first.color.b || vert.color.a != first.color.a)
			return false;

		(right ? u1 : u0) = vert.tex_coord.x;
		(bottom ? v1 : v0) = vert.tex_coord.y;
	}
	if (corners_seen != 0xF)
		return false;

	// Consistency: opposing corners must agree on their shared u/v.
	for (int i = 0; i < 4; i++)
	{
		const SDL_Vertex& vert = vertices[unique[i]];
		if (vert.tex_coord.x != ((vert.position.x == x1) ? u1 : u0) || vert.tex_coord.y != ((vert.position.y == y1) ? v1 : v0))
			return false;
	}
	if (u1 < u0 || v1 < v0)
		return false; // Flipped; SDL_RenderCopyF cannot express this.

	out_src = {(int)SDL_roundf(u0 * texture_w), (int)SDL_roundf(v0 * texture_h), (int)SDL_roundf((u1 - u0) * texture_w),
		(int)SDL_roundf((v1 - v0) * texture_h)};
	out_dst = {x0, y0, x1 - x0, y1 - y0};
	out_color = first.color;
	return true;
}

static void BlitTexturedQuad(SDL_Renderer* renderer, SDL_Texture* texture, const SDL_Rect& src, const SDL_FRect& dst, SDL_Color color)
{
	SDL_SetTextureColorMod(texture, color.r, color.g, color.b);
	SDL_SetTextureAlphaMod(texture, color.a);
#if SDL_MAJOR_VERSION >= 3
	const SDL_FRect src_f = {(float)src.x, (float)src.y, (float)src.w, (float)src.h};
	SDL_RenderTexture(renderer, texture, &src_f, &dst);
#else
	SDL_RenderCopyF(renderer, texture, &src, &dst);
#endif
	SDL_SetTextureColorMod(texture, 255, 255, 255);
	SDL_SetTextureAlphaMod(texture, 255);
}

void RenderInterface_SDL::BeginFrame()
{
	SetRenderViewport(renderer, nullptr);
	SDL_SetRenderDrawColor(renderer, 0, 0, 0, 255);
	SDL_RenderClear(renderer);
	SDL_SetRenderDrawBlendMode(renderer, blend_mode);
}

void RenderInterface_SDL::EndFrame() {}

Rml::CompiledGeometryHandle RenderInterface_SDL::CompileGeometry(Rml::Span<const Rml::Vertex> vertices, Rml::Span<const int> indices)
{
	GeometryView* data = new GeometryView{vertices, indices};
	return reinterpret_cast<Rml::CompiledGeometryHandle>(data);
}

void RenderInterface_SDL::ReleaseGeometry(Rml::CompiledGeometryHandle geometry)
{
	delete reinterpret_cast<GeometryView*>(geometry);
}

void RenderInterface_SDL::RenderGeometry(Rml::CompiledGeometryHandle handle, Rml::Vector2f translation, Rml::TextureHandle texture)
{
	const GeometryView* geometry = reinterpret_cast<GeometryView*>(handle);
	const Rml::Vertex* vertices = geometry->vertices.data();
	const size_t num_vertices = geometry->vertices.size();
	const int* indices = geometry->indices.data();
	const size_t num_indices = geometry->indices.size();

	if (sdl_vertices_size < num_vertices)
	{
		sdl_vertices_size = num_vertices * 1.5;
		sdl_vertices.reset(new SDL_Vertex[sdl_vertices_size]);
	}

	for (size_t i = 0; i < num_vertices; i++)
	{
		SDL_Vertex& sdl_vertex = sdl_vertices[i];
		// Snap to the pixel grid: without AA, fractional element positions
		// otherwise produce uneven 1px coverage bands along the horizontal
		// edges of rounded boxes.
		sdl_vertex.position = {SDL_roundf(vertices[i].position.x + translation.x), SDL_roundf(vertices[i].position.y + translation.y)};
		sdl_vertex.tex_coord = {vertices[i].tex_coord.x, vertices[i].tex_coord.y};

		auto color = vertices[i].colour;
		if (!premultiplied_alpha && color.alpha != 0 && color.alpha != 255)
		{
			// Back to straight alpha for SDL_BLENDMODE_BLEND.
			const int a = color.alpha;
			color.red = Rml::byte(SDL_min(255, int(color.red) * 255 / a));
			color.green = Rml::byte(SDL_min(255, int(color.green) * 255 / a));
			color.blue = Rml::byte(SDL_min(255, int(color.blue) * 255 / a));
		}
		sdl_vertex.color = {color.red, color.green, color.blue, color.alpha};
	}

	SDL_Texture* sdl_texture = (SDL_Texture*)texture;

	if (split_untextured_triangles && !sdl_texture)
	{
		for (size_t i = 0; i + 2 < num_indices; i += 3)
			SDL_RenderGeometry(renderer, sdl_texture, sdl_vertices.get(), (int)num_vertices, indices + i, 3);
	}
	else if (blit_textured_quads && sdl_texture)
	{
		float texture_w = 0.f, texture_h = 0.f;
#if SDL_MAJOR_VERSION >= 3
		SDL_GetTextureSize(sdl_texture, &texture_w, &texture_h);
#else
		int w = 0, h = 0;
		SDL_QueryTexture(sdl_texture, nullptr, nullptr, &w, &h);
		texture_w = (float)w;
		texture_h = (float)h;
#endif
		// Blit each axis-aligned quad exactly; submit any run of indices
		// that does not form quads through SDL_RenderGeometry unchanged.
		size_t run_begin = 0;
		for (size_t i = 0; i + 5 < num_indices; i += 6)
		{
			SDL_Rect src;
			SDL_FRect dst;
			SDL_Color color;
			if (!GetTexturedQuad(texture_w, texture_h, sdl_vertices.get(), indices + i, src, dst, color))
				continue;
			// Flush preceding non-quad indices first to preserve draw order.
			if (i > run_begin)
				SDL_RenderGeometry(renderer, sdl_texture, sdl_vertices.get(), (int)num_vertices, indices + run_begin, (int)(i - run_begin));
			BlitTexturedQuad(renderer, sdl_texture, src, dst, color);
			run_begin = i + 6;
		}
		if (num_indices > run_begin)
			SDL_RenderGeometry(renderer, sdl_texture, sdl_vertices.get(), (int)num_vertices, indices + run_begin, (int)(num_indices - run_begin));
	}
	else
	{
		SDL_RenderGeometry(renderer, sdl_texture, sdl_vertices.get(), (int)num_vertices, indices, (int)num_indices);
	}
}

void RenderInterface_SDL::EnableScissorRegion(bool enable)
{
	if (enable)
		SetRenderClipRect(renderer, &rect_scissor);
	else
		SetRenderClipRect(renderer, nullptr);

	scissor_region_enabled = enable;
}

void RenderInterface_SDL::SetScissorRegion(Rml::Rectanglei region)
{
	rect_scissor.x = region.Left();
	rect_scissor.y = region.Top();
	rect_scissor.w = region.Width();
	rect_scissor.h = region.Height();

	if (scissor_region_enabled)
		SetRenderClipRect(renderer, &rect_scissor);
}

Rml::TextureHandle RenderInterface_SDL::LoadTexture(Rml::Vector2i& texture_dimensions, const Rml::String& source)
{
	Rml::FileInterface* file_interface = Rml::GetFileInterface();
	Rml::FileHandle file_handle = file_interface->Open(source);
	if (!file_handle)
		return {};

	file_interface->Seek(file_handle, 0, SEEK_END);
	size_t buffer_size = file_interface->Tell(file_handle);
	file_interface->Seek(file_handle, 0, SEEK_SET);

	using Rml::byte;
	Rml::UniquePtr<byte[]> buffer(new byte[buffer_size]);
	file_interface->Read(buffer.get(), buffer_size, file_handle);
	file_interface->Close(file_handle);

	const size_t i_ext = source.rfind('.');
	Rml::String extension = (i_ext == Rml::String::npos ? Rml::String() : source.substr(i_ext + 1));

#if SDL_MAJOR_VERSION >= 3
	auto CreateSurface = [&]() { return IMG_LoadTyped_IO(SDL_IOFromMem(buffer.get(), int(buffer_size)), 1, extension.c_str()); };
	auto GetSurfaceFormat = [](SDL_Surface* surface) { return surface->format; };
	auto ConvertSurface = [](SDL_Surface* surface, SDL_PixelFormat format) { return SDL_ConvertSurface(surface, format); };
	auto DestroySurface = [](SDL_Surface* surface) { SDL_DestroySurface(surface); };
#else
	auto CreateSurface = [&]() { return IMG_LoadTyped_RW(SDL_RWFromMem(buffer.get(), int(buffer_size)), 1, extension.c_str()); };
	auto GetSurfaceFormat = [](SDL_Surface* surface) { return surface->format->format; };
	auto ConvertSurface = [](SDL_Surface* surface, Uint32 format) { return SDL_ConvertSurfaceFormat(surface, format, 0); };
	auto DestroySurface = [](SDL_Surface* surface) { SDL_FreeSurface(surface); };
#endif

	SDL_Surface* surface = CreateSurface();
	if (!surface)
		return {};

	texture_dimensions = {surface->w, surface->h};

	if (GetSurfaceFormat(surface) != SDL_PIXELFORMAT_RGBA32 && GetSurfaceFormat(surface) != SDL_PIXELFORMAT_BGRA32)
	{
		SDL_Surface* converted_surface = ConvertSurface(surface, SDL_PIXELFORMAT_RGBA32);
		DestroySurface(surface);
		if (!converted_surface)
			return {};

		surface = converted_surface;
	}

	// Convert colors to premultiplied alpha, which is necessary for correct
	// alpha compositing. Loaded images are straight alpha already, which is
	// exactly what the straight-alpha fallback path wants, so skip it there.
	if (premultiplied_alpha)
	{
		const size_t pixels_byte_size = surface->w * surface->h * 4;
		byte* pixels = static_cast<byte*>(surface->pixels);
		for (size_t i = 0; i < pixels_byte_size; i += 4)
		{
			const byte alpha = pixels[i + 3];
			for (size_t j = 0; j < 3; ++j)
				pixels[i + j] = byte(int(pixels[i + j]) * int(alpha) / 255);
		}
	}

	SDL_Texture* texture = SDL_CreateTextureFromSurface(renderer, surface);
	texture_dimensions = Rml::Vector2i(surface->w, surface->h);
	DestroySurface(surface);

	if (texture)
		SDL_SetTextureBlendMode(texture, blend_mode);

	return (Rml::TextureHandle)texture;
}

Rml::TextureHandle RenderInterface_SDL::GenerateTexture(Rml::Span<const Rml::byte> source, Rml::Vector2i source_dimensions)
{
	RMLUI_ASSERT(source.data() && source.size() == size_t(source_dimensions.x * source_dimensions.y * 4));

#if SDL_MAJOR_VERSION >= 3
	auto CreateSurface = [&]() {
		return SDL_CreateSurfaceFrom(source_dimensions.x, source_dimensions.y, SDL_PIXELFORMAT_RGBA32, (void*)source.data(), source_dimensions.x * 4);
	};
	auto DestroySurface = [](SDL_Surface* surface) { SDL_DestroySurface(surface); };
#else
	auto CreateSurface = [&]() {
		return SDL_CreateRGBSurfaceWithFormatFrom((void*)source.data(), source_dimensions.x, source_dimensions.y, 32, source_dimensions.x * 4,
			SDL_PIXELFORMAT_RGBA32);
	};
	auto DestroySurface = [](SDL_Surface* surface) { SDL_FreeSurface(surface); };
#endif

	// RmlUi hands premultiplied RGBA; the straight-alpha fallback needs it
	// un-premultiplied.
	Rml::UniquePtr<Rml::byte[]> straight;
	if (!premultiplied_alpha)
	{
		straight.reset(new Rml::byte[source.size()]);
		for (size_t i = 0; i < source.size(); i += 4)
		{
			const Rml::byte a = source[i + 3];
			for (size_t j = 0; j < 3; ++j)
				straight[i + j] = (a == 0 || a == 255) ? source[i + j] : Rml::byte(SDL_min(255, int(source[i + j]) * 255 / int(a)));
			straight[i + 3] = a;
		}
		source = {straight.get(), source.size()};
	}

	SDL_Surface* surface = CreateSurface();

	SDL_Texture* texture = SDL_CreateTextureFromSurface(renderer, surface);
	SDL_SetTextureBlendMode(texture, blend_mode);

	DestroySurface(surface);
	return (Rml::TextureHandle)texture;
}

void RenderInterface_SDL::ReleaseTexture(Rml::TextureHandle texture_handle)
{
	SDL_DestroyTexture((SDL_Texture*)texture_handle);
}
