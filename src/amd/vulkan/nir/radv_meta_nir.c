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

nir_shader *
radv_meta_nir_build_blit_vertex_shader(struct radv_device *dev)
{
   const struct glsl_type *vec4 = glsl_vec4_type();
   nir_builder b = radv_meta_init_shader(dev, MESA_SHADER_VERTEX, "meta_blit_vs");

   nir_variable *pos_out = nir_variable_create(b.shader, nir_var_shader_out, vec4, "gl_Position");
   pos_out->data.location = VARYING_SLOT_POS;

   nir_variable *tex_pos_out = nir_variable_create(b.shader, nir_var_shader_out, vec4, "v_tex_pos");
   tex_pos_out->data.location = VARYING_SLOT_VAR0;
   tex_pos_out->data.interpolation = INTERP_MODE_SMOOTH;

   nir_def *outvec = nir_gen_rect_vertices(&b, NULL, NULL);

   nir_store_var(&b, pos_out, outvec, 0xf);

   nir_def *src_box = nir_load_push_constant(&b, 4, 32, nir_imm_int(&b, 0), .range = 16);
   nir_def *src0_z = nir_load_push_constant(&b, 1, 32, nir_imm_int(&b, 0), .base = 16, .range = 4);

   nir_def *vertex_id = nir_load_vertex_id_zero_base(&b);

   /* vertex 0 - src0_x, src0_y, src0_z */
   /* vertex 1 - src0_x, src1_y, src0_z*/
   /* vertex 2 - src1_x, src0_y, src0_z */
   /* so channel 0 is vertex_id != 2 ? src_x : src_x + w
      channel 1 is vertex id != 1 ? src_y : src_y + w */

   nir_def *c0cmp = nir_ine_imm(&b, vertex_id, 2);
   nir_def *c1cmp = nir_ine_imm(&b, vertex_id, 1);

   nir_def *comp[4];
   comp[0] = nir_bcsel(&b, c0cmp, nir_channel(&b, src_box, 0), nir_channel(&b, src_box, 2));

   comp[1] = nir_bcsel(&b, c1cmp, nir_channel(&b, src_box, 1), nir_channel(&b, src_box, 3));
   comp[2] = src0_z;
   comp[3] = nir_imm_float(&b, 1.0);
   nir_def *out_tex_vec = nir_vec(&b, comp, 4);
   nir_store_var(&b, tex_pos_out, out_tex_vec, 0xf);
   return b.shader;
}

nir_shader *
radv_meta_nir_build_blit_copy_fragment_shader(struct radv_device *dev, enum glsl_sampler_dim tex_dim)
{
   const struct glsl_type *vec4 = glsl_vec4_type();
   nir_builder b = radv_meta_init_shader(dev, MESA_SHADER_FRAGMENT, "meta_blit_fs.%d", tex_dim);

   nir_variable *tex_pos_in = nir_variable_create(b.shader, nir_var_shader_in, vec4, "v_tex_pos");
   tex_pos_in->data.location = VARYING_SLOT_VAR0;

   /* Swizzle the array index which comes in as Z coordinate into the right
    * position.
    */
   unsigned swz[] = {0, (tex_dim == GLSL_SAMPLER_DIM_1D ? 2 : 1), 2};
   nir_def *const tex_pos =
      nir_swizzle(&b, nir_load_var(&b, tex_pos_in), swz, (tex_dim == GLSL_SAMPLER_DIM_1D ? 2 : 3));

   const struct glsl_type *sampler_type =
      glsl_sampler_type(tex_dim, false, tex_dim != GLSL_SAMPLER_DIM_3D, glsl_get_base_type(vec4));
   nir_variable *sampler = nir_variable_create(b.shader, nir_var_uniform, sampler_type, "s_tex");
   sampler->data.descriptor_set = 0;
   sampler->data.binding = 0;

   nir_deref_instr *tex_deref = nir_build_deref_var(&b, sampler);
   nir_def *color = nir_tex_deref(&b, tex_deref, tex_deref, tex_pos);

   nir_variable *color_out = nir_variable_create(b.shader, nir_var_shader_out, vec4, "f_color");
   color_out->data.location = FRAG_RESULT_DATA0;
   nir_store_var(&b, color_out, color, 0xf);

   return b.shader;
}

