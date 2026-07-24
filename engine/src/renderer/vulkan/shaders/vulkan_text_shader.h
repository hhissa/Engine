#pragma once
#include "../../../resources/bitmap_font.h"
#include "../vulkan_buffer.h"
#include "../vulkan_shader.h"
#include "../vulkan_types.inl"

#include <glm/glm.hpp>
#include <memory>
#include <optional>
#include <string_view>

class VulkanCommandBuffer;
class VulkanRenderpass;
struct Material;

// Draws a single line of text using a bitmap font baked from a .ttf file
// at construction (see BitmapFont). Runs in the same UI renderpass as
// VulkanUIShader (see VulkanRendererBackend::end_frame()), so text draws
// on top of both the raymarched scene and any UI quads already drawn.
//
// A thin wrapper around VulkanShader (Shader.Builtin.Text): owns only
// what's genuinely specific to this shader -- the baked font, and the
// glyph vertex/index buffers rebuilt every render_to() call since text
// content varies frame to frame (unlike VulkanUIShader's static quad) --
// and delegates everything else to the shared generic implementation.
class VulkanTextShader {
public:
  VulkanTextShader(VulkanContext &context, VulkanRenderpass &ui_renderpass);
  ~VulkanTextShader();

  VulkanTextShader(const VulkanTextShader &) = delete;
  VulkanTextShader &operator=(const VulkanTextShader &) = delete;
  VulkanTextShader(VulkanTextShader &&) = delete;
  VulkanTextShader &operator=(VulkanTextShader &&) = delete;

  bool is_valid() const noexcept { return valid_; }
  explicit operator bool() const noexcept { return valid_; }

  // Resets the per-frame glyph batch cursor. Must be called once per frame
  // before the first render_to() (see VulkanRendererBackend::end_frame()'s
  // queued-text flush loop): every render_to() in a frame appends its
  // glyphs at the cursor within the shared vertex/index buffers, since the
  // command buffer is only *recorded* at that point -- writing each draw's
  // glyphs at offset 0 would make every recorded draw read whichever text
  // was uploaded last by the time the GPU actually executes.
  void begin_batch();

  // Rebakes the bitmap font atlas from assets/fonts/<name>.ttf at
  // pixel_height and points every subsequent render_to() call at it,
  // replacing whatever font this shader started with (see kFontName/
  // kFontPixelHeight in the .cpp). Waits for the device to go idle first
  // -- the old atlas texture (and the descriptor binding pointing at it)
  // could still be read by an in-flight frame's command buffer, the same
  // hazard VulkanRaymarchShader::rebake() guards against. Not cheap (a
  // full re-bake + device-idle wait), so call it for a deliberate font/
  // size change, not every frame. Logs an error and leaves the current
  // font in place if name.ttf can't be baked (e.g. missing file).
  void set_font(std::string_view name, f32 pixel_height);

  // The pixel height set_font() (or the constructor) last baked -- for a
  // caller that wants to query the current size rather than track its own
  // copy (e.g. to step it up/down by a relative amount).
  f32 font_pixel_height() const noexcept { return font_pixel_height_; }

  // Draws text at origin (screen pixels -- see BitmapFont::layout() for
  // the exact baseline convention) in colour. command_buffer must
  // currently be inside the UI renderpass. Text past the shared
  // kMaxCharacters-per-frame budget (across all render_to() calls since
  // begin_batch()) is truncated with a warning.
  void render_to(VulkanCommandBuffer &command_buffer, u32 width, u32 height,
                std::string_view text, glm::vec2 origin, glm::vec4 colour);

private:
  // Per frame, shared across every render_to() call -- see begin_batch().
  static constexpr u32 kMaxCharacters = 256;

  VulkanContext *context_;

  // unique_ptr rather than std::optional: BitmapFont holds a VulkanTexture,
  // which has both its copy and move constructors deleted, so it can't be
  // moved into an optional's storage in place. set_font() below needs to
  // bake the *new* font, confirm it succeeded, and only then discard the
  // old one -- a pointer swap does that trivially, where re-emplacing an
  // optional in place would destroy the working font before the
  // replacement's success is even known.
  std::unique_ptr<BitmapFont> font_;
  // See font_pixel_height() above.
  f32 font_pixel_height_ = 0.0f;

  // Non-owning -- registered with and owned by ShaderSystem.
  VulkanShader *shader_ = nullptr;
  VulkanShader::UniformIndex projection_uniform_ =
      VulkanShader::kInvalidUniformIndex;
  VulkanShader::UniformIndex view_uniform_ =
      VulkanShader::kInvalidUniformIndex;

  // Rebuilt (up to kMaxCharacters worth per frame) as render_to() calls
  // append their glyphs at batched_characters_, since the text/position
  // can change frame to frame -- unlike VulkanUIShader's static single
  // quad, there's no fixed geometry to upload just once.
  std::optional<VulkanBuffer> vertex_buffer_;
  std::optional<VulkanBuffer> index_buffer_;

  // Glyphs written into the buffers so far this frame -- the append
  // cursor. Reset by begin_batch().
  u32 batched_characters_ = 0;

  // A trivial material acquired purely as the vehicle for this shader's
  // instance resources (the font atlas sampler binding) -- see
  // assets/materials/default_text_material.kmt and the constructor, which
  // overrides diffuse_texture to point at the baked font atlas instead of
  // an on-disk texture.
  Material *material_ = nullptr;

  bool valid_ = false;
};
