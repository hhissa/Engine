#include "vulkan_buffer.h"
#include "../../core/logger.h"
#include "vulkan_commandbuffer.h"

#include <cstring>
#include <utility>

VulkanBuffer::VulkanBuffer(VulkanContext &context, u64 size,
                          VkBufferUsageFlags usage,
                          VkMemoryPropertyFlags memory_property_flags,
                          bool bind_on_create)
    : context_(&context), total_size_(size), usage_(usage),
      memory_property_flags_(memory_property_flags) {
  VkBufferCreateInfo buffer_info{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
  buffer_info.size = size;
  buffer_info.usage = usage;
  buffer_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE; // Only used in one queue.

  VK_CHECK(vkCreateBuffer(context_->device.logical_device, &buffer_info,
                          context_->allocator, &handle_));

  VkMemoryRequirements requirements;
  vkGetBufferMemoryRequirements(context_->device.logical_device, handle_,
                                &requirements);
  memory_index_ = context_->find_memory_index(
      *context_, requirements.memoryTypeBits, memory_property_flags_);
  if (memory_index_ == -1) {
    KERROR("Unable to create vulkan buffer because the required memory type "
          "index was not found.");
    return;
  }

  VkMemoryAllocateInfo allocate_info{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
  allocate_info.allocationSize = requirements.size;
  allocate_info.memoryTypeIndex = static_cast<u32>(memory_index_);

  VkResult result = vkAllocateMemory(context_->device.logical_device,
                                     &allocate_info, context_->allocator,
                                     &memory_);
  if (result != VK_SUCCESS) {
    KERROR("Unable to create vulkan buffer because the required memory "
          "allocation failed. Error: {}",
          static_cast<i32>(result));
    return;
  }

  if (bind_on_create) {
    bind(0);
  }
}

VulkanBuffer::~VulkanBuffer() {
  if (memory_ != VK_NULL_HANDLE) {
    vkFreeMemory(context_->device.logical_device, memory_, context_->allocator);
    memory_ = VK_NULL_HANDLE;
  }
  if (handle_ != VK_NULL_HANDLE) {
    vkDestroyBuffer(context_->device.logical_device, handle_,
                    context_->allocator);
    handle_ = VK_NULL_HANDLE;
  }
}

VulkanBuffer::VulkanBuffer(VulkanBuffer &&other) noexcept
    : context_(other.context_), total_size_(other.total_size_),
      handle_(other.handle_), usage_(other.usage_), memory_(other.memory_),
      memory_index_(other.memory_index_),
      memory_property_flags_(other.memory_property_flags_) {
  other.handle_ = VK_NULL_HANDLE;
  other.memory_ = VK_NULL_HANDLE;
  other.total_size_ = 0;
  other.memory_index_ = -1;
}

VulkanBuffer &VulkanBuffer::operator=(VulkanBuffer &&other) noexcept {
  if (this != &other) {
    if (memory_ != VK_NULL_HANDLE) {
      vkFreeMemory(context_->device.logical_device, memory_,
                  context_->allocator);
    }
    if (handle_ != VK_NULL_HANDLE) {
      vkDestroyBuffer(context_->device.logical_device, handle_,
                      context_->allocator);
    }

    context_ = other.context_;
    total_size_ = other.total_size_;
    handle_ = other.handle_;
    usage_ = other.usage_;
    memory_ = other.memory_;
    memory_index_ = other.memory_index_;
    memory_property_flags_ = other.memory_property_flags_;

    other.handle_ = VK_NULL_HANDLE;
    other.memory_ = VK_NULL_HANDLE;
    other.total_size_ = 0;
    other.memory_index_ = -1;
  }
  return *this;
}

bool VulkanBuffer::resize(u64 new_size, VkQueue queue, VkCommandPool pool) {
  VkBufferCreateInfo buffer_info{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
  buffer_info.size = new_size;
  buffer_info.usage = usage_;
  buffer_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

  VkBuffer new_handle;
  VK_CHECK(vkCreateBuffer(context_->device.logical_device, &buffer_info,
                          context_->allocator, &new_handle));

  VkMemoryRequirements requirements;
  vkGetBufferMemoryRequirements(context_->device.logical_device, new_handle,
                                &requirements);

  VkMemoryAllocateInfo allocate_info{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
  allocate_info.allocationSize = requirements.size;
  allocate_info.memoryTypeIndex = static_cast<u32>(memory_index_);

  VkDeviceMemory new_memory;
  VkResult result = vkAllocateMemory(context_->device.logical_device,
                                     &allocate_info, context_->allocator,
                                     &new_memory);
  if (result != VK_SUCCESS) {
    KERROR("Unable to resize vulkan buffer because the required memory "
          "allocation failed. Error: {}",
          static_cast<i32>(result));
    vkDestroyBuffer(context_->device.logical_device, new_handle,
                    context_->allocator);
    return false;
  }

  VK_CHECK(
      vkBindBufferMemory(context_->device.logical_device, new_handle, new_memory, 0));

  // Copy the existing contents into the new buffer.
  copy_raw(handle_, 0, new_handle, 0, total_size_, queue, pool);

  // Make sure anything potentially using the old buffer is finished.
  vkDeviceWaitIdle(context_->device.logical_device);

  if (memory_ != VK_NULL_HANDLE) {
    vkFreeMemory(context_->device.logical_device, memory_, context_->allocator);
  }
  if (handle_ != VK_NULL_HANDLE) {
    vkDestroyBuffer(context_->device.logical_device, handle_,
                    context_->allocator);
  }

  total_size_ = new_size;
  memory_ = new_memory;
  handle_ = new_handle;

  return true;
}

void VulkanBuffer::bind(u64 offset) {
  VK_CHECK(vkBindBufferMemory(context_->device.logical_device, handle_,
                              memory_, offset));
}

void *VulkanBuffer::lock(u64 offset, u64 size, VkMemoryMapFlags flags) {
  void *data;
  VK_CHECK(vkMapMemory(context_->device.logical_device, memory_, offset, size,
                       flags, &data));
  return data;
}

void VulkanBuffer::unlock() {
  vkUnmapMemory(context_->device.logical_device, memory_);
}

void VulkanBuffer::load_data(u64 offset, u64 size, VkMemoryMapFlags flags,
                            const void *data) {
  void *data_ptr = lock(offset, size, flags);
  std::memcpy(data_ptr, data, size);
  unlock();
}

void VulkanBuffer::copy_to(VulkanBuffer &dest, u64 source_offset,
                          u64 dest_offset, u64 size, VkQueue queue,
                          VkCommandPool pool) {
  copy_raw(handle_, source_offset, dest.handle_, dest_offset, size, queue,
          pool);
}

void VulkanBuffer::copy_raw(VkBuffer source, u64 source_offset, VkBuffer dest,
                           u64 dest_offset, u64 size, VkQueue queue,
                           VkCommandPool pool) const {
  vkQueueWaitIdle(queue);

  auto command_buffer =
      VulkanCommandBuffer::allocate_and_begin_single_use(*context_, pool);

  VkBufferCopy copy_region{};
  copy_region.srcOffset = source_offset;
  copy_region.dstOffset = dest_offset;
  copy_region.size = size;

  vkCmdCopyBuffer(command_buffer->handle(), source, dest, 1, &copy_region);

  VulkanCommandBuffer::end_single_use(*context_, pool, std::move(command_buffer),
                                      queue);
}
