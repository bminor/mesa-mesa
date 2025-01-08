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

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <vulkan/vulkan_core.h>

#include "compiler/shader_enums.h"
#include "hwdef/rogue_hw_utils.h"
#include "pvr_device_info.h"
#include "pvr_hardcode.h"
#include "pvr_private.h"
#include "util/macros.h"
#include "util/u_dynarray.h"
#include "util/u_process.h"

/**
 * \file pvr_hardcode.c
 *
 * \brief Contains hard coding functions.
 * This should eventually be deleted as the compiler becomes more capable.
 */

#define util_dynarray_append_mem(buf, size, mem) \
   memcpy(util_dynarray_grow_bytes((buf), 1, size), mem, size)

void pvr_hard_code_get_idfwdf_program(
   const struct pvr_device_info *const dev_info,
   struct util_dynarray *program_out,
   uint32_t *usc_shareds_out,
   uint32_t *usc_temps_out)
{
   static const uint8_t shader[] = { 0, 0, 0, 0, 0, 0, 0, 0 };

   mesa_loge("No hard coded idfwdf program. Returning empty program.");

   util_dynarray_append_mem(program_out, ARRAY_SIZE(shader), &shader[0]);

   *usc_shareds_out = 12U;
   *usc_temps_out = 4U;
}

void pvr_hard_code_get_passthrough_vertex_shader(
   const struct pvr_device_info *const dev_info,
   struct util_dynarray *program_out)
{
   static const uint8_t shader[] = { 0, 0, 0, 0, 0, 0, 0, 0 };

   mesa_loge(
      "No hard coded passthrough vertex shader. Returning empty shader.");

   util_dynarray_append_mem(program_out, ARRAY_SIZE(shader), &shader[0]);
};

/* Render target array (RTA). */
void pvr_hard_code_get_passthrough_rta_vertex_shader(
   const struct pvr_device_info *const dev_info,
   struct util_dynarray *program_out)
{
   uint32_t shader[] = { 0, 0, 0, 0, 0, 0, 0, 0 };

   util_dynarray_append_mem(program_out, ARRAY_SIZE(shader), &shader);

   mesa_loge("No hard coded passthrough rta vertex shader. Returning "
             "empty shader.");
}

void pvr_hard_code_get_zero_wgmem_program(
   UNUSED const struct pvr_device_info *const dev_info,
   UNUSED unsigned start,
   UNUSED unsigned count,
   struct util_dynarray *program_out,
   uint32_t *usc_temps_out)
{
   uint32_t shader[] = { 0, 0, 0, 0, 0, 0, 0, 0 };

   mesa_loge("No hard coded zero wg memory program. Returning empty program.");

   util_dynarray_append_mem(program_out, sizeof(shader), &shader[0]);

   *usc_temps_out = 2;
}