nir_shader *
radv_meta_nir_build_blit_copy_fragment_shader_depth(struct radv_device *dev, enum glsl_sampler_dim tex_dim)
{
   const struct glsl_type *vec4 = glsl_vec4_type();
   nir_builder b = radv_meta_init_shader(dev, MESA_SHADER_FRAGMENT, "meta_blit_depth_fs.%d", tex_dim);

   nir_variable *tex_pos_in = nir_variable_create(b.shader, nir_var_shader_in, vec4, "v_tex_pos");
   tex_pos_in->data.location = VARYING_SLOT_VAR0;

   /* Swizzle the array index which comes in as Z coordinate into the right
    * position.
    */
   unsigned swz[] = {0, (tex_dim == GLSL_SAMPLER_DIM_1D ? 2 : 1), 2};
   nir_def *const tex_pos =
      nir_swizzle(&b, nir_load_var(&b, tex_pos_in), swz, (tex_dim == GLSL_SAMPLER_DIM_1D ? 2 : 3));

   const struct glsl_type *sampler_type =
      glsl_sampler_type(tex_dim, false, tex_dim != GLSL_SAMPLER_DIM_3D, glsl_get_base_type(vec4));
   nir_variable *sampler = nir_variable_create(b.shader, nir_var_uniform, sampler_type, "s_tex");
   sampler->data.descriptor_set = 0;
   sampler->data.binding = 0;

   nir_deref_instr *tex_deref = nir_build_deref_var(&b, sampler);
   nir_def *color = nir_tex_deref(&b, tex_deref, tex_deref, tex_pos);

   nir_variable *color_out = nir_variable_create(b.shader, nir_var_shader_out, vec4, "f_color");
   color_out->data.location = FRAG_RESULT_DEPTH;
   nir_store_var(&b, color_out, color, 0x1);

   return b.shader;
}

nir_shader *
radv_meta_nir_build_blit_copy_fragment_shader_stencil(struct radv_device *dev, enum glsl_sampler_dim tex_dim)
{
   const struct glsl_type *vec4 = glsl_vec4_type();
   nir_builder b = radv_meta_init_shader(dev, MESA_SHADER_FRAGMENT, "meta_blit_stencil_fs.%d", tex_dim);

   nir_variable *tex_pos_in = nir_variable_create(b.shader, nir_var_shader_in, vec4, "v_tex_pos");
   tex_pos_in->data.location = VARYING_SLOT_VAR0;

   /* Swizzle the array index which comes in as Z coordinate into the right
    * position.
    */
   unsigned swz[] = {0, (tex_dim == GLSL_SAMPLER_DIM_1D ? 2 : 1), 2};
   nir_def *const tex_pos =
      nir_swizzle(&b, nir_load_var(&b, tex_pos_in), swz, (tex_dim == GLSL_SAMPLER_DIM_1D ? 2 : 3));

   const struct glsl_type *sampler_type =
      glsl_sampler_type(tex_dim, false, tex_dim != GLSL_SAMPLER_DIM_3D, glsl_get_base_type(vec4));
   nir_variable *sampler = nir_variable_create(b.shader, nir_var_uniform, sampler_type, "s_tex");
   sampler->data.descriptor_set = 0;
   sampler->data.binding = 0;

   nir_deref_instr *tex_deref = nir_build_deref_var(&b, sampler);
   nir_def *color = nir_tex_deref(&b, tex_deref, tex_deref, tex_pos);

   nir_variable *color_out = nir_variable_create(b.shader, nir_var_shader_out, vec4, "f_color");
   color_out->data.location = FRAG_RESULT_STENCIL;
   nir_store_var(&b, color_out, color, 0x1);

   return b.shader;
}
