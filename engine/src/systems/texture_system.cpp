#include "texture_system.h"
#include "../core/logger.h"
#include "../resources/image_loader.h"

namespace {
constexpr u32 kDefaultTextureSize = 256;
constexpr u32 kDefaultTextureTile = 32;

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
    if (auto loaded = load_image(texture_path(name))) {
      entry.texture.emplace(*context_, loaded->width, loaded->height,
                            loaded->channel_count, loaded->pixels);
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
