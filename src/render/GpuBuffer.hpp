#pragma once
#include <vk_mem_alloc.h>
#include <vulkan/vulkan.h>

// RAII wrapper for a VkBuffer + VmaAllocation pair.
// Not copyable; movable.
struct GpuBuffer {
  VkBuffer      buffer{VK_NULL_HANDLE};
  VmaAllocation alloc{};
  VkDeviceSize  size{0};

  GpuBuffer() = default;
  GpuBuffer(const GpuBuffer&) = delete;
  GpuBuffer& operator=(const GpuBuffer&) = delete;
  GpuBuffer(GpuBuffer&& o) noexcept
      : buffer(o.buffer), alloc(o.alloc), size(o.size) {
    o.buffer = VK_NULL_HANDLE;
    o.alloc  = {};
    o.size   = 0;
  }

  bool valid() const { return buffer != VK_NULL_HANDLE; }

  void create(VmaAllocator allocator,
              VkDeviceSize            byteSize,
              VkBufferUsageFlags      usage,
              VmaMemoryUsage          memUsage);

  // Create and persistently map (for CPU→GPU upload buffers).
  void* createMapped(VmaAllocator allocator,
                     VkDeviceSize       byteSize,
                     VkBufferUsageFlags usage);

  void destroy(VmaAllocator allocator);
};
