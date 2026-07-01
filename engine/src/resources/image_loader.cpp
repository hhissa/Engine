#include "image_loader.h"
#include "../core/logger.h"

// This is the one translation unit stb_image's implementation is compiled
// into (its usage docs require exactly one #define STB_IMAGE_IMPLEMENTATION
// site across the whole program).
#define STB_IMAGE_IMPLEMENTATION
#include "../vendor/stb_image.h"

std::optional<LoadedImage> load_image(std::string_view path) {
  constexpr int kRequiredChannels = 4;

  // Flips rows on load; matches upstream kohi's convention. Doesn't affect
  // correctness for triplanar world-space sampling (no authored UVs to
  // mismatch), just which way the texture appears mirrored.
  stbi_set_flip_vertically_on_load(true);

  std::string path_str(path);
  int width = 0;
  int height = 0;
  int source_channels = 0;
  stbi_uc *data = stbi_load(path_str.c_str(), &width, &height,
                           &source_channels, kRequiredChannels);
  if (!data) {
    const char *reason = stbi_failure_reason();
    KERROR("Failed to load image '{}': {}", path,
          reason ? reason : "unknown reason");
    return std::nullopt;
  }

  LoadedImage image;
  image.width = static_cast<u32>(width);
  image.height = static_cast<u32>(height);
  image.channel_count = kRequiredChannels;
  image.pixels.assign(
      data, data + (static_cast<size_t>(width) * height * kRequiredChannels));

  stbi_image_free(data);
  return image;
}
