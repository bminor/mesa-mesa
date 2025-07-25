/*
 * Copyright © 2022 Collabora Ltd. and Red Hat Inc.
 * SPDX-License-Identifier: MIT
 */
#ifndef NVK_BUFFER_H
#define NVK_BUFFER_H 1

#include "nvk_private.h"
#include "nvk_device_memory.h"

#include "vk_buffer.h"

struct nvk_device_memory;
struct nvk_physical_device;
struct nvk_queue;
struct nvkmd_va;

struct nvk_buffer {
   struct vk_buffer vk;

   /** Reserved VA for sparse buffers, NULL otherwise. */
   struct nvkmd_va *va;
};

VK_DEFINE_NONDISP_HANDLE_CASTS(nvk_buffer, vk.base, VkBuffer,
                               VK_OBJECT_TYPE_BUFFER)

static inline struct nvk_addr_range
nvk_buffer_addr_range(const struct nvk_buffer *buffer,
                      uint64_t offset, uint64_t range)
{
   if (buffer == NULL)
      return (struct nvk_addr_range) { .range = 0 };

   return (struct nvk_addr_range) {
      .addr = vk_buffer_address(&buffer->vk, offset),
      .range = vk_buffer_range(&buffer->vk, offset, range),
   };
}

VkResult nvk_queue_buffer_bind(struct nvk_queue *queue,
                               const VkSparseBufferMemoryBindInfo *bind_info);

#endif
