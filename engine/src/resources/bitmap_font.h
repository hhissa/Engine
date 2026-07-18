#pragma once
#include "../renderer/vulkan/vulkan_texture.h"
#include "../vendor/stb_truetype.h"

#include <glm/glm.hpp>
#include <optional>
#include <string_view>
#include <vector>

// A single fixed-size bitmap font: ASCII 32..126 baked once from a .ttf
// file (assets/fonts/<name>.ttf) into a texture atlas via stb_truetype, at
// construction. There's no live font system managing multiple fonts/sizes
// yet -- this is the smallest useful unit, analogous to how
// TextureSystem's default_texture() is one procedurally-generated texture
// with no registry wrapped around it.
class BitmapFont {
public:
  BitmapFont(VulkanContext &context, std::string_view name, f32 pixel_height,
            u32 atlas_size = 512);

  bool is_valid() const noexcept { return valid_; }
  explicit operator bool() const noexcept { return valid_; }

  VulkanTexture &atlas() noexcept { return *atlas_texture_; }

  // One quad (screen-space position + atlas UV rect) per glyph in text,
  // laid out left-to-right starting at origin -- baseline convention: y
  // increases downward, matching the rest of this engine's UI screen space
  // (see VulkanUIShader). Characters outside the baked ASCII range (e.g.
  // control codes) are skipped.
  struct GlyphQuad {
    f32 x0, y0, x1, y1; // screen-space position, pixels
    f32 s0, t0, s1, t1; // atlas UV rect
  };
  std::vector<GlyphQuad> layout(std::string_view text, glm::vec2 origin) const;

private:
  static constexpr int kFirstChar = 32;  // ' '
  static constexpr int kNumChars = 95;   // ' '..'~' inclusive

  u32 atlas_size_;
  std::vector<stbtt_bakedchar> baked_chars_;
  std::optional<VulkanTexture> atlas_texture_;
  bool valid_ = false;
};
