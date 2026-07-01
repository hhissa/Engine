#pragma once
#include "../../camera.h"
#include "../vulkan_buffer.h"
#include "../vulkan_compute_pipeline.h"
#include "../vulkan_shader_module.h"
#include "../vulkan_texture.h"
#include "../vulkan_types.inl"

#include <optional>

class VulkanCommandBuffer;

// Two-pass sparse-voxel raymarching:
//
//  Pass 1 ("voxelize"): evaluates the analytic SDF once and bakes it into a
//  sparse voxel field — a coarse indirection grid where each cell either
//  points at an allocated "brick" of fine SDF samples (only where the
//  surface actually passes through that cell) or is empty. Runs once, at
//  construction, since the scene is currently static.
//
//  Pass 2 ("render", repeating): every frame, marches a ray per pixel
//  against the baked field instead of evaluating the SDF directly — for
//  cells with no brick it skips straight across (the actual point of
//  sparsity: empty space costs one lookup, not per-voxel sampling), and for
//  bricked cells it trilinearly samples the fine distance values.
class VulkanRaymarchShader {
public:
  explicit VulkanRaymarchShader(VulkanContext &context);
  ~VulkanRaymarchShader();

  VulkanRaymarchShader(const VulkanRaymarchShader &) = delete;
  VulkanRaymarchShader &operator=(const VulkanRaymarchShader &) = delete;
  VulkanRaymarchShader(VulkanRaymarchShader &&) = delete;
  VulkanRaymarchShader &operator=(VulkanRaymarchShader &&) = delete;

  // True only if both shader modules compiled, both pipelines were created,
  // and the field buffers were allocated.
  bool is_valid() const noexcept { return valid_; }
  explicit operator bool() const noexcept { return valid_; }

  // Dispatches the render (pass 2) compute shader into the internal storage
  // image (one invocation per pixel, sized width x height), then copies the
  // result into swapchain_image. Must be called outside an active render
  // pass instance — barriers and vkCmdDispatch are not valid inside one.
  // swapchain_image's prior contents are not preserved: the copy overwrites
  // it completely.
  //
  // camera's position and basis (forward/right/up) and the model offset are
  // sent as push constants (see PushConstants in vulkan_raymarch_shader.cpp)
  // rather than a UBO: they're recorded directly into this call's command
  // buffer, so unlike a separate UBO buffer there's no risk of overwriting a
  // value a still-in-flight previous frame might still be reading — no
  // per-image buffering needed.
  void render_to(VulkanCommandBuffer &command_buffer, VkImage swapchain_image,
                 u32 width, u32 height, f32 delta_time, const Camera &camera);

  // Recreates the output storage image at the new size and re-points the
  // render descriptor set's binding 0 at it. Must be called whenever the
  // framebuffer resizes — the output image is sized to match it, unlike
  // the sparse voxel field (which represents world-space scene geometry
  // and is independent of screen resolution, so it never needs this). Call
  // while the device is idle (VulkanRendererBackend::recreate_swapchain()
  // already waits before calling this).
  void on_resized(u32 width, u32 height);

private:
  // Dispatches the voxelize (pass 1) compute shader once, via a one-time
  // command buffer, and waits for it to complete before returning — so by
  // the time the constructor finishes, the field is fully baked.
  void voxelize();

  static void transition_image(VkCommandBuffer cmd, VkImage image,
                               VkImageLayout old_layout,
                               VkImageLayout new_layout,
                               VkAccessFlags src_access,
                               VkAccessFlags dst_access,
                               VkPipelineStageFlags src_stage,
                               VkPipelineStageFlags dst_stage);

  VulkanContext *context_;

  // Pass 1: SDF -> sparse voxel field. Run once, at construction.
  VulkanShaderModule voxelize_stage_;
  VkDescriptorSetLayout voxelize_set_layout_ = VK_NULL_HANDLE;
  VkDescriptorSet voxelize_set_ = VK_NULL_HANDLE;
  std::optional<VulkanComputePipeline> voxelize_pipeline_;

  // The sparse voxel field: a coarse indirection grid of brick indices
  // (kCoarseDim^3 i32s, -1 = no brick) pointing into a pool of fixed-size
  // bricks (kMaxBricks * kBrickVoxelCount f32 distances), allocated by an
  // atomic counter during voxelize(). See vulkan_raymarch_shader.cpp for the
  // dimension constants — they must match the ones hardcoded in the two
  // .comp.glsl shaders exactly, since there's no shared config between them.
  std::optional<VulkanBuffer> indirection_buffer_;
  std::optional<VulkanBuffer> brick_pool_buffer_;
  std::optional<VulkanBuffer> brick_counter_buffer_;

  // A procedurally-generated checkerboard, triplanar-mapped onto the
  // sphere's surface in the render shader (no UV coordinates exist for an
  // implicit surface, so it's sampled via 3 axis-aligned projections
  // blended by surface normal instead of a single 2D unwrap).
  std::optional<VulkanTexture> surface_texture_;

  // Pass 2: repeating — marches rays against the baked field each frame.
  // A single descriptor set: out_image/indirection/brick_pool/surface_tex
  // never change, so (unlike the camera, which is a push constant) there's
  // no per-frame write to guard against and no need for one set per
  // swapchain image.
  VulkanShaderModule render_stage_;
  VkDescriptorSetLayout render_set_layout_ = VK_NULL_HANDLE;
  VkDescriptorSet render_set_ = VK_NULL_HANDLE;
  std::optional<VulkanComputePipeline> render_pipeline_;

  // Simple animation clock driving the model orbit in render_to() (the
  // camera itself is externally driven now — see the Camera parameter on
  // render_to()).
  f32 time_seconds_ = 0.0f;

  // Backs both descriptor sets above.
  VkDescriptorPool descriptor_pool_ = VK_NULL_HANDLE;

  VulkanImage output_image_{};

  bool valid_ = false;
};
