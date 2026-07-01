#pragma once
#include "vulkan_types.inl"

#include <cstdint>
#include <vector>

// A sampled 2D texture: a VulkanImage plus the VkSampler needed to read it
// with filtering/wrapping in a shader (unlike the raymarch storage image,
// which is read/written directly via imageLoad/imageStore with no sampler
// at all).
//
// Uploads pixel data via a staging buffer, exactly like VulkanRaymarchShader
// voxelizes its field: a one-time command buffer that submits and waits
// (VulkanCommandBuffer::allocate_and_begin_single_use/end_single_use)
// rather than needing frame-to-frame synchronization, since this all
// happens once at construction.
class VulkanTexture {
public:
  // pixels must contain width * height * channel_count bytes (8 bits per
  // channel, matching VK_FORMAT_R8G8B8A8_UNORM -- channel_count < 4 is
  // padded to RGBA with alpha=255 before upload).
  VulkanTexture(VulkanContext &context, u32 width, u32 height,
               u32 channel_count, const std::vector<u8> &pixels);
  ~VulkanTexture();

  VulkanTexture(const VulkanTexture &) = delete;
  VulkanTexture &operator=(const VulkanTexture &) = delete;
  VulkanTexture(VulkanTexture &&) = delete;
  VulkanTexture &operator=(VulkanTexture &&) = delete;

  bool is_valid() const noexcept {
    return image_.view != VK_NULL_HANDLE && sampler_ != VK_NULL_HANDLE;
  }
  explicit operator bool() const noexcept { return is_valid(); }

  VkImageView view() const noexcept { return image_.view; }
  VkSampler sampler() const noexcept { return sampler_; }

private:
  VulkanContext *context_;
  VulkanImage image_{};
  VkSampler sampler_ = VK_NULL_HANDLE;
};

// Generates a size x size RGBA checkerboard (alternating white/black
// squares, tile_size pixels per square) in code, matching upstream's
// approach of avoiding an asset/file-loading dependency for the first
// texture. Useful as a placeholder for anything that needs *a* texture
// before real asset loading exists.
std::vector<u8> generate_checkerboard_pixels(u32 size, u32 tile_size);
