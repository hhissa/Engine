#include "texture_system.h"
#include "../core/logger.h"
#include "../resources/image_loader.h"

namespace {
constexpr u32 kDefaultTextureSize = 256;
constexpr u32 kDefaultTextureTile = 32;
// Small -- flat_texture_ is spatially uniform by construction, so it needs
// no real resolution at all, unlike the checkerboard default. 16 rather
// than something even smaller is now only habit: vulkan_image.cpp used to
// hardcode every VulkanImage to 4 mip levels, and Vulkan requires
// mipLevels <= floor(log2(max(width,height)))+1, so a 4x4 failed image
// creation outright. It creates single-mip images now, so any size works.
constexpr u32 kFlatTextureSize = 16;

std::string texture_path(std::string_view name) {
  return "assets/textures/" + std::string(name) + ".png";
}
} // namespace

TextureSystem::TextureSystem(VulkanContext &context) : context_(&context) {
  default_texture_.emplace(
      *context_, kDefaultTextureSize, kDefaultTextureSize, 4,
      generate_checkerboard_pixels(kDefaultTextureSize, kDefaultTextureTile));
  if (!default_texture_->is_valid()) {
    KERROR("Failed to create the default texture.");
  }

  flat_texture_.emplace(*context_, kFlatTextureSize, kFlatTextureSize, 4,
                       generate_flat_pixels(kFlatTextureSize, 255, 255, 255, 255));
  if (!flat_texture_->is_valid()) {
    KERROR("Failed to create the flat texture.");
  }
}

VulkanTexture &TextureSystem::acquire(std::string_view name,
                                      bool auto_release) {
  std::string key(name);
  Entry &entry = textures_.try_emplace(key).first->second;

  if (entry.reference_count == 0) {
    entry.auto_release = auto_release;
  }
  ++entry.reference_count;

  if (!entry.texture) {
    if (const LoadedImage *image = decoded_image(key)) {
      entry.texture.emplace(*context_, image->width, image->height,
                            image->channel_count, image->pixels);
    }
    if (!entry.texture || !entry.texture->is_valid()) {
      KWARN("Texture '{}' failed to load; using the default texture in its "
           "place.",
           name);
      entry.texture.reset();
    } else {
      KTRACE("Texture '{}' loaded, reference count now {}.", name,
            entry.reference_count);
    }
  }

  return entry.texture ? *entry.texture : *default_texture_;
}

const LoadedImage *TextureSystem::decoded_image(const std::string &name) {
  if (auto it = decoded_.find(name); it != decoded_.end()) {
    return &it->second;
  }
  std::optional<LoadedImage> loaded = load_image(texture_path(name));
  if (!loaded) {
    return nullptr;
  }
  const u64 bytes = loaded->pixels.size();
  // Make room first, oldest out. (An image bigger than the whole budget
  // empties the cache and is kept anyway -- it has to live somewhere until
  // its upload, and is evicted by the next decode.)
  while (!decoded_order_.empty() &&
         decoded_bytes_ + bytes > kDecodedCacheBudgetBytes) {
    auto oldest = decoded_.find(decoded_order_.front());
    decoded_bytes_ -= oldest->second.pixels.size();
    decoded_.erase(oldest);
    decoded_order_.pop_front();
  }
  decoded_bytes_ += bytes;
  decoded_order_.push_back(name);
  return &decoded_.emplace(name, std::move(*loaded)).first->second;
}

void TextureSystem::release(std::string_view name) {
  std::string key(name);
  auto it = textures_.find(key);
  if (it == textures_.end() || it->second.reference_count == 0) {
    KWARN("TextureSystem::release called for a texture with no outstanding "
         "references: '{}'.",
         name);
    return;
  }

  Entry &entry = it->second;
  --entry.reference_count;
  if (entry.reference_count == 0 && entry.auto_release) {
    textures_.erase(it);
  }
}
