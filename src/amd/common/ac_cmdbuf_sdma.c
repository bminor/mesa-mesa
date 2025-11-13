/*
 * Copyright 2012 Advanced Micro Devices, Inc.
 * Copyright 2025 Valve Corporation
 *
 * SPDX-License-Identifier: MIT
 */

#include "ac_cmdbuf.h"
#include "ac_cmdbuf_sdma.h"
#include "sid.h"

#include "util/u_math.h"

void
ac_emit_sdma_nop(struct ac_cmdbuf *cs)
{
   /* SDMA NOP acts as a fence command and causes the SDMA engine to wait for pending copy operations. */
   ac_cmdbuf_begin(cs);
   ac_cmdbuf_emit(SDMA_PACKET(SDMA_OPCODE_NOP, 0, 0));
   ac_cmdbuf_end();
}

void
ac_emit_sdma_write_timestamp(struct ac_cmdbuf *cs, uint64_t va)
{
   ac_cmdbuf_begin(cs);
   ac_cmdbuf_emit(SDMA_PACKET(SDMA_OPCODE_TIMESTAMP, SDMA_TS_SUB_OPCODE_GET_GLOBAL_TIMESTAMP, 0));
   ac_cmdbuf_emit(va);
   ac_cmdbuf_emit(va >> 32);
   ac_cmdbuf_end();
}

void
ac_emit_sdma_fence(struct ac_cmdbuf *cs, uint64_t va, uint32_t fence)
{
   ac_cmdbuf_begin(cs);
   ac_cmdbuf_emit(SDMA_PACKET(SDMA_OPCODE_FENCE, 0, SDMA_FENCE_MTYPE_UC));
   ac_cmdbuf_emit(va);
   ac_cmdbuf_emit(va >> 32);
   ac_cmdbuf_emit(fence);
   ac_cmdbuf_end();
}

void
ac_emit_sdma_wait_mem(struct ac_cmdbuf *cs, uint32_t op, uint64_t va, uint32_t ref, uint32_t mask)
{
   ac_cmdbuf_begin(cs);
   ac_cmdbuf_emit(SDMA_PACKET(SDMA_OPCODE_POLL_REGMEM, 0, 0) | op << 28 | SDMA_POLL_MEM);
   ac_cmdbuf_emit(va);
   ac_cmdbuf_emit(va >> 32);
   ac_cmdbuf_emit(ref);
   ac_cmdbuf_emit(mask);
   ac_cmdbuf_emit(SDMA_POLL_INTERVAL_160_CLK | SDMA_POLL_RETRY_INDEFINITELY << 16);
   ac_cmdbuf_end();
}

void
ac_emit_sdma_write_data_head(struct ac_cmdbuf *cs, uint64_t va, uint32_t count)
{
   ac_cmdbuf_begin(cs);
   ac_cmdbuf_emit(SDMA_PACKET(SDMA_OPCODE_WRITE, SDMA_WRITE_SUB_OPCODE_LINEAR, 0));
   ac_cmdbuf_emit(va);
   ac_cmdbuf_emit(va >> 32);
   ac_cmdbuf_emit(count - 1);
   ac_cmdbuf_end();
}

uint64_t
ac_emit_sdma_constant_fill(struct ac_cmdbuf *cs, enum sdma_version sdma_ip_version,
                           uint64_t va, uint64_t size, uint32_t value)
{
   const uint32_t fill_size = 2; /* This means that count is in DWORDS. */

   assert(sdma_ip_version >= SDMA_2_4);

   const uint64_t max_fill_size = BITFIELD64_MASK(sdma_ip_version >= SDMA_6_0 ? 30 : 22) & ~0x3;
   const uint64_t bytes_written = MIN2(size, max_fill_size);

   ac_cmdbuf_begin(cs);
   ac_cmdbuf_emit(SDMA_PACKET(SDMA_OPCODE_CONSTANT_FILL, 0, 0) | (fill_size << 30));
   ac_cmdbuf_emit(va);
   ac_cmdbuf_emit(va >> 32);
   ac_cmdbuf_emit(value);
   ac_cmdbuf_emit(bytes_written - 1); /* Must be programmed in bytes, even if the fill is done in dwords. */
   ac_cmdbuf_end();

   return bytes_written;
}

uint64_t
ac_emit_sdma_copy_linear(struct ac_cmdbuf *cs, enum sdma_version sdma_ip_version,
                         uint64_t src_va, uint64_t dst_va, uint64_t size,
                         bool tmz)
{
   const unsigned max_size_per_packet =
      sdma_ip_version >= SDMA_5_2 ? SDMA_V5_2_COPY_MAX_BYTES : SDMA_V2_0_COPY_MAX_BYTES;
   uint32_t align = ~0u;

   assert(sdma_ip_version >= SDMA_2_0);

   /* SDMA FW automatically enables a faster dword copy mode when
    * source, destination and size are all dword-aligned.
    *
    * When source and destination are dword-aligned, round down the size to
    * take advantage of faster dword copy, and copy the remaining few bytes
    * with the last copy packet.
    */
   if ((src_va & 0x3) == 0 && (dst_va & 0x3) == 0 && size > 4 && (size & 0x3) != 0) {
      align = ~0x3u;
   }

   const uint64_t bytes_written = size >= 4 ? MIN2(size & align, max_size_per_packet) : size;

   ac_cmdbuf_begin(cs);
   ac_cmdbuf_emit(SDMA_PACKET(SDMA_OPCODE_COPY, SDMA_COPY_SUB_OPCODE_LINEAR, (tmz ? 4 : 0)));
   ac_cmdbuf_emit(sdma_ip_version >= SDMA_4_0 ? bytes_written - 1 : bytes_written);
   ac_cmdbuf_emit(0);
   ac_cmdbuf_emit(src_va);
   ac_cmdbuf_emit(src_va >> 32);
   ac_cmdbuf_emit(dst_va);
   ac_cmdbuf_emit(dst_va >> 32);
   ac_cmdbuf_end();

   return bytes_written;
}
