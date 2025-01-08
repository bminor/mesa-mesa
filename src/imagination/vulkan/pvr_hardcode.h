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

#ifndef PVR_HARDCODE_SHADERS_H
#define PVR_HARDCODE_SHADERS_H

#include <stdbool.h>
#include <stdint.h>
#include <vulkan/vulkan_core.h>

#include "compiler/shader_enums.h"
#include "util/u_dynarray.h"

/**
 * \file pvr_hardcode.h
 *
 * \brief Contains hard coding functions.
 * This should eventually be deleted as the compiler becomes more capable.
 */

void pvr_hard_code_get_idfwdf_program(
   const struct pvr_device_info *const dev_info,
   struct util_dynarray *program_out,
   uint32_t *usc_shareds_out,
   uint32_t *usc_temps_out);

void pvr_hard_code_get_passthrough_vertex_shader(
   const struct pvr_device_info *const dev_info,
   struct util_dynarray *program_out);
void pvr_hard_code_get_passthrough_rta_vertex_shader(
   const struct pvr_device_info *const dev_info,
   struct util_dynarray *program_out);

void pvr_hard_code_get_zero_wgmem_program(
   const struct pvr_device_info *const dev_info,
   unsigned start,
   unsigned count,
   struct util_dynarray *program_out,
   uint32_t *usc_temps_out);

#endif /* PVR_HARDCODE_SHADERS_H */
