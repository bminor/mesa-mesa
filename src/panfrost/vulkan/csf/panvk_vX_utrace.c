/*
 * Copyright 2024 Google LLC
 * SPDX-License-Identifier: MIT
 */

#include "panvk_utrace.h"

#include "panvk_device.h"

void
panvk_per_arch(utrace_context_init)(struct panvk_device *dev)
{
   u_trace_context_init(&dev->utrace.utctx, NULL, sizeof(uint64_t), 0,
                        panvk_utrace_create_buffer, panvk_utrace_delete_buffer,
                        NULL, panvk_utrace_read_ts, NULL, NULL, NULL);
}

void
panvk_per_arch(utrace_context_fini)(struct panvk_device *dev)
{
   u_trace_context_fini(&dev->utrace.utctx);
}
