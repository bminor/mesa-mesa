/* Copyright © 2025 Intel Corporation
 * SPDX-License-Identifier: MIT
 */

#include "anv_slab_bo.h"

struct anv_bo *
anv_slab_bo_alloc(struct anv_device *device, const char *name, uint64_t requested_size,
                  uint32_t alignment, enum anv_bo_alloc_flags alloc_flags)
{
   return NULL;
}

void
anv_slab_bo_free(struct anv_device *device, struct anv_bo *bo)
{
}

bool
anv_slab_bo_init(struct anv_device *device)
{
   return true;
}

void
anv_slab_bo_deinit(struct anv_device *device)
{
}
