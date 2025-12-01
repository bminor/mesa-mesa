/*
 * Copyright (C) 2025 Collabora, Ltd.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the next
 * paragraph) shall be included in all copies or substantial portions of the
 * Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 *
 */

#include "pan_compiler.h"

#include "bifrost/bifrost_compile.h"
#include "bifrost/bifrost/disassemble.h"
#include "bifrost/valhall/disassemble.h"
#include "midgard/disassemble.h"
#include "midgard/midgard_compile.h"

#include "panfrost/model/pan_model.h"

const nir_shader_compiler_options *
pan_get_nir_shader_compiler_options(unsigned arch)
{
   switch (arch) {
   case 4:
   case 5:
      return &midgard_nir_options;
   case 6:
   case 7:
      return &bifrost_nir_options_v6;
   case 9:
   case 10:
      return &bifrost_nir_options_v9;
   case 11:
   case 12:
   case 13:
      return &bifrost_nir_options_v11;
   default:
      assert(!"Unsupported arch");
      return NULL;
   }
}

void
pan_preprocess_nir(nir_shader *nir, unsigned gpu_id)
{
   if (pan_arch(gpu_id) >= 6)
      bifrost_preprocess_nir(nir, gpu_id);
   else
      midgard_preprocess_nir(nir, gpu_id);
}

void
pan_optimize_nir(nir_shader *nir, unsigned gpu_id)
{
   assert(pan_arch(gpu_id) >= 6);
   bifrost_optimize_nir(nir, gpu_id);
}

void
pan_postprocess_nir(nir_shader *nir, unsigned gpu_id)
{
   if (pan_arch(gpu_id) >= 6)
      bifrost_postprocess_nir(nir, gpu_id);
   else
      midgard_postprocess_nir(nir, gpu_id);
}

void
pan_nir_lower_texture_early(nir_shader *nir, unsigned gpu_id)
{
   nir_lower_tex_options lower_tex_options = {
      .lower_txs_lod = true,
      .lower_txp = ~0,
      .lower_tg4_offsets = true,
      .lower_tg4_broadcom_swizzle = true,
      .lower_txd = pan_arch(gpu_id) < 6,
      .lower_txd_cube_map = true,
      .lower_invalid_implicit_lod = true,
      .lower_index_to_offset = pan_arch(gpu_id) >= 6,
   };

   NIR_PASS(_, nir, nir_lower_tex, &lower_tex_options);
}

void
pan_nir_lower_texture_late(nir_shader *nir, unsigned gpu_id)
{
   /* This must be called after any lowering of resource indices
    * (panfrost_nir_lower_res_indices / panvk_per_arch(nir_lower_descriptors))
    */
   if (pan_arch(gpu_id) >= 6)
      bifrost_lower_texture_late_nir(nir, gpu_id);
}

void
pan_disassemble(FILE *fp, const void *code, size_t size,
                unsigned gpu_id, bool verbose)
{
   if (pan_arch(gpu_id) >= 9)
      disassemble_valhall(fp, (const uint64_t *)code, size, verbose);
   else if (pan_arch(gpu_id) >= 6)
      disassemble_bifrost(fp, code, size, verbose);
   else
      disassemble_midgard(fp, code, size, gpu_id, verbose);
}
