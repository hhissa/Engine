#pragma once
#include "../defines.h"

#include <optional>
#include <string_view>
#include <vector>

struct LoadedImage {
  u32 width = 0;
  u32 height = 0;
  u32 channel_count = 4; // always forced to RGBA on load, regardless of the
                        // source file's actual channel count
  std::vector<u8> pixels;
};

// Loads an image file (PNG, JPEG, and whatever else stb_image supports) from
// disk, decoding it to 4-channel (RGBA) pixel data ready to hand to
// VulkanTexture. Returns std::nullopt on failure (missing file, unsupported
// or corrupt format) — check the log for the reason.
std::optional<LoadedImage> load_image(std::string_view path);
