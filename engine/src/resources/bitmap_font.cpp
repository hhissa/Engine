#include "bitmap_font.h"
#include "../core/logger.h"

#define STB_TRUETYPE_IMPLEMENTATION
#include "../vendor/stb_truetype.h"

#include <fstream>

namespace {
std::string font_path(std::string_view name) {
  return "assets/fonts/" + std::string(name) + ".ttf";
}
} // namespace

BitmapFont::BitmapFont(VulkanContext &context, std::string_view name,
                       f32 pixel_height, u32 atlas_size)
    : atlas_size_(atlas_size) {
  std::ifstream file(font_path(name), std::ios::binary | std::ios::ate);
  if (!file.is_open()) {
    KERROR("Failed to open font file for '{}'.", name);
    return;
  }

  std::streamsize size = file.tellg();
  file.seekg(0);
  std::vector<u8> ttf_data(static_cast<size_t>(size));
  if (!file.read(reinterpret_cast<char *>(ttf_data.data()), size)) {
    KERROR("Failed to read font file for '{}'.", name);
    return;
  }

  std::vector<u8> coverage(static_cast<size_t>(atlas_size_) * atlas_size_, 0);
  baked_chars_.resize(kNumChars);

  int result = stbtt_BakeFontBitmap(
      ttf_data.data(), 0, pixel_height, coverage.data(),
      static_cast<int>(atlas_size_), static_cast<int>(atlas_size_),
      kFirstChar, kNumChars, baked_chars_.data());
  if (result <= 0) {
    KERROR("Failed to bake font '{}': atlas too small for {} characters at "
          "{} px (stbtt_BakeFontBitmap returned {}).",
          name, kNumChars, pixel_height, result);
    return;
  }

  // stb_truetype bakes a single-channel coverage bitmap; expand to RGBA
  // (white RGB, coverage as alpha) so it can go through VulkanTexture
  // unmodified and be tinted any colour by the text shader multiplying by
  // its own colour uniform.
  std::vector<u8> rgba(coverage.size() * 4);
  for (size_t i = 0; i < coverage.size(); ++i) {
    rgba[i * 4 + 0] = 255;
    rgba[i * 4 + 1] = 255;
    rgba[i * 4 + 2] = 255;
    rgba[i * 4 + 3] = coverage[i];
  }

  atlas_texture_.emplace(context, atlas_size_, atlas_size_, 4, rgba);
  if (!atlas_texture_->is_valid()) {
    KERROR("Failed to create font atlas texture for '{}'.", name);
    return;
  }

  valid_ = true;
}

std::vector<BitmapFont::GlyphQuad> BitmapFont::layout(std::string_view text,
                                                      glm::vec2 origin) const {
  std::vector<GlyphQuad> quads;
  quads.reserve(text.size());

  f32 x = origin.x;
  f32 y = origin.y;

  for (char c : text) {
    if (c < kFirstChar || c >= kFirstChar + kNumChars) {
      continue; // Unsupported character (e.g. a control code) -- skipped.
    }

    stbtt_aligned_quad q{};
    stbtt_GetBakedQuad(baked_chars_.data(), static_cast<int>(atlas_size_),
                      static_cast<int>(atlas_size_), c - kFirstChar, &x, &y,
                      &q, 1);

    GlyphQuad quad{};
    quad.x0 = q.x0;
    quad.y0 = q.y0;
    quad.x1 = q.x1;
    quad.y1 = q.y1;
    quad.s0 = q.s0;
    quad.t0 = q.t0;
    quad.s1 = q.s1;
    quad.t1 = q.t1;
    quads.push_back(quad);
  }

  return quads;
}
