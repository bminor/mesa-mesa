/*
 * Copyright © 2016 Red Hat.
 * Copyright © 2016 Bas Nieuwenhuizen
 *
 * based in part on anv driver which is:
 * Copyright © 2015 Intel Corporation
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef RADV_BUFFER_VIEW_H
#define RADV_BUFFER_VIEW_H

#include "vk_buffer_view.h"

struct radv_device;

struct radv_buffer_view {
   struct vk_buffer_view vk;
   struct radeon_winsys_bo *bo;
   uint32_t state[4];
};

VK_DEFINE_NONDISP_HANDLE_CASTS(radv_buffer_view, vk.base, VkBufferView, VK_OBJECT_TYPE_BUFFER_VIEW)

void radv_make_texel_buffer_descriptor(struct radv_device *device, uint64_t va, VkFormat vk_format, unsigned range,
                                       uint32_t *state);

#endif /* RADV_BUFFER_VIEW_H */
