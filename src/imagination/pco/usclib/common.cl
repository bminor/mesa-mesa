/*
 * Copyright © 2025 Imagination Technologies Ltd.
 * SPDX-License-Identifier: MIT
 */

#include "libcl.h"
#include "compiler/libcl/libcl_vk.h"

KERNEL(1)
vs_nop_common(void)
{
   return;
}

KERNEL(1)
fs_nop_common(void)
{
   return;
}

KERNEL(1)
cs_nop_common(void)
{
   return;
}

/* gl_Position = vec4(in.xyz, 1.0f); */
KERNEL(1)
vs_passthrough_common(void)
{
   nir_uvsw_write_pco(0, nir_load_vtxin_pco(3, 0));
   nir_uvsw_write_pco(3, 1.0f);
}

/* gl_Position = vec4(in.xyz, 1.0f); rta_out = in.w; */
KERNEL(1)
vs_passthrough_rta_common(void)
{
   vs_passthrough_common();
   nir_uvsw_write_pco(4, nir_load_vtxin_pco(1, 3));
}

/* TODO: uint index = cl_global_id.x;
 * instead of this function once things
 * are properly hooked up.
*/
static inline uint
query_calc_global_id(void)
{
   uint local_invoc_index = nir_load_vtxin_pco(1, 0);
   local_invoc_index &= get_local_size(0) - 1;
   uint wg_id = nir_load_coeff_pco(1, 0);
   return nir_imad(wg_id, get_local_size(0), local_invoc_index);
}

/* TODO: support parameter passing. */
/* TODO: switch to common implementation. */
KERNEL(32)
cs_query_availability_common(void)
{
   uint index_count = nir_load_preamble(1, PVR_QUERY_AVAILABILITY_DATA_INDEX_COUNT, 0);

   uint index_base_addr_lo = nir_load_preamble(1, PVR_QUERY_AVAILABILITY_DATA_INDEX_BO_LO, 0);
   uint index_base_addr_hi = nir_load_preamble(1, PVR_QUERY_AVAILABILITY_DATA_INDEX_BO_HI, 0);

   uint avail_base_addr_lo = nir_load_preamble(1, PVR_QUERY_AVAILABILITY_DATA_BO_LO, 0);
   uint avail_base_addr_hi = nir_load_preamble(1, PVR_QUERY_AVAILABILITY_DATA_BO_HI, 0);

   uint index = query_calc_global_id();

   if (index < index_count) {
      uint2 index_addr = nir_uadd64_32(index_base_addr_lo, index_base_addr_hi, index * sizeof(uint32_t));
      uint offset = nir_dma_ld_pco(1, index_addr);

      uint2 avail_addr = nir_uadd64_32(avail_base_addr_lo, avail_base_addr_hi, offset * sizeof(uint32_t));

      nir_dma_st_pco(avail_addr, ~0U);
   }
}

KERNEL(32)
cs_query_copy_common(void)
{
   uint index_count = nir_load_preamble(1, PVR_QUERY_COPY_DATA_INDEX_COUNT, 0);

   uint dest_base_addr_lo = nir_load_preamble(1, PVR_QUERY_COPY_DATA_DEST_BO_LO, 0);
   uint dest_base_addr_hi = nir_load_preamble(1, PVR_QUERY_COPY_DATA_DEST_BO_HI, 0);

   uint avail_base_addr_lo = nir_load_preamble(1, PVR_QUERY_COPY_DATA_AVAILABILITY_BO_LO, 0);
   uint avail_base_addr_hi = nir_load_preamble(1, PVR_QUERY_COPY_DATA_AVAILABILITY_BO_HI, 0);

   uint result_base_addr_lo = nir_load_preamble(1, PVR_QUERY_COPY_DATA_RESULT_BO_LO, 0);
   uint result_base_addr_hi = nir_load_preamble(1, PVR_QUERY_COPY_DATA_RESULT_BO_HI, 0);

   uint dest_stride = nir_load_preamble(1, PVR_QUERY_COPY_DATA_DEST_STRIDE, 0);

   uint flags = nir_load_preamble(1, PVR_QUERY_COPY_DATA_FLAGS, 0);

   uint index = query_calc_global_id();

   if (index < index_count) {
      uint2 avail_addr = nir_uadd64_32(avail_base_addr_lo, avail_base_addr_hi, index * sizeof(uint32_t));
      uint available = nir_dma_ld_pco(1, avail_addr);

      uint2 dest_addr = nir_umad64_32(dest_stride, index, dest_base_addr_lo, dest_base_addr_hi);

      if (available || (flags & VK_QUERY_RESULT_PARTIAL_BIT)) {
         uint2 result_addr = nir_uadd64_32(result_base_addr_lo, result_base_addr_hi, index * sizeof(uint32_t));
         uint result = nir_dma_ld_pco(1, result_addr);

         /* TODO: for 64/32-bit writes, just prep the 64-bit one and set the burst-length variably. */
         if (flags & VK_QUERY_RESULT_64_BIT) {
            /* TODO: check if data should be (result, 0) or (0, result) */
            nir_dma_st_pco(dest_addr, result, 0);
         } else {
            nir_dma_st_pco(dest_addr, result);
         }
      }

      if (flags & VK_QUERY_RESULT_WITH_AVAILABILITY_BIT) {
         if (flags & VK_QUERY_RESULT_64_BIT) {
            dest_addr = nir_uadd64_32(dest_addr.x, dest_addr.y, sizeof(uint64_t));
            /* TODO: check if data should be (available, 0) or (0, available) */
            nir_dma_st_pco(dest_addr, available, 0);
         } else {
            dest_addr = nir_uadd64_32(dest_addr.x, dest_addr.y, sizeof(uint32_t));
            nir_dma_st_pco(dest_addr, available);
         }
      }
   }
}

KERNEL(32)
cs_query_reset_common(void)
{
   uint index_count = nir_load_preamble(1, PVR_QUERY_RESET_DATA_INDEX_COUNT, 0);

   uint result_base_addr_lo = nir_load_preamble(1, PVR_QUERY_RESET_DATA_RESULT_BO_LO, 0);
   uint result_base_addr_hi = nir_load_preamble(1, PVR_QUERY_RESET_DATA_RESULT_BO_HI, 0);

   uint avail_base_addr_lo = nir_load_preamble(1, PVR_QUERY_RESET_DATA_AVAILABILITY_BO_LO, 0);
   uint avail_base_addr_hi = nir_load_preamble(1, PVR_QUERY_RESET_DATA_AVAILABILITY_BO_HI, 0);

   uint index = query_calc_global_id();

   if (index < index_count) {
      uint2 result_addr = nir_uadd64_32(result_base_addr_lo, result_base_addr_hi, index * sizeof(uint32_t));
      nir_dma_st_pco(result_addr, 0);

      uint2 avail_addr = nir_uadd64_32(avail_base_addr_lo, avail_base_addr_hi, index * sizeof(uint32_t));
      nir_dma_st_pco(avail_addr, 0);
   }
}
