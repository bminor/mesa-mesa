/*
 * Copyright 2024 Alyssa Rosenzweig
 * Copyright 2024 Valve Corporation
 * Copyright 2022 Collabora Ltd. and Red Hat Inc.
 * SPDX-License-Identifier: MIT
 */
#include "compiler/libcl/libcl.h"

#pragma once

struct libagx_xfb_counter_copy {
   GLOBAL(uint32_t) dest[4];
   GLOBAL(uint32_t) src[4];
};

struct libagx_imm_write {
   GLOBAL(uint32_t) address;
   uint32_t value;
};

#define LIBAGX_QUERY_UNAVAILABLE (uint64_t)((int64_t)-1)
