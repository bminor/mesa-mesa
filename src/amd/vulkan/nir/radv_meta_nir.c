/* Based on anv:
 * Copyright © 2015 Intel Corporation
 *
 * Copyright © 2016 Red Hat Inc.
 * Copyright © 2018 Valve Corporation
 *
 * SPDX-License-Identifier: MIT
 */

#include "../meta/radv_meta.h"
#include "nir_builder.h"

nir_shader *
radv_meta_nir_build_buffer_fill_shader(struct radv_device *dev)
{
   nir_builder b = radv_meta_init_shader(dev, MESA_SHADER_COMPUTE, "meta_buffer_fill");
   b.shader->info.workgroup_size[0] = 64;

   nir_def *pconst = nir_load_push_constant(&b, 4, 32, nir_imm_int(&b, 0), .range = 16);
   nir_def *buffer_addr = nir_pack_64_2x32(&b, nir_channels(&b, pconst, 0b0011));
   nir_def *max_offset = nir_channel(&b, pconst, 2);
   nir_def *data = nir_swizzle(&b, nir_channel(&b, pconst, 3), (unsigned[]){0, 0, 0, 0}, 4);

   nir_def *global_id =
      nir_iadd(&b, nir_imul_imm(&b, nir_channel(&b, nir_load_workgroup_id(&b), 0), b.shader->info.workgroup_size[0]),
               nir_load_local_invocation_index(&b));

   nir_def *offset = nir_imin(&b, nir_imul_imm(&b, global_id, 16), max_offset);
   nir_def *dst_addr = nir_iadd(&b, buffer_addr, nir_u2u64(&b, offset));
   nir_build_store_global(&b, data, dst_addr, .align_mul = 4);

   return b.shader;
}

nir_shader *
radv_meta_nir_build_buffer_copy_shader(struct radv_device *dev)
{
   nir_builder b = radv_meta_init_shader(dev, MESA_SHADER_COMPUTE, "meta_buffer_copy");
   b.shader->info.workgroup_size[0] = 64;

   nir_def *pconst = nir_load_push_constant(&b, 4, 32, nir_imm_int(&b, 0), .range = 16);
   nir_def *max_offset = nir_load_push_constant(&b, 1, 32, nir_imm_int(&b, 0), .base = 16, .range = 4);
   nir_def *src_addr = nir_pack_64_2x32(&b, nir_channels(&b, pconst, 0b0011));
   nir_def *dst_addr = nir_pack_64_2x32(&b, nir_channels(&b, pconst, 0b1100));

   nir_def *global_id =
      nir_iadd(&b, nir_imul_imm(&b, nir_channel(&b, nir_load_workgroup_id(&b), 0), b.shader->info.workgroup_size[0]),
               nir_load_local_invocation_index(&b));

   nir_def *offset = nir_u2u64(&b, nir_imin(&b, nir_imul_imm(&b, global_id, 16), max_offset));

   nir_def *data = nir_build_load_global(&b, 4, 32, nir_iadd(&b, src_addr, offset), .align_mul = 4);
   nir_build_store_global(&b, data, nir_iadd(&b, dst_addr, offset), .align_mul = 4);

   return b.shader;
}
