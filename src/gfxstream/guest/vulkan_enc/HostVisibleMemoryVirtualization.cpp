/*
 * Copyright 2018 Google
 * SPDX-License-Identifier: MIT
 */
#include "HostVisibleMemoryVirtualization.h"

#include <set>

#include "ResourceTracker.h"
#include "Resources.h"
#include "VkEncoder.h"

namespace gfxstream {
namespace vk {

CoherentMemory::CoherentMemory(VirtGpuResourceMappingPtr blobMapping, uint64_t size,
                               VkDevice device, VkDeviceMemory memory)
    : mSize(size), mBlobMapping(blobMapping), mDevice(device), mMemory(memory) {
    mHeap = u_mmInit(0, kHostVisibleHeapSize);
    mBaseAddr = blobMapping->asRawPtr();
}

#if defined(__ANDROID__)
CoherentMemory::CoherentMemory(GoldfishAddressSpaceBlockPtr block, uint64_t gpuAddr, uint64_t size,
                               VkDevice device, VkDeviceMemory memory)
    : mSize(size), mBlock(block), mDevice(device), mMemory(memory) {
    mHeap = u_mmInit(0, kHostVisibleHeapSize);
    mBaseAddr = (uint8_t*)block->mmap(gpuAddr);
}
#endif  // defined(__ANDROID__)

CoherentMemory::~CoherentMemory() {
    ResourceTracker::getThreadLocalEncoder()->vkFreeMemorySyncGOOGLE(mDevice, mMemory, nullptr,
                                                                     false);
    u_mmDestroy(mHeap);
}

VkDeviceMemory CoherentMemory::getDeviceMemory() const { return mMemory; }

bool CoherentMemory::subAllocate(uint64_t size, uint8_t** ptr, uint64_t& offset) {
    auto block = u_mmAllocMem(mHeap, (int)size, 0, 0);
    if (!block) return false;

    *ptr = mBaseAddr + block->ofs;
    return true;
}

bool CoherentMemory::release(uint8_t* ptr) {
    int offset = ptr - mBaseAddr;
    auto block = u_mmFindBlock(mHeap, offset);
    if (block) {
        u_mmFreeMem(block);
        return true;
    }

    return false;
}

}  // namespace vk
}  // namespace gfxstream
