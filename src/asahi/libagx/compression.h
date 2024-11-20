/*
 * Copyright 2024 Valve Corporation
 * SPDX-License-Identifier: MIT
 */
#include "agx_pack.h"
#include "libagx.h"

#pragma once

struct libagx_decompress_images {
   struct agx_texture_packed compressed;
   struct agx_pbe_packed uncompressed;
};
AGX_STATIC_ASSERT(sizeof(struct libagx_decompress_images) == 48);
