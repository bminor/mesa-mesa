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

static void
ac_sdma_check_pitches(uint32_t pitch, uint32_t slice_pitch, uint32_t bpp, bool uses_depth)
{
   ASSERTED const uint32_t pitch_alignment = MAX2(1, 4 / bpp);
   assert(pitch);
   assert(pitch <= (1 << 14));
   assert(util_is_aligned(pitch, pitch_alignment));

   if (uses_depth) {
      ASSERTED const uint32_t slice_pitch_alignment = 4;
      assert(slice_pitch);
      assert(slice_pitch <= (1 << 28));
      assert(util_is_aligned(slice_pitch, slice_pitch_alignment));
   }
}

void
ac_emit_sdma_copy_linear_sub_window(struct ac_cmdbuf *cs, enum sdma_version sdma_ip_version,
                                    const struct ac_sdma_surf_linear *src,
                                    const struct ac_sdma_surf_linear *dst,
                                    uint32_t width, uint32_t height, uint32_t depth)
{
   /* This packet is the same since SDMA v2.4, haven't bothered to check older versions.
    * The main difference is the bitfield sizes:
    *
    * v2.4 - src/dst_pitch: 14 bits, rect_z: 11 bits
    * v4.0 - src/dst_pitch: 19 bits, rect_z: 11 bits
    * v5.0 - src/dst_pitch: 19 bits, rect_z: 13 bits
    *
    * We currently use the smallest limits (from SDMA v2.4).
    */
   assert(src->bpp == dst->bpp);
   assert(util_is_power_of_two_nonzero(src->bpp));
   ac_sdma_check_pitches(src->pitch, src->slice_pitch, src->bpp, false);
   ac_sdma_check_pitches(dst->pitch, dst->slice_pitch, dst->bpp, false);

   ac_cmdbuf_begin(cs);
   ac_cmdbuf_emit(SDMA_PACKET(SDMA_OPCODE_COPY, SDMA_COPY_SUB_OPCODE_LINEAR_SUB_WINDOW, 0) |
                  util_logbase2(src->bpp) << 29);
   ac_cmdbuf_emit(src->va);
   ac_cmdbuf_emit(src->va >> 32);
   ac_cmdbuf_emit(src->offset.x | src->offset.y << 16);
   ac_cmdbuf_emit(src->offset.z | (src->pitch - 1) << (sdma_ip_version >= SDMA_7_0 ? 16 : 13));
   ac_cmdbuf_emit(src->slice_pitch - 1);
   ac_cmdbuf_emit(dst->va);
   ac_cmdbuf_emit(dst->va >> 32);
   ac_cmdbuf_emit(dst->offset.x | dst->offset.y << 16);
   ac_cmdbuf_emit(dst->offset.z | (dst->pitch - 1) << (sdma_ip_version >= SDMA_7_0 ? 16 : 13));
   ac_cmdbuf_emit(dst->slice_pitch - 1);
   if (sdma_ip_version == SDMA_2_0) {
      ac_cmdbuf_emit(width | (height << 16));
      ac_cmdbuf_emit(depth);
   } else {
      ac_cmdbuf_emit((width - 1) | (height - 1) << 16);
      ac_cmdbuf_emit((depth - 1));
   }
   ac_cmdbuf_end();
}
