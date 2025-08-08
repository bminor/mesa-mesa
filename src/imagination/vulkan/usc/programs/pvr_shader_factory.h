/*
 * Copyright © 2022 Imagination Technologies Ltd.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the next
 * paragraph) shall be included in all copies or substantial portions of the
 * Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */

#ifndef PVR_SHADER_FACTORY_H
#define PVR_SHADER_FACTORY_H

#include <stdint.h>
#include <stdbool.h>

#include "util/bitpack_helpers.h"
#include "util/bitscan.h"
#include "util/u_math.h"

enum pvr_spm_load_const {
   SPM_LOAD_CONST_TILE_BUFFER_1_UPPER,
   SPM_LOAD_CONST_TILE_BUFFER_1_LOWER,
   SPM_LOAD_CONST_TILE_BUFFER_2_UPPER,
   SPM_LOAD_CONST_TILE_BUFFER_2_LOWER,
   SPM_LOAD_CONST_TILE_BUFFER_3_UPPER,
   SPM_LOAD_CONST_TILE_BUFFER_3_LOWER,
   /* The following are only available if the core does not have the
    * has_eight_output_registers feature. I.e. only available if the device has
    * 4 output regs.
    */
   SPM_LOAD_CONST_TILE_BUFFER_4_UPPER,
   SPM_LOAD_CONST_TILE_BUFFER_4_LOWER,
   SPM_LOAD_CONST_TILE_BUFFER_5_UPPER,
   SPM_LOAD_CONST_TILE_BUFFER_5_LOWER,
   SPM_LOAD_CONST_TILE_BUFFER_6_UPPER,
   SPM_LOAD_CONST_TILE_BUFFER_6_LOWER,
   SPM_LOAD_CONST_TILE_BUFFER_7_UPPER,
   SPM_LOAD_CONST_TILE_BUFFER_7_LOWER,
};
#define PVR_SPM_LOAD_CONST_COUNT (SPM_LOAD_CONST_TILE_BUFFER_7_LOWER + 1)
#define PVR_SPM_LOAD_DEST_UNUSED ~0

#define PVR_SPM_LOAD_SAMPLES_COUNT 4U

#define PVR_SPM_LOAD_IN_REGS_COUNT 3 /* 1, 2, 4 */
#define PVR_SPM_LOAD_IN_TILE_BUFFERS_COUNT 7 /* 1, 2, 3, 4, 5, 6, 7 */

/* If output_regs == 8
 *    reg_load_programs = 4            # 1, 2, 4, 8
 *    tile_buffer_load_programs = 3    # 1, 2, 3
 * else                                #output_regs == 4
 *    reg_load_programs = 3            # 1, 2, 4
 *    tile_buffer_load_programs = 7    # 1, 2, 3, 4, 5, 6, 7
 *
 * See PVR_SPM_LOAD_IN_BUFFERS_COUNT for where the amount of
 * tile_buffer_load_programs comes from.
 *
 * Tot = sample_count * (reg_load_programs + tile_buffer_load_programs)
 */
/* FIXME: This is currently hard coded for the am62. The Chromebook has 8
 * output regs so the count is different.
 */
#define PVR_SPM_LOAD_PROGRAM_COUNT \
   (PVR_SPM_LOAD_SAMPLES_COUNT *   \
    (PVR_SPM_LOAD_IN_REGS_COUNT + PVR_SPM_LOAD_IN_TILE_BUFFERS_COUNT))

static inline uint32_t pvr_get_spm_load_program_index(uint32_t sample_count,
                                                      uint32_t num_tile_buffers,
                                                      uint32_t num_output_regs)
{
   uint32_t idx;

   assert(util_is_power_of_two_nonzero(sample_count));
   idx = util_logbase2(sample_count) *
         (PVR_SPM_LOAD_IN_REGS_COUNT + PVR_SPM_LOAD_IN_TILE_BUFFERS_COUNT);

   assert((num_tile_buffers > 0) ^ (num_output_regs > 0));

   if (num_output_regs > 0) {
      assert(util_is_power_of_two_nonzero(num_output_regs));
      assert(util_logbase2(num_output_regs) < PVR_SPM_LOAD_IN_REGS_COUNT);
      idx += util_logbase2(num_output_regs);
   } else {
      assert(num_tile_buffers <= PVR_SPM_LOAD_IN_TILE_BUFFERS_COUNT);
      idx += PVR_SPM_LOAD_IN_REGS_COUNT + num_tile_buffers - 1;
   }

   assert(idx < PVR_SPM_LOAD_PROGRAM_COUNT);
   return idx;
}

#endif /* PVR_SHADER_FACTORY_H */
