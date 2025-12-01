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
#include "pan_nir.h"

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

/** Converts a per-component mask to a byte mask */
uint16_t
pan_to_bytemask(unsigned bytes, unsigned mask)
{
   switch (bytes) {
   case 0:
      assert(mask == 0);
      return 0;

   case 8:
      return mask;

   case 16: {
      unsigned space =
         (mask & 0x1) | ((mask & 0x2) << (2 - 1)) | ((mask & 0x4) << (4 - 2)) |
         ((mask & 0x8) << (6 - 3)) | ((mask & 0x10) << (8 - 4)) |
         ((mask & 0x20) << (10 - 5)) | ((mask & 0x40) << (12 - 6)) |
         ((mask & 0x80) << (14 - 7));

      return space | (space << 1);
   }

   case 32: {
      unsigned space = (mask & 0x1) | ((mask & 0x2) << (4 - 1)) |
                       ((mask & 0x4) << (8 - 2)) | ((mask & 0x8) << (12 - 3));

      return space | (space << 1) | (space << 2) | (space << 3);
   }

   case 64: {
      unsigned A = (mask & 0x1) ? 0xFF : 0x00;
      unsigned B = (mask & 0x2) ? 0xFF : 0x00;
      return A | (B << 8);
   }

   default:
      UNREACHABLE("Invalid register mode");
   }
}

/* Could optimize with a better data structure if anyone cares, TODO: profile */
unsigned
pan_lookup_pushed_ubo(struct pan_ubo_push *push, unsigned ubo, unsigned offs)
{
   struct pan_ubo_word word = {.ubo = ubo, .offset = offs};

   for (unsigned i = 0; i < push->count; ++i) {
      if (memcmp(push->words + i, &word, sizeof(word)) == 0)
         return i;
   }

   UNREACHABLE("UBO not pushed");
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
