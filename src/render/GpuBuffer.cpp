#include "GpuBuffer.hpp"
#include <stdexcept>

void GpuBuffer::create(VmaAllocator       allocator,
                       VkDeviceSize       byteSize,
                       VkBufferUsageFlags usage,
                       VmaMemoryUsage     memUsage) {
  size = byteSize;

  VkBufferCreateInfo bci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
  bci.size        = byteSize;
  bci.usage       = usage;
  bci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

  VmaAllocationCreateInfo aci{};
  aci.usage = memUsage;

  if (vmaCreateBuffer(allocator, &bci, &aci, &buffer, &alloc, nullptr) != VK_SUCCESS)
    throw std::runtime_error("GpuBuffer: vmaCreateBuffer failed");
}

void* GpuBuffer::createMapped(VmaAllocator       allocator,
                               VkDeviceSize       byteSize,
                               VkBufferUsageFlags usage) {
  size = byteSize;

  VkBufferCreateInfo bci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
  bci.size        = byteSize;
  bci.usage       = usage;
  bci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

  VmaAllocationCreateInfo aci{};
  aci.usage = VMA_MEMORY_USAGE_CPU_TO_GPU;
  aci.flags = VMA_ALLOCATION_CREATE_MAPPED_BIT;

  VmaAllocationInfo info{};
  if (vmaCreateBuffer(allocator, &bci, &aci, &buffer, &alloc, &info) != VK_SUCCESS)
    throw std::runtime_error("GpuBuffer: vmaCreateBuffer (mapped) failed");

  return info.pMappedData;
}

void GpuBuffer::destroy(VmaAllocator allocator) {
  if (buffer != VK_NULL_HANDLE) {
    vmaDestroyBuffer(allocator, buffer, alloc);
    buffer = VK_NULL_HANDLE;
    alloc  = {};
    size   = 0;
  }
}
