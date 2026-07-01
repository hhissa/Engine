#pragma once
#include "vulkan_types.inl"

// A generic Vulkan buffer: a VkBuffer plus its backing VkDeviceMemory.
// Usable as a vertex/index/uniform/storage/staging buffer — the specific
// role is entirely determined by the `usage` flags passed at construction;
// nothing here assumes rasterization vs. compute.
class VulkanBuffer {
public:
  VulkanBuffer(VulkanContext &context, u64 size, VkBufferUsageFlags usage,
              VkMemoryPropertyFlags memory_property_flags,
              bool bind_on_create = true);
  ~VulkanBuffer();

  VulkanBuffer(const VulkanBuffer &) = delete;
  VulkanBuffer &operator=(const VulkanBuffer &) = delete;
  VulkanBuffer(VulkanBuffer &&other) noexcept;
  VulkanBuffer &operator=(VulkanBuffer &&other) noexcept;

  bool is_valid() const noexcept {
    return handle_ != VK_NULL_HANDLE && memory_ != VK_NULL_HANDLE;
  }
  explicit operator bool() const noexcept { return is_valid(); }

  // Grows the buffer to new_size, copying its existing contents across via
  // a one-time command buffer submitted to queue/pool. Any pointer
  // previously returned by lock() is invalidated.
  bool resize(u64 new_size, VkQueue queue, VkCommandPool pool);

  void bind(u64 offset);

  void *lock(u64 offset, u64 size, VkMemoryMapFlags flags);
  void unlock();

  // Maps, copies size bytes from data in, then unmaps.
  void load_data(u64 offset, u64 size, VkMemoryMapFlags flags,
                const void *data);

  // Copies size bytes from this buffer into dest, using a one-time command
  // buffer submitted to queue/pool.
  void copy_to(VulkanBuffer &dest, u64 source_offset, u64 dest_offset,
              u64 size, VkQueue queue, VkCommandPool pool);

  VkBuffer handle() const noexcept { return handle_; }
  u64 size() const noexcept { return total_size_; }

private:
  void copy_raw(VkBuffer source, u64 source_offset, VkBuffer dest,
               u64 dest_offset, u64 size, VkQueue queue,
               VkCommandPool pool) const;

  VulkanContext *context_;
  u64 total_size_ = 0;
  VkBuffer handle_ = VK_NULL_HANDLE;
  VkBufferUsageFlags usage_ = 0;
  VkDeviceMemory memory_ = VK_NULL_HANDLE;
  i32 memory_index_ = -1;
  VkMemoryPropertyFlags memory_property_flags_ = 0;
};
