/*
 * Copyright 2024 Alyssa Rosenzweig
 * Copyright 2024 Valve Corporation
 * Copyright 2022 Collabora Ltd. and Red Hat Inc.
 * SPDX-License-Identifier: MIT
 */
#include "libagx.h"

#pragma once

struct libagx_xfb_counter_copy {
   GLOBAL(uint32_t) dest[4];
   GLOBAL(uint32_t) src[4];
};

static inline uint32_t
libagx_cs_invocations(uint32_t local_size_threads, uint32_t x, uint32_t y,
                      uint32_t z)
{
   return local_size_threads * x * y * z;
}

struct libagx_imm_write {
   GLOBAL(uint32_t) address;
   uint32_t value;
};
