/*
 * Copyright 2012 Advanced Micro Devices, Inc.
 * Copyright 2025 Valve Corporation
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef AC_CMDBUF_SDMA_H
#define AC_CMDBUF_SDMA_H

struct ac_cmdbuf;

#ifdef __cplusplus
extern "C" {
#endif

void ac_emit_sdma_nop(struct ac_cmdbuf *cs);

void ac_emit_sdma_write_timestamp(struct ac_cmdbuf *cs, uint64_t va);

void ac_emit_sdma_fence(struct ac_cmdbuf *cs, uint64_t va, uint32_t fence);

void ac_emit_sdma_wait_mem(struct ac_cmdbuf *cs, uint32_t op, uint64_t va, uint32_t ref, uint32_t mask);

void ac_emit_sdma_write_data_head(struct ac_cmdbuf *cs, uint64_t va, uint32_t count);

uint64_t
ac_emit_sdma_constant_fill(struct ac_cmdbuf *cs, enum sdma_version sdma_ip_version,
                           uint64_t va, uint64_t size, uint32_t value);

#ifdef __cplusplus
}
#endif

#endif
