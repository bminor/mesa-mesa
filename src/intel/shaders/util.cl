/* Copyright © 2024 Intel Corporation
 * SPDX-License-Identifier: MIT
 */

#include "libintel_shaders.h"

/* Copy size from src_ptr to dst_ptr for using a single lane with size
 * multiple of 4.
 */
void genX(copy_data)(global void *dst_ptr,
                     global void *src_ptr,
                     uint32_t size)
{
   for (uint32_t offset = 0; offset < size; offset += 16) {
      if (offset + 16 <= size) {
         *(global uint4 *)(dst_ptr + offset) = *(global uint4 *)(src_ptr + offset);
      } else if (offset + 12 <= size) {
         *(global uint3 *)(dst_ptr + offset) = *(global uint3 *)(src_ptr + offset);
      } else if (offset + 8 <= size) {
         *(global uint2 *)(dst_ptr + offset) = *(global uint2 *)(src_ptr + offset);
      } else if (offset + 4 <= size) {
         *(global uint *)(dst_ptr + offset) = *(global uint *)(src_ptr + offset);
      }
   }
}
