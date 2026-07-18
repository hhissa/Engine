#pragma once
#include "../renderer/vulkan/vulkan_texture.h"
#include "../renderer/vulkan/vulkan_types.inl"

#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>

// Loads and caches named textures from assets/textures/<name>.png, so
// multiple consumers requesting the same asset share one GPU upload instead
// of reloading and re-uploading it independently -- ported from kohi's
// texture_system, minus its fixed-size array + hand-rolled hashtable (an
// std::unordered_map already gives reference-stable, dynamically-sized
// storage for free, so there's no need for a manual slot allocator here).
//
// Every named texture is reference-counted: acquire() increments the count
// (loading from disk the first time a given name is requested), release()
// decrements it, and the underlying VulkanTexture is destroyed once the
// count reaches zero *and* the texture was acquired with auto_release=true.
class TextureSystem {
public:
  explicit TextureSystem(VulkanContext &context);
  ~TextureSystem() = default;

  TextureSystem(const TextureSystem &) = delete;
  TextureSystem &operator=(const TextureSystem &) = delete;

  // Returns a texture, incrementing its reference count. Loads
  // assets/textures/<name>.png the first time a given name is requested; if
  // that fails (missing file, decode error), falls back to
  // default_texture() instead of returning null, so callers can always
  // dereference the result.
  VulkanTexture &acquire(std::string_view name, bool auto_release);

  // Decrements name's reference count. Once it reaches zero, the texture is
  // destroyed if it was acquired with auto_release=true (kept resident
  // otherwise). Every acquire() call must be paired with exactly one
  // release() call for the same name.
  void release(std::string_view name);

  // A procedurally-generated checkerboard, always resident for the
  // lifetime of the system -- the fallback returned by acquire() on load
  // failure, and available directly for anything that just wants *a*
  // texture without a named asset.
  VulkanTexture &default_texture() noexcept { return *default_texture_; }

private:
  struct Entry {
    std::optional<VulkanTexture> texture;
    u32 reference_count = 0;
    bool auto_release = false;
  };

  VulkanContext *context_;
  std::unordered_map<std::string, Entry> textures_;
  std::optional<VulkanTexture> default_texture_;
};
