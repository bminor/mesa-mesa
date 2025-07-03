/*
 * Copyright © 2023-2025 Amazon.com, Inc. or its affiliates.
 *
 * SPDX-License-Identifier: MIT
 */

#include "pan_afbc.h"
#include "util/perf/cpu_trace.h"
#include "util/detect_arch.h"

#if DETECT_ARCH_AARCH64
#include <arm_neon.h>

/* Arm A64 NEON intrinsics version. */
uint32_t
pan_afbc_payload_layout_packed(unsigned arch,
                               const struct pan_afbc_headerblock *headers,
                               struct pan_afbc_payload_extent *layout,
                               uint32_t nr_blocks, enum pipe_format format,
                               uint64_t modifier)
{
   MESA_TRACE_FUNC();

   uint32_t uncompressed_size =
      pan_afbc_payload_uncompressed_size(format, modifier);
   uint32_t body_size = 0;

   alignas(16) static const uint8_t idx0[16] =
      { 4, 5, 6, ~0, 7, 8, 9, ~0, 10, 11, 12, ~0, 13, 14, 15, ~0 };
   alignas(16) static const uint8_t idx1[16] =
      { 0, 4, 8, 12, 16, 20, 24, 28, 32, 36, 40, 44, 48, 52, 56, 60 };
   alignas(16) static const uint8_t mask[16] =
      { 63, 63, 63, 63, 63, 63, 63, 63, 63, 63, 63, 63, 63, 63, 63, 63 };
   alignas(16) static const uint8_t ones[16] =
      { 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1 };

   uint8x16_t vidx0 = vld1q_u8(idx0);
   uint8x16_t vidx1 = vld1q_u8(idx1);
   uint8x16_t vmask = vld1q_u8(mask);
   uint8x16_t vones = vld1q_u8(ones);

   for (uint32_t i = 0; i < nr_blocks; ++i) {
      uint32_t payload_size = 0;

      /* Skip sum if the 1st subblock is 0 (solid color encoding). */
      if (arch < 7 || headers[i].payload.subblock_sizes[0] & 0x3f) {
         uint8x16_t vhdr = vld1q_u8((uint8_t *)&headers[i]);

         /* Dispatch 6-bit packed 16 subblock sizes into 8-bit vector. */
         uint8x16_t v0 = vqtbl1q_u8(vhdr, vidx0);
         uint8x16_t v1 = vreinterpretq_u8_u32(
            vshrq_n_u32(vreinterpretq_u32_u8(v0), 6));
         uint8x16_t v2 = vreinterpretq_u8_u32(
            vshrq_n_u32(vreinterpretq_u32_u8(v0), 12));
         uint8x16_t v3 = vreinterpretq_u8_u32(
            vshrq_n_u32(vreinterpretq_u32_u8(v0), 18));
         uint8x16x4_t vtbl = {{ v0, v1, v2, v3 }};
         v0 = vqtbl4q_u8(vtbl, vidx1);
         v0 = vandq_u8(v0, vmask);

         /* Sum across vector. */
         payload_size = vaddlvq_u8(v0);

         /* Number of subblocks of size 1. */
         v0 = vceqq_u8(v0, vones);
         v0 = vandq_u8(v0, vones);
         uint32_t nr_ones = vaddvq_u8(v0);

         /* Payload size already stores subblocks of size 1. Fix-up the sum
          * using the number of such subblocks. */
         payload_size += nr_ones * (uncompressed_size - 1);

         payload_size = ALIGN_POT(payload_size, 16);
      }

      layout[i].size = payload_size;
      layout[i].offset = body_size;
      body_size += payload_size;
   }

   return body_size;
}

#else

/* Generic version. */
uint32_t
pan_afbc_payload_layout_packed(unsigned arch,
                               const struct pan_afbc_headerblock *headers,
                               struct pan_afbc_payload_extent *layout,
                               uint32_t nr_blocks, enum pipe_format format,
                               uint64_t modifier)
{
   MESA_TRACE_FUNC();

   uint32_t uncompressed_size =
      pan_afbc_payload_uncompressed_size(format, modifier);
   uint32_t body_size = 0;

   /* XXX: It might be faster to copy the header from non-cacheable memory
    * into a cacheline sized chunk in cacheable memory in order to avoid too
    * many uncached transactions. Not sure though, so it needs testing. */

   for (uint32_t i = 0; i < nr_blocks; i++) {
      uint32_t payload_size =
         pan_afbc_payload_size(arch, headers[i], uncompressed_size);
      layout[i].size = payload_size;
      layout[i].offset = body_size;
      body_size += payload_size;
   }

   return body_size;
}

#endif
