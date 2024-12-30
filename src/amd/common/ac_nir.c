/*
 * Copyright © 2016 Bas Nieuwenhuizen
 *
 * SPDX-License-Identifier: MIT
 */

#include "ac_gpu_info.h"
#include "ac_nir.h"
#include "ac_nir_helpers.h"
#include "sid.h"
#include "nir_builder.h"
#include "nir_xfb_info.h"

/* Sleep for the given number of clock cycles. */
void
ac_nir_sleep(nir_builder *b, unsigned num_cycles)
{
   /* s_sleep can only sleep for N*64 cycles. */
   if (num_cycles >= 64) {
      nir_sleep_amd(b, num_cycles / 64);
      num_cycles &= 63;
   }

   /* Use s_nop to sleep for the remaining cycles. */
   while (num_cycles) {
      unsigned nop_cycles = MIN2(num_cycles, 16);

      nir_nop_amd(b, nop_cycles - 1);
      num_cycles -= nop_cycles;
   }
}

/* Load argument with index start from arg plus relative_index. */
nir_def *
ac_nir_load_arg_at_offset(nir_builder *b, const struct ac_shader_args *ac_args,
                          struct ac_arg arg, unsigned relative_index)
{
   unsigned arg_index = arg.arg_index + relative_index;
   unsigned num_components = ac_args->args[arg_index].size;

   if (ac_args->args[arg_index].skip)
      return nir_undef(b, num_components, 32);

   if (ac_args->args[arg_index].file == AC_ARG_SGPR)
      return nir_load_scalar_arg_amd(b, num_components, .base = arg_index);
   else
      return nir_load_vector_arg_amd(b, num_components, .base = arg_index);
}

nir_def *
ac_nir_load_arg(nir_builder *b, const struct ac_shader_args *ac_args, struct ac_arg arg)
{
   return ac_nir_load_arg_at_offset(b, ac_args, arg, 0);
}

nir_def *
ac_nir_load_arg_upper_bound(nir_builder *b, const struct ac_shader_args *ac_args, struct ac_arg arg,
                            unsigned upper_bound)
{
   nir_def *value = ac_nir_load_arg_at_offset(b, ac_args, arg, 0);
   nir_intrinsic_set_arg_upper_bound_u32_amd(nir_instr_as_intrinsic(value->parent_instr),
                                             upper_bound);
   return value;
}

void
ac_nir_store_arg(nir_builder *b, const struct ac_shader_args *ac_args, struct ac_arg arg,
                 nir_def *val)
{
   assert(nir_cursor_current_block(b->cursor)->cf_node.parent->type == nir_cf_node_function);

   if (ac_args->args[arg.arg_index].file == AC_ARG_SGPR)
      nir_store_scalar_arg_amd(b, val, .base = arg.arg_index);
   else
      nir_store_vector_arg_amd(b, val, .base = arg.arg_index);
}

static nir_def *
ac_nir_unpack_value(nir_builder *b, nir_def *value, unsigned rshift, unsigned bitwidth)
{
   if (rshift == 0 && bitwidth == 32)
      return value;
   else if (rshift == 0)
      return nir_iand_imm(b, value, BITFIELD_MASK(bitwidth));
   else if ((32 - rshift) <= bitwidth)
      return nir_ushr_imm(b, value, rshift);
   else
      return nir_ubfe_imm(b, value, rshift, bitwidth);
}

nir_def *
ac_nir_unpack_arg(nir_builder *b, const struct ac_shader_args *ac_args, struct ac_arg arg,
                  unsigned rshift, unsigned bitwidth)
{
   nir_def *value = ac_nir_load_arg(b, ac_args, arg);
   return ac_nir_unpack_value(b, value, rshift, bitwidth);
}

static bool
is_sin_cos(const nir_instr *instr, UNUSED const void *_)
{
   return instr->type == nir_instr_type_alu && (nir_instr_as_alu(instr)->op == nir_op_fsin ||
                                                nir_instr_as_alu(instr)->op == nir_op_fcos);
}

static nir_def *
lower_sin_cos(struct nir_builder *b, nir_instr *instr, UNUSED void *_)
{
   nir_alu_instr *sincos = nir_instr_as_alu(instr);
   nir_def *src = nir_fmul_imm(b, nir_ssa_for_alu_src(b, sincos, 0), 0.15915493667125702);
   return sincos->op == nir_op_fsin ? nir_fsin_amd(b, src) : nir_fcos_amd(b, src);
}

bool
ac_nir_lower_sin_cos(nir_shader *shader)
{
   return nir_shader_lower_instructions(shader, is_sin_cos, lower_sin_cos, NULL);
}

typedef struct {
   const struct ac_shader_args *const args;
   const enum amd_gfx_level gfx_level;
   bool has_ls_vgpr_init_bug;
   unsigned wave_size;
   unsigned workgroup_size;
   const enum ac_hw_stage hw_stage;

   nir_def *vertex_id;
   nir_def *instance_id;
   nir_def *vs_rel_patch_id;
   nir_def *tes_u;
   nir_def *tes_v;
   nir_def *tes_patch_id;
   nir_def *tes_rel_patch_id;
} lower_intrinsics_to_args_state;

static nir_def *
preload_arg(lower_intrinsics_to_args_state *s, nir_function_impl *impl, struct ac_arg arg,
            struct ac_arg ls_buggy_arg, unsigned upper_bound)
{
   nir_builder start_b = nir_builder_at(nir_before_impl(impl));
   nir_def *value = ac_nir_load_arg_upper_bound(&start_b, s->args, arg, upper_bound);

   /* If there are no HS threads, SPI mistakenly loads the LS VGPRs starting at VGPR 0. */
   if ((s->hw_stage == AC_HW_LOCAL_SHADER || s->hw_stage == AC_HW_HULL_SHADER) &&
       s->has_ls_vgpr_init_bug) {
      nir_def *count = ac_nir_unpack_arg(&start_b, s->args, s->args->merged_wave_info, 8, 8);
      nir_def *hs_empty = nir_ieq_imm(&start_b, count, 0);
      value = nir_bcsel(&start_b, hs_empty,
                        ac_nir_load_arg_upper_bound(&start_b, s->args, ls_buggy_arg, upper_bound),
                        value);
   }
   return value;
}

static nir_def *
load_subgroup_id_lowered(lower_intrinsics_to_args_state *s, nir_builder *b)
{
   if (s->workgroup_size <= s->wave_size) {
      return nir_imm_int(b, 0);
   } else if (s->hw_stage == AC_HW_COMPUTE_SHADER) {
      if (s->gfx_level >= GFX12)
         return false;

      assert(s->args->tg_size.used);

      if (s->gfx_level >= GFX10_3) {
         return ac_nir_unpack_arg(b, s->args, s->args->tg_size, 20, 5);
      } else {
         /* GFX6-10 don't actually support a wave id, but we can
          * use the ordered id because ORDERED_APPEND_* is set to
          * zero in the compute dispatch initiatior.
          */
         return ac_nir_unpack_arg(b, s->args, s->args->tg_size, 6, 6);
      }
   } else if (s->hw_stage == AC_HW_HULL_SHADER && s->gfx_level >= GFX11) {
      assert(s->args->tcs_wave_id.used);
      return ac_nir_unpack_arg(b, s->args, s->args->tcs_wave_id, 0, 3);
   } else if (s->hw_stage == AC_HW_LEGACY_GEOMETRY_SHADER ||
              s->hw_stage == AC_HW_NEXT_GEN_GEOMETRY_SHADER) {
      assert(s->args->merged_wave_info.used);
      return ac_nir_unpack_arg(b, s->args, s->args->merged_wave_info, 24, 4);
   } else {
      return nir_imm_int(b, 0);
   }
}

static bool
lower_intrinsic_to_arg(nir_builder *b, nir_instr *instr, void *state)
{
   if (instr->type != nir_instr_type_intrinsic)
      return false;

   lower_intrinsics_to_args_state *s = (lower_intrinsics_to_args_state *)state;
   nir_intrinsic_instr *intrin = nir_instr_as_intrinsic(instr);
   nir_def *replacement = NULL;
   b->cursor = nir_after_instr(&intrin->instr);

   switch (intrin->intrinsic) {
   case nir_intrinsic_load_subgroup_id:
      replacement = load_subgroup_id_lowered(s, b);
      break;
   case nir_intrinsic_load_num_subgroups: {
      if (s->hw_stage == AC_HW_COMPUTE_SHADER) {
         assert(s->args->tg_size.used);
         replacement = ac_nir_unpack_arg(b, s->args, s->args->tg_size, 0, 6);
      } else if (s->hw_stage == AC_HW_LEGACY_GEOMETRY_SHADER ||
                 s->hw_stage == AC_HW_NEXT_GEN_GEOMETRY_SHADER) {
         assert(s->args->merged_wave_info.used);
         replacement = ac_nir_unpack_arg(b, s->args, s->args->merged_wave_info, 28, 4);
      } else {
         replacement = nir_imm_int(b, 1);
      }

      break;
   }
   case nir_intrinsic_load_workgroup_id:
      if (b->shader->info.stage == MESA_SHADER_MESH) {
         /* This lowering is only valid with fast_launch = 2, otherwise we assume that
          * lower_workgroup_id_to_index removed any uses of the workgroup id by this point.
          */
         assert(s->gfx_level >= GFX11);
         nir_def *xy = ac_nir_load_arg(b, s->args, s->args->tess_offchip_offset);
         nir_def *z = ac_nir_load_arg(b, s->args, s->args->gs_attr_offset);
         replacement = nir_vec3(b, nir_extract_u16(b, xy, nir_imm_int(b, 0)),
                                nir_extract_u16(b, xy, nir_imm_int(b, 1)),
                                nir_extract_u16(b, z, nir_imm_int(b, 1)));
      } else {
         return false;
      }
      break;
   case nir_intrinsic_load_pixel_coord:
      replacement = nir_unpack_32_2x16(b, ac_nir_load_arg(b, s->args, s->args->pos_fixed_pt));
      break;
   case nir_intrinsic_load_frag_coord:
      replacement = nir_vec4(b, ac_nir_load_arg(b, s->args, s->args->frag_pos[0]),
                             ac_nir_load_arg(b, s->args, s->args->frag_pos[1]),
                             ac_nir_load_arg(b, s->args, s->args->frag_pos[2]),
                             ac_nir_load_arg(b, s->args, s->args->frag_pos[3]));
      break;
   case nir_intrinsic_load_local_invocation_id: {
      unsigned num_bits[3];
      nir_def *vec[3];

      for (unsigned i = 0; i < 3; i++) {
         bool has_chan = b->shader->info.workgroup_size_variable ||
                         b->shader->info.workgroup_size[i] > 1;
         /* Extract as few bits possible - we want the constant to be an inline constant
          * instead of a literal.
          */
         num_bits[i] = !has_chan ? 0 :
                       b->shader->info.workgroup_size_variable ?
                                   10 : util_logbase2_ceil(b->shader->info.workgroup_size[i]);
      }

      if (s->args->local_invocation_ids_packed.used) {
         unsigned extract_bits[3];
         memcpy(extract_bits, num_bits, sizeof(num_bits));

         /* Thread IDs are packed in VGPR0, 10 bits per component.
          * Always extract all remaining bits if later ID components are always 0, which will
          * translate to a bit shift.
          */
         if (num_bits[2]) {
            extract_bits[2] = 12; /* Z > 0 */
         } else if (num_bits[1])
            extract_bits[1] = 22; /* Y > 0, Z == 0 */
         else if (num_bits[0])
            extract_bits[0] = 32; /* X > 0, Y == 0, Z == 0 */

         nir_def *ids_packed =
            ac_nir_load_arg_upper_bound(b, s->args, s->args->local_invocation_ids_packed,
                                        b->shader->info.workgroup_size_variable ?
                                           0 : ((b->shader->info.workgroup_size[0] - 1) |
                                                ((b->shader->info.workgroup_size[1] - 1) << 10) |
                                                ((b->shader->info.workgroup_size[2] - 1) << 20)));

         for (unsigned i = 0; i < 3; i++) {
            vec[i] = !num_bits[i] ? nir_imm_int(b, 0) :
                                    ac_nir_unpack_value(b,  ids_packed, i * 10, extract_bits[i]);
         }
      } else {
         const struct ac_arg ids[] = {
            s->args->local_invocation_id_x,
            s->args->local_invocation_id_y,
            s->args->local_invocation_id_z,
         };

         for (unsigned i = 0; i < 3; i++) {
            unsigned max = b->shader->info.workgroup_size_variable ?
                              1023 : (b->shader->info.workgroup_size[i] - 1);
            vec[i] = !num_bits[i] ? nir_imm_int(b, 0) :
                                    ac_nir_load_arg_upper_bound(b, s->args, ids[i], max);
         }
      }
      replacement = nir_vec(b, vec, 3);
      break;
   }
   case nir_intrinsic_load_merged_wave_info_amd:
      replacement = ac_nir_load_arg(b, s->args, s->args->merged_wave_info);
      break;
   case nir_intrinsic_load_workgroup_num_input_vertices_amd:
      replacement = ac_nir_unpack_arg(b, s->args, s->args->gs_tg_info, 12, 9);
      break;
   case nir_intrinsic_load_workgroup_num_input_primitives_amd:
      replacement = ac_nir_unpack_arg(b, s->args, s->args->gs_tg_info, 22, 9);
      break;
   case nir_intrinsic_load_packed_passthrough_primitive_amd:
      /* NGG passthrough mode: the HW already packs the primitive export value to a single register.
       */
      replacement = ac_nir_load_arg(b, s->args, s->args->gs_vtx_offset[0]);
      break;
   case nir_intrinsic_load_ordered_id_amd:
      replacement = ac_nir_unpack_arg(b, s->args, s->args->gs_tg_info, 0, 12);
      break;
   case nir_intrinsic_load_ring_tess_offchip_offset_amd:
      replacement = ac_nir_load_arg(b, s->args, s->args->tess_offchip_offset);
      break;
   case nir_intrinsic_load_ring_tess_factors_offset_amd:
      replacement = ac_nir_load_arg(b, s->args, s->args->tcs_factor_offset);
      break;
   case nir_intrinsic_load_ring_es2gs_offset_amd:
      replacement = ac_nir_load_arg(b, s->args, s->args->es2gs_offset);
      break;
   case nir_intrinsic_load_ring_gs2vs_offset_amd:
      replacement = ac_nir_load_arg(b, s->args, s->args->gs2vs_offset);
      break;
   case nir_intrinsic_load_gs_vertex_offset_amd:
      replacement = ac_nir_load_arg(b, s->args, s->args->gs_vtx_offset[nir_intrinsic_base(intrin)]);
      break;
   case nir_intrinsic_load_streamout_config_amd:
      replacement = ac_nir_load_arg(b, s->args, s->args->streamout_config);
      break;
   case nir_intrinsic_load_streamout_write_index_amd:
      replacement = ac_nir_load_arg(b, s->args, s->args->streamout_write_index);
      break;
   case nir_intrinsic_load_streamout_offset_amd:
      replacement = ac_nir_load_arg(b, s->args, s->args->streamout_offset[nir_intrinsic_base(intrin)]);
      break;
   case nir_intrinsic_load_ring_attr_offset_amd: {
      nir_def *ring_attr_offset = ac_nir_load_arg(b, s->args, s->args->gs_attr_offset);
      replacement = nir_ishl_imm(b, nir_ubfe_imm(b, ring_attr_offset, 0, 15), 9); /* 512b increments. */
      break;
   }
   case nir_intrinsic_load_first_vertex:
      replacement = ac_nir_load_arg(b, s->args, s->args->base_vertex);
      break;
   case nir_intrinsic_load_base_instance:
      replacement = ac_nir_load_arg(b, s->args, s->args->start_instance);
      break;
   case nir_intrinsic_load_draw_id:
      replacement = ac_nir_load_arg(b, s->args, s->args->draw_id);
      break;
   case nir_intrinsic_load_view_index:
      replacement = ac_nir_load_arg_upper_bound(b, s->args, s->args->view_index, 1);
      break;
   case nir_intrinsic_load_invocation_id:
      if (b->shader->info.stage == MESA_SHADER_TESS_CTRL) {
         replacement = ac_nir_unpack_arg(b, s->args, s->args->tcs_rel_ids, 8, 5);
      } else if (b->shader->info.stage == MESA_SHADER_GEOMETRY) {
         if (s->gfx_level >= GFX12) {
            replacement = ac_nir_unpack_arg(b, s->args, s->args->gs_vtx_offset[0], 27, 5);
         } else if (s->gfx_level >= GFX10) {
            replacement = ac_nir_unpack_arg(b, s->args, s->args->gs_invocation_id, 0, 5);
         } else {
            replacement = ac_nir_load_arg_upper_bound(b, s->args, s->args->gs_invocation_id, 31);
         }
      } else {
         unreachable("unexpected shader stage");
      }
      break;
   case nir_intrinsic_load_sample_id:
      replacement = ac_nir_unpack_arg(b, s->args, s->args->ancillary, 8, 4);
      break;
   case nir_intrinsic_load_sample_pos:
      replacement = nir_vec2(b, nir_ffract(b, ac_nir_load_arg(b, s->args, s->args->frag_pos[0])),
                             nir_ffract(b, ac_nir_load_arg(b, s->args, s->args->frag_pos[1])));
      break;
   case nir_intrinsic_load_frag_shading_rate: {
      /* VRS Rate X = Ancillary[2:3]
       * VRS Rate Y = Ancillary[4:5]
       */
      nir_def *x_rate = ac_nir_unpack_arg(b, s->args, s->args->ancillary, 2, 2);
      nir_def *y_rate = ac_nir_unpack_arg(b, s->args, s->args->ancillary, 4, 2);

      /* xRate = xRate == 0x1 ? Horizontal2Pixels : None. */
      x_rate = nir_bcsel(b, nir_ieq_imm(b, x_rate, 1), nir_imm_int(b, 4), nir_imm_int(b, 0));

      /* yRate = yRate == 0x1 ? Vertical2Pixels : None. */
      y_rate = nir_bcsel(b, nir_ieq_imm(b, y_rate, 1), nir_imm_int(b, 1), nir_imm_int(b, 0));
      replacement = nir_ior(b, x_rate, y_rate);
      break;
   }
   case nir_intrinsic_load_front_face:
      replacement = nir_fgt_imm(b, ac_nir_load_arg(b, s->args, s->args->front_face), 0);
      break;
   case nir_intrinsic_load_front_face_fsign:
      replacement = ac_nir_load_arg(b, s->args, s->args->front_face);
      break;
   case nir_intrinsic_load_layer_id:
      replacement = ac_nir_unpack_arg(b, s->args, s->args->ancillary,
                                      16, s->gfx_level >= GFX12 ? 14 : 13);
      break;
   case nir_intrinsic_load_barycentric_optimize_amd: {
      nir_def *prim_mask = ac_nir_load_arg(b, s->args, s->args->prim_mask);
      /* enabled when bit 31 is set */
      replacement = nir_ilt_imm(b, prim_mask, 0);
      break;
   }
   case nir_intrinsic_load_barycentric_pixel:
      if (nir_intrinsic_interp_mode(intrin) == INTERP_MODE_NOPERSPECTIVE)
         replacement = ac_nir_load_arg(b, s->args, s->args->linear_center);
      else
         replacement = ac_nir_load_arg(b, s->args, s->args->persp_center);
      nir_intrinsic_set_flags(nir_instr_as_intrinsic(replacement->parent_instr),
                              AC_VECTOR_ARG_FLAG(AC_VECTOR_ARG_INTERP_MODE,
                                                 nir_intrinsic_interp_mode(intrin)));
      break;
   case nir_intrinsic_load_barycentric_centroid:
      if (nir_intrinsic_interp_mode(intrin) == INTERP_MODE_NOPERSPECTIVE)
         replacement = ac_nir_load_arg(b, s->args, s->args->linear_centroid);
      else
         replacement = ac_nir_load_arg(b, s->args, s->args->persp_centroid);
      nir_intrinsic_set_flags(nir_instr_as_intrinsic(replacement->parent_instr),
                              AC_VECTOR_ARG_FLAG(AC_VECTOR_ARG_INTERP_MODE,
                                                 nir_intrinsic_interp_mode(intrin)));
      break;
   case nir_intrinsic_load_barycentric_sample:
      if (nir_intrinsic_interp_mode(intrin) == INTERP_MODE_NOPERSPECTIVE)
         replacement = ac_nir_load_arg(b, s->args, s->args->linear_sample);
      else
         replacement = ac_nir_load_arg(b, s->args, s->args->persp_sample);
      nir_intrinsic_set_flags(nir_instr_as_intrinsic(replacement->parent_instr),
                              AC_VECTOR_ARG_FLAG(AC_VECTOR_ARG_INTERP_MODE,
                                                 nir_intrinsic_interp_mode(intrin)));
      break;
   case nir_intrinsic_load_barycentric_model:
      replacement = ac_nir_load_arg(b, s->args, s->args->pull_model);
      break;
   case nir_intrinsic_load_barycentric_at_offset: {
      nir_def *baryc = nir_intrinsic_interp_mode(intrin) == INTERP_MODE_NOPERSPECTIVE ?
                          ac_nir_load_arg(b, s->args, s->args->linear_center) :
                          ac_nir_load_arg(b, s->args, s->args->persp_center);
      nir_def *i = nir_channel(b, baryc, 0);
      nir_def *j = nir_channel(b, baryc, 1);
      nir_def *offset_x = nir_channel(b, intrin->src[0].ssa, 0);
      nir_def *offset_y = nir_channel(b, intrin->src[0].ssa, 1);
      nir_def *ddx_i = nir_ddx(b, i);
      nir_def *ddx_j = nir_ddx(b, j);
      nir_def *ddy_i = nir_ddy(b, i);
      nir_def *ddy_j = nir_ddy(b, j);

      /* Interpolate standard barycentrics by offset. */
      nir_def *offset_i = nir_ffma(b, ddy_i, offset_y, nir_ffma(b, ddx_i, offset_x, i));
      nir_def *offset_j = nir_ffma(b, ddy_j, offset_y, nir_ffma(b, ddx_j, offset_x, j));
      replacement = nir_vec2(b, offset_i, offset_j);
      break;
   }
   case nir_intrinsic_load_gs_wave_id_amd:
      if (s->args->merged_wave_info.used)
         replacement = ac_nir_unpack_arg(b, s->args, s->args->merged_wave_info, 16, 8);
      else if (s->args->gs_wave_id.used)
         replacement = ac_nir_load_arg(b, s->args, s->args->gs_wave_id);
      else
         unreachable("Shader doesn't have GS wave ID.");
      break;
   case nir_intrinsic_overwrite_vs_arguments_amd:
      s->vertex_id = intrin->src[0].ssa;
      s->instance_id = intrin->src[1].ssa;
      nir_instr_remove(instr);
      return true;
   case nir_intrinsic_overwrite_tes_arguments_amd:
      s->tes_u = intrin->src[0].ssa;
      s->tes_v = intrin->src[1].ssa;
      s->tes_patch_id = intrin->src[2].ssa;
      s->tes_rel_patch_id = intrin->src[3].ssa;
      nir_instr_remove(instr);
      return true;
   case nir_intrinsic_load_vertex_id_zero_base:
      if (!s->vertex_id)
         s->vertex_id = preload_arg(s, b->impl, s->args->vertex_id, s->args->tcs_patch_id, 0);
      replacement = s->vertex_id;
      break;
   case nir_intrinsic_load_instance_id:
      if (!s->instance_id)
         s->instance_id = preload_arg(s, b->impl, s->args->instance_id, s->args->vertex_id, 0);
      replacement = s->instance_id;
      break;
   case nir_intrinsic_load_tess_rel_patch_id_amd:
      if (b->shader->info.stage == MESA_SHADER_TESS_CTRL) {
         replacement = ac_nir_unpack_arg(b, s->args, s->args->tcs_rel_ids, 0, 8);
      } else if (b->shader->info.stage == MESA_SHADER_TESS_EVAL) {
         if (s->tes_rel_patch_id) {
            replacement = s->tes_rel_patch_id;
         } else {
            replacement = ac_nir_load_arg(b, s->args, s->args->tes_rel_patch_id);
            if (b->shader->info.tess.tcs_vertices_out) {
               /* Setting an upper bound like this will actually make it possible
                * to optimize some multiplications (in address calculations) so that
                * constant additions can be added to the const offset in memory load instructions.
                */
               nir_intrinsic_set_arg_upper_bound_u32_amd(nir_instr_as_intrinsic(replacement->parent_instr),
                                                         2048 / b->shader->info.tess.tcs_vertices_out);
            }
         }
      } else {
         unreachable("invalid stage");
      }
      break;
   case nir_intrinsic_load_primitive_id:
      if (b->shader->info.stage == MESA_SHADER_GEOMETRY) {
         replacement = ac_nir_load_arg(b, s->args, s->args->gs_prim_id);
      } else if (b->shader->info.stage == MESA_SHADER_TESS_CTRL) {
         replacement = ac_nir_load_arg(b, s->args, s->args->tcs_patch_id);
      } else if (b->shader->info.stage == MESA_SHADER_TESS_EVAL) {
         replacement = s->tes_patch_id ? s->tes_patch_id :
                                         ac_nir_load_arg(b, s->args, s->args->tes_patch_id);
      } else if (b->shader->info.stage == MESA_SHADER_VERTEX) {
         if (s->hw_stage == AC_HW_VERTEX_SHADER)
            replacement = ac_nir_load_arg(b, s->args, s->args->vs_prim_id); /* legacy */
         else
            replacement = ac_nir_load_arg(b, s->args, s->args->gs_prim_id); /* NGG */
      } else {
         unreachable("invalid stage");
      }
      break;
   case nir_intrinsic_load_tess_coord: {
      nir_def *coord[3] = {
         s->tes_u ? s->tes_u : ac_nir_load_arg(b, s->args, s->args->tes_u),
         s->tes_v ? s->tes_v : ac_nir_load_arg(b, s->args, s->args->tes_v),
         nir_imm_float(b, 0),
      };

      /* For triangles, the vector should be (u, v, 1-u-v). */
      if (b->shader->info.tess._primitive_mode == TESS_PRIMITIVE_TRIANGLES)
         coord[2] = nir_fsub(b, nir_imm_float(b, 1), nir_fadd(b, coord[0], coord[1]));
      replacement = nir_vec(b, coord, 3);
      break;
   }
   case nir_intrinsic_load_local_invocation_index:
      /* GFX11 HS has subgroup_id, so use it instead of vs_rel_patch_id. */
      if (s->gfx_level < GFX11 &&
          (s->hw_stage == AC_HW_LOCAL_SHADER || s->hw_stage == AC_HW_HULL_SHADER)) {
         if (!s->vs_rel_patch_id) {
            s->vs_rel_patch_id = preload_arg(s, b->impl, s->args->vs_rel_patch_id,
                                             s->args->tcs_rel_ids, 255);
         }
         replacement = s->vs_rel_patch_id;
      } else if (s->workgroup_size <= s->wave_size) {
         /* Just a subgroup invocation ID. */
         replacement = nir_mbcnt_amd(b, nir_imm_intN_t(b, ~0ull, s->wave_size), nir_imm_int(b, 0));
      } else if (s->gfx_level < GFX12 && s->hw_stage == AC_HW_COMPUTE_SHADER && s->wave_size == 64) {
         /* After the AND the bits are already multiplied by 64 (left shifted by 6) so we can just
          * feed that to mbcnt. (GFX12 doesn't have tg_size)
          */
         nir_def *wave_id_mul_64 = nir_iand_imm(b, ac_nir_load_arg(b, s->args, s->args->tg_size), 0xfc0);
         replacement = nir_mbcnt_amd(b, nir_imm_intN_t(b, ~0ull, s->wave_size), wave_id_mul_64);
      } else {
         replacement = nir_mbcnt_amd(b, nir_imm_intN_t(b, ~0ull, s->wave_size),
                                     nir_imul_imm(b, load_subgroup_id_lowered(s, b), s->wave_size));
      }
      break;
   case nir_intrinsic_load_subgroup_invocation:
      replacement = nir_mbcnt_amd(b, nir_imm_intN_t(b, ~0ull, s->wave_size), nir_imm_int(b, 0));
      break;
   default:
      return false;
   }

   assert(replacement);
   nir_def_replace(&intrin->def, replacement);
   return true;
}

bool
ac_nir_lower_intrinsics_to_args(nir_shader *shader, const enum amd_gfx_level gfx_level,
                                bool has_ls_vgpr_init_bug, const enum ac_hw_stage hw_stage,
                                unsigned wave_size, unsigned workgroup_size,
                                const struct ac_shader_args *ac_args)
{
   lower_intrinsics_to_args_state state = {
      .gfx_level = gfx_level,
      .hw_stage = hw_stage,
      .has_ls_vgpr_init_bug = has_ls_vgpr_init_bug,
      .wave_size = wave_size,
      .workgroup_size = workgroup_size,
      .args = ac_args,
   };

   return nir_shader_instructions_pass(shader, lower_intrinsic_to_arg,
                                       nir_metadata_control_flow, &state);
}

void
ac_nir_store_var_components(nir_builder *b, nir_variable *var, nir_def *value,
                            unsigned component, unsigned writemask)
{
   /* component store */
   if (value->num_components != 4) {
      nir_def *undef = nir_undef(b, 1, value->bit_size);

      /* add undef component before and after value to form a vec4 */
      nir_def *comp[4];
      for (int i = 0; i < 4; i++) {
         comp[i] = (i >= component && i < component + value->num_components) ?
            nir_channel(b, value, i - component) : undef;
      }

      value = nir_vec(b, comp, 4);
      writemask <<= component;
   } else {
      /* if num_component==4, there should be no component offset */
      assert(component == 0);
   }

   nir_store_var(b, var, value, writemask);
}

/* Process the given store_output intrinsic and process its information.
 * Meant to be used for VS/TES/GS when they are the last pre-rasterization stage.
 *
 * Assumptions:
 * - We called nir_lower_io_to_temporaries on the shader
 * - 64-bit outputs are lowered
 * - no indirect indexing is present
 */
void ac_nir_gather_prerast_store_output_info(nir_builder *b, nir_intrinsic_instr *intrin, ac_nir_prerast_out *out)
{
   assert(intrin->intrinsic == nir_intrinsic_store_output);
   assert(nir_src_is_const(intrin->src[1]) && !nir_src_as_uint(intrin->src[1]));

   const nir_io_semantics io_sem = nir_intrinsic_io_semantics(intrin);
   const unsigned slot = io_sem.location;

   nir_def *store_val = intrin->src[0].ssa;
   assert(store_val->bit_size == 16 || store_val->bit_size == 32);

   nir_def **output;
   nir_alu_type *type;
   ac_nir_prerast_per_output_info *info;

   if (slot >= VARYING_SLOT_VAR0_16BIT) {
      const unsigned index = slot - VARYING_SLOT_VAR0_16BIT;

      if (io_sem.high_16bits) {
         output = out->outputs_16bit_hi[index];
         type = out->types_16bit_hi[index];
         info = &out->infos_16bit_hi[index];
      } else {
         output = out->outputs_16bit_lo[index];
         type = out->types_16bit_lo[index];
         info = &out->infos_16bit_lo[index];
      }
   } else {
      output = out->outputs[slot];
      type = out->types[slot];
      info = &out->infos[slot];
   }

   unsigned component_offset = nir_intrinsic_component(intrin);
   unsigned write_mask = nir_intrinsic_write_mask(intrin);
   nir_alu_type src_type = nir_intrinsic_src_type(intrin);
   assert(nir_alu_type_get_type_size(src_type) == store_val->bit_size);

   b->cursor = nir_before_instr(&intrin->instr);

   /* 16-bit output stored in a normal varying slot that isn't a dedicated 16-bit slot. */
   const bool non_dedicated_16bit = slot < VARYING_SLOT_VAR0_16BIT && store_val->bit_size == 16;

   u_foreach_bit (i, write_mask) {
      const unsigned stream = (io_sem.gs_streams >> (i * 2)) & 0x3;

      if (b->shader->info.stage == MESA_SHADER_GEOMETRY) {
         if (!(b->shader->info.gs.active_stream_mask & (1 << stream)))
            continue;
      }

      const unsigned c = component_offset + i;

      /* The same output component should always belong to the same stream. */
      assert(!(info->components_mask & (1 << c)) ||
             ((info->stream >> (c * 2)) & 3) == stream);

      /* Components of the same output slot may belong to different streams. */
      info->stream |= stream << (c * 2);
      info->components_mask |= BITFIELD_BIT(c);

      if (!io_sem.no_varying)
         info->as_varying_mask |= BITFIELD_BIT(c);
      if (!io_sem.no_sysval_output)
         info->as_sysval_mask |= BITFIELD_BIT(c);

      nir_def *store_component = nir_channel(b, intrin->src[0].ssa, i);

      if (non_dedicated_16bit) {
         if (io_sem.high_16bits) {
            nir_def *lo = output[c] ? nir_unpack_32_2x16_split_x(b, output[c]) : nir_imm_intN_t(b, 0, 16);
            output[c] = nir_pack_32_2x16_split(b, lo, store_component);
         } else {
            nir_def *hi = output[c] ? nir_unpack_32_2x16_split_y(b, output[c]) : nir_imm_intN_t(b, 0, 16);
            output[c] = nir_pack_32_2x16_split(b, store_component, hi);
         }
         type[c] = nir_type_uint32;
      } else {
         output[c] = store_component;
         type[c] = src_type;
      }
   }
}

static nir_intrinsic_instr *
export(nir_builder *b, nir_def *val, nir_def *row, unsigned base, unsigned flags,
       unsigned write_mask)
{
   if (row) {
      return nir_export_row_amd(b, val, row, .base = base, .flags = flags,
                                .write_mask = write_mask);
   } else {
      return nir_export_amd(b, val, .base = base, .flags = flags,
                            .write_mask = write_mask);
   }
}

void
ac_nir_export_primitive(nir_builder *b, nir_def *prim, nir_def *row)
{
   unsigned write_mask = BITFIELD_MASK(prim->num_components);

   export(b, nir_pad_vec4(b, prim), row, V_008DFC_SQ_EXP_PRIM, AC_EXP_FLAG_DONE,
          write_mask);
}

static nir_def *
get_export_output(nir_builder *b, nir_def **output)
{
   nir_def *vec[4];
   for (int i = 0; i < 4; i++) {
      if (output[i])
         vec[i] = nir_u2uN(b, output[i], 32);
      else
         vec[i] = nir_undef(b, 1, 32);
   }

   return nir_vec(b, vec, 4);
}

static nir_def *
get_pos0_output(nir_builder *b, nir_def **output)
{
   /* Some applications don't write position but expect (0, 0, 0, 1)
    * so use that value instead of undef when it isn't written.
    */
   nir_def *vec[4] = {0};

   for (int i = 0; i < 4; i++) {
      if (output[i])
         vec[i] = nir_u2u32(b, output[i]);
     else
         vec[i] = nir_imm_float(b, i == 3 ? 1.0 : 0.0);
   }

   return nir_vec(b, vec, 4);
}

void
ac_nir_export_position(nir_builder *b,
                       enum amd_gfx_level gfx_level,
                       uint32_t clip_cull_mask,
                       bool no_param_export,
                       bool force_vrs,
                       bool done,
                       uint64_t outputs_written,
                       ac_nir_prerast_out *out,
                       nir_def *row)
{
   nir_intrinsic_instr *exp[4];
   unsigned exp_num = 0;
   unsigned exp_pos_offset = 0;

   if (outputs_written & VARYING_BIT_POS) {
      /* GFX10 (Navi1x) skip POS0 exports if EXEC=0 and DONE=0, causing a hang.
      * Setting valid_mask=1 prevents it and has no other effect.
      */
      const unsigned pos_flags = gfx_level == GFX10 ? AC_EXP_FLAG_VALID_MASK : 0;
      nir_def *pos = get_pos0_output(b, out->outputs[VARYING_SLOT_POS]);

      exp[exp_num] = export(b, pos, row, V_008DFC_SQ_EXP_POS + exp_num, pos_flags, 0xf);
      exp_num++;
   } else {
      exp_pos_offset++;
   }

   uint64_t mask =
      VARYING_BIT_PSIZ |
      VARYING_BIT_EDGE |
      VARYING_BIT_LAYER |
      VARYING_BIT_VIEWPORT |
      VARYING_BIT_PRIMITIVE_SHADING_RATE;

   /* clear output mask if no one written */
   if (!out->outputs[VARYING_SLOT_PSIZ][0] || !out->infos[VARYING_SLOT_PSIZ].as_sysval_mask)
      outputs_written &= ~VARYING_BIT_PSIZ;
   if (!out->outputs[VARYING_SLOT_EDGE][0] || !out->infos[VARYING_SLOT_EDGE].as_sysval_mask)
      outputs_written &= ~VARYING_BIT_EDGE;
   if (!out->outputs[VARYING_SLOT_PRIMITIVE_SHADING_RATE][0] || !out->infos[VARYING_SLOT_PRIMITIVE_SHADING_RATE].as_sysval_mask)
      outputs_written &= ~VARYING_BIT_PRIMITIVE_SHADING_RATE;
   if (!out->outputs[VARYING_SLOT_LAYER][0] || !out->infos[VARYING_SLOT_LAYER].as_sysval_mask)
      outputs_written &= ~VARYING_BIT_LAYER;
   if (!out->outputs[VARYING_SLOT_VIEWPORT][0] || !out->infos[VARYING_SLOT_VIEWPORT].as_sysval_mask)
      outputs_written &= ~VARYING_BIT_VIEWPORT;

   if ((outputs_written & mask) || force_vrs) {
      nir_def *zero = nir_imm_float(b, 0);
      nir_def *vec[4] = { zero, zero, zero, zero };
      unsigned write_mask = 0;

      if (outputs_written & VARYING_BIT_PSIZ) {
         vec[0] = out->outputs[VARYING_SLOT_PSIZ][0];
         write_mask |= BITFIELD_BIT(0);
      }

      if (outputs_written & VARYING_BIT_EDGE) {
         vec[1] = nir_umin(b, out->outputs[VARYING_SLOT_EDGE][0], nir_imm_int(b, 1));
         write_mask |= BITFIELD_BIT(1);
      }

      nir_def *rates = NULL;
      if (outputs_written & VARYING_BIT_PRIMITIVE_SHADING_RATE) {
         rates = out->outputs[VARYING_SLOT_PRIMITIVE_SHADING_RATE][0];
      } else if (force_vrs) {
         /* If Pos.W != 1 (typical for non-GUI elements), use coarse shading. */
         nir_def *pos_w = out->outputs[VARYING_SLOT_POS][3];
         pos_w = pos_w ? nir_u2u32(b, pos_w) : nir_imm_float(b, 1.0);
         nir_def *cond = nir_fneu_imm(b, pos_w, 1);
         rates = nir_bcsel(b, cond, nir_load_force_vrs_rates_amd(b), nir_imm_int(b, 0));
      }

      if (rates) {
         vec[1] = nir_ior(b, vec[1], rates);
         write_mask |= BITFIELD_BIT(1);
      }

      if (outputs_written & VARYING_BIT_LAYER) {
         vec[2] = out->outputs[VARYING_SLOT_LAYER][0];
         write_mask |= BITFIELD_BIT(2);
      }

      if (outputs_written & VARYING_BIT_VIEWPORT) {
         if (gfx_level >= GFX9) {
            /* GFX9 has the layer in [10:0] and the viewport index in [19:16]. */
            nir_def *v = nir_ishl_imm(b, out->outputs[VARYING_SLOT_VIEWPORT][0], 16);
            vec[2] = nir_ior(b, vec[2], v);
            write_mask |= BITFIELD_BIT(2);
         } else {
            vec[3] = out->outputs[VARYING_SLOT_VIEWPORT][0];
            write_mask |= BITFIELD_BIT(3);
         }
      }

      exp[exp_num] = export(b, nir_vec(b, vec, 4), row,
                            V_008DFC_SQ_EXP_POS + exp_num + exp_pos_offset,
                            0, write_mask);
      exp_num++;
   }

   for (int i = 0; i < 2; i++) {
      if ((outputs_written & (VARYING_BIT_CLIP_DIST0 << i)) &&
          (clip_cull_mask & BITFIELD_RANGE(i * 4, 4))) {
         exp[exp_num] = export(
            b, get_export_output(b, out->outputs[VARYING_SLOT_CLIP_DIST0 + i]), row,
            V_008DFC_SQ_EXP_POS + exp_num + exp_pos_offset, 0,
            (clip_cull_mask >> (i * 4)) & 0xf);
         exp_num++;
      }
   }

   if (outputs_written & VARYING_BIT_CLIP_VERTEX) {
      nir_def *vtx = get_export_output(b, out->outputs[VARYING_SLOT_CLIP_VERTEX]);

      /* Clip distance for clip vertex to each user clip plane. */
      nir_def *clip_dist[8] = {0};
      u_foreach_bit (i, clip_cull_mask) {
         nir_def *ucp = nir_load_user_clip_plane(b, .ucp_id = i);
         clip_dist[i] = nir_fdot4(b, vtx, ucp);
      }

      for (int i = 0; i < 2; i++) {
         if (clip_cull_mask & BITFIELD_RANGE(i * 4, 4)) {
            exp[exp_num] = export(
               b, get_export_output(b, clip_dist + i * 4), row,
               V_008DFC_SQ_EXP_POS + exp_num + exp_pos_offset, 0,
               (clip_cull_mask >> (i * 4)) & 0xf);
            exp_num++;
         }
      }
   }

   if (!exp_num)
      return;

   nir_intrinsic_instr *final_exp = exp[exp_num - 1];

   if (done) {
      /* Specify that this is the last export */
      const unsigned final_exp_flags = nir_intrinsic_flags(final_exp);
      nir_intrinsic_set_flags(final_exp, final_exp_flags | AC_EXP_FLAG_DONE);
   }

   /* If a shader has no param exports, rasterization can start before
    * the shader finishes and thus memory stores might not finish before
    * the pixel shader starts.
    */
   if (gfx_level >= GFX10 && no_param_export && b->shader->info.writes_memory) {
      nir_cursor cursor = b->cursor;
      b->cursor = nir_before_instr(&final_exp->instr);
      nir_scoped_memory_barrier(b, SCOPE_DEVICE, NIR_MEMORY_RELEASE,
                                nir_var_mem_ssbo | nir_var_mem_global | nir_var_image);
      b->cursor = cursor;
   }
}

void
ac_nir_export_parameters(nir_builder *b,
                         const uint8_t *param_offsets,
                         uint64_t outputs_written,
                         uint16_t outputs_written_16bit,
                         ac_nir_prerast_out *out)
{
   uint32_t exported_params = 0;

   u_foreach_bit64 (slot, outputs_written) {
      unsigned offset = param_offsets[slot];
      if (offset > AC_EXP_PARAM_OFFSET_31)
         continue;

      uint32_t write_mask = 0;
      for (int i = 0; i < 4; i++) {
         if (out->outputs[slot][i])
            write_mask |= (out->infos[slot].as_varying_mask & BITFIELD_BIT(i));
      }

      /* no one set this output slot, we can skip the param export */
      if (!write_mask)
         continue;

      /* Since param_offsets[] can map multiple varying slots to the same
       * param export index (that's radeonsi-specific behavior), we need to
       * do this so as not to emit duplicated exports.
       */
      if (exported_params & BITFIELD_BIT(offset))
         continue;

      nir_export_amd(
         b, get_export_output(b, out->outputs[slot]),
         .base = V_008DFC_SQ_EXP_PARAM + offset,
         .write_mask = write_mask);
      exported_params |= BITFIELD_BIT(offset);
   }

   u_foreach_bit (slot, outputs_written_16bit) {
      unsigned offset = param_offsets[VARYING_SLOT_VAR0_16BIT + slot];
      if (offset > AC_EXP_PARAM_OFFSET_31)
         continue;

      uint32_t write_mask = 0;
      for (int i = 0; i < 4; i++) {
         if (out->outputs_16bit_lo[slot][i] || out->outputs_16bit_hi[slot][i])
            write_mask |= BITFIELD_BIT(i);
      }

      /* no one set this output slot, we can skip the param export */
      if (!write_mask)
         continue;

      /* Since param_offsets[] can map multiple varying slots to the same
       * param export index (that's radeonsi-specific behavior), we need to
       * do this so as not to emit duplicated exports.
       */
      if (exported_params & BITFIELD_BIT(offset))
         continue;

      nir_def *vec[4];
      nir_def *undef = nir_undef(b, 1, 16);
      for (int i = 0; i < 4; i++) {
         nir_def *lo = out->outputs_16bit_lo[slot][i] ? out->outputs_16bit_lo[slot][i] : undef;
         nir_def *hi = out->outputs_16bit_hi[slot][i] ? out->outputs_16bit_hi[slot][i] : undef;
         vec[i] = nir_pack_32_2x16_split(b, lo, hi);
      }

      nir_export_amd(
         b, nir_vec(b, vec, 4),
         .base = V_008DFC_SQ_EXP_PARAM + offset,
         .write_mask = write_mask);
      exported_params |= BITFIELD_BIT(offset);
   }
}

void
ac_nir_store_parameters_to_attr_ring(nir_builder *b,
                                     const uint8_t *param_offsets,
                                     const uint64_t outputs_written,
                                     const uint16_t outputs_written_16bit,
                                     ac_nir_prerast_out *out,
                                     nir_def *export_tid, nir_def *num_export_threads)
{
   nir_def *attr_rsrc = nir_load_ring_attr_amd(b);

   /* We should always store full vec4s in groups of 8 lanes for the best performance even if
    * some of them are garbage or have unused components, so align the number of export threads
    * to 8.
    */
   num_export_threads = nir_iand_imm(b, nir_iadd_imm(b, num_export_threads, 7), ~7);

   if (!export_tid)
      nir_push_if(b, nir_is_subgroup_invocation_lt_amd(b, num_export_threads));
   else
      nir_push_if(b, nir_ult(b, export_tid, num_export_threads));

   nir_def *attr_offset = nir_load_ring_attr_offset_amd(b);
   nir_def *vindex = nir_load_local_invocation_index(b);
   nir_def *voffset = nir_imm_int(b, 0);
   nir_def *undef = nir_undef(b, 1, 32);

   uint32_t exported_params = 0;

   u_foreach_bit64 (slot, outputs_written) {
      const unsigned offset = param_offsets[slot];

      if (offset > AC_EXP_PARAM_OFFSET_31)
         continue;

      if (!out->infos[slot].as_varying_mask)
         continue;

      if (exported_params & BITFIELD_BIT(offset))
         continue;

      nir_def *comp[4];
      for (unsigned j = 0; j < 4; j++) {
         comp[j] = out->outputs[slot][j] ? out->outputs[slot][j] : undef;
      }

      nir_store_buffer_amd(b, nir_vec(b, comp, 4), attr_rsrc, voffset, attr_offset, vindex,
                           .base = offset * 16,
                           .memory_modes = nir_var_shader_out,
                           .access = ACCESS_COHERENT | ACCESS_IS_SWIZZLED_AMD);

      exported_params |= BITFIELD_BIT(offset);
   }

   u_foreach_bit (i, outputs_written_16bit) {
      const unsigned offset = param_offsets[VARYING_SLOT_VAR0_16BIT + i];

      if (offset > AC_EXP_PARAM_OFFSET_31)
         continue;

      if (!out->infos_16bit_lo[i].as_varying_mask &&
          !out->infos_16bit_hi[i].as_varying_mask)
         continue;

      if (exported_params & BITFIELD_BIT(offset))
         continue;

      nir_def *comp[4];
      for (unsigned j = 0; j < 4; j++) {
         nir_def *lo = out->outputs_16bit_lo[i][j] ? out->outputs_16bit_lo[i][j] : undef;
         nir_def *hi = out->outputs_16bit_hi[i][j] ? out->outputs_16bit_hi[i][j] : undef;
         comp[j] = nir_pack_32_2x16_split(b, lo, hi);
      }

      nir_store_buffer_amd(b, nir_vec(b, comp, 4), attr_rsrc, voffset, attr_offset, vindex,
                           .base = offset * 16,
                           .memory_modes = nir_var_shader_out,
                           .access = ACCESS_COHERENT | ACCESS_IS_SWIZZLED_AMD);

      exported_params |= BITFIELD_BIT(offset);
   }

   nir_pop_if(b, NULL);
}

unsigned
ac_nir_map_io_location(unsigned location,
                       uint64_t mask,
                       ac_nir_map_io_driver_location map_io)
{
   /* Unlinked shaders:
    * We are unaware of the inputs of the next stage while lowering outputs.
    * The driver needs to pass a callback to map varyings to a fixed location.
    */
   if (map_io)
      return map_io(location);

   /* Linked shaders:
    * Take advantage of knowledge of the inputs of the next stage when lowering outputs.
    * Map varyings to a prefix sum of the IO mask to save space in LDS or VRAM.
    */
   assert(mask & BITFIELD64_BIT(location));
   return util_bitcount64(mask & BITFIELD64_MASK(location));
}

/**
 * This function takes an I/O intrinsic like load/store_input,
 * and emits a sequence that calculates the full offset of that instruction,
 * including a stride to the base and component offsets.
 */
nir_def *
ac_nir_calc_io_off(nir_builder *b,
                             nir_intrinsic_instr *intrin,
                             nir_def *base_stride,
                             unsigned component_stride,
                             unsigned mapped_driver_location)
{
   /* base is the driver_location, which is in slots (1 slot = 4x4 bytes) */
   nir_def *base_op = nir_imul_imm(b, base_stride, mapped_driver_location);

   /* offset should be interpreted in relation to the base,
    * so the instruction effectively reads/writes another input/output
    * when it has an offset
    */
   nir_def *offset_op = nir_imul(b, base_stride,
                                 nir_get_io_offset_src(intrin)->ssa);

   /* component is in bytes */
   unsigned const_op = nir_intrinsic_component(intrin) * component_stride;

   return nir_iadd_imm_nuw(b, nir_iadd_nuw(b, base_op, offset_op), const_op);
}

bool
ac_nir_lower_indirect_derefs(nir_shader *shader,
                             enum amd_gfx_level gfx_level)
{
   bool progress = false;

   /* TODO: Don't lower convergent VGPR indexing because the hw can do it. */

   /* Lower large variables to scratch first so that we won't bloat the
    * shader by generating large if ladders for them.
    */
   NIR_PASS(progress, shader, nir_lower_vars_to_scratch, nir_var_function_temp, 256,
            glsl_get_natural_size_align_bytes, glsl_get_natural_size_align_bytes);

   /* This lowers indirect indexing to if-else ladders. */
   NIR_PASS(progress, shader, nir_lower_indirect_derefs, nir_var_function_temp, UINT32_MAX);
   return progress;
}

static nir_def **
get_output_and_type(ac_nir_prerast_out *out, unsigned slot, bool high_16bits,
                    nir_alu_type **types)
{
   nir_def **data;
   nir_alu_type *type;

   /* Only VARYING_SLOT_VARn_16BIT slots need output type to convert 16bit output
    * to 32bit. Vulkan is not allowed to streamout output less than 32bit.
    */
   if (slot < VARYING_SLOT_VAR0_16BIT) {
      data = out->outputs[slot];
      type = NULL;
   } else {
      unsigned index = slot - VARYING_SLOT_VAR0_16BIT;

      if (high_16bits) {
         data = out->outputs_16bit_hi[index];
         type = out->types_16bit_hi[index];
      } else {
         data = out->outputs[index];
         type = out->types_16bit_lo[index];
      }
   }

   *types = type;
   return data;
}

static void
emit_streamout(nir_builder *b, unsigned stream, nir_xfb_info *info, ac_nir_prerast_out *out)
{
   nir_def *so_vtx_count = nir_ubfe_imm(b, nir_load_streamout_config_amd(b), 16, 7);
   nir_def *tid = nir_load_subgroup_invocation(b);

   nir_push_if(b, nir_ilt(b, tid, so_vtx_count));
   nir_def *so_write_index = nir_load_streamout_write_index_amd(b);

   nir_def *so_buffers[NIR_MAX_XFB_BUFFERS];
   nir_def *so_write_offset[NIR_MAX_XFB_BUFFERS];
   u_foreach_bit(i, info->buffers_written) {
      so_buffers[i] = nir_load_streamout_buffer_amd(b, i);

      unsigned stride = info->buffers[i].stride;
      nir_def *offset = nir_load_streamout_offset_amd(b, i);
      offset = nir_iadd(b, nir_imul_imm(b, nir_iadd(b, so_write_index, tid), stride),
                        nir_imul_imm(b, offset, 4));
      so_write_offset[i] = offset;
   }

   nir_def *undef = nir_undef(b, 1, 32);
   for (unsigned i = 0; i < info->output_count; i++) {
      const nir_xfb_output_info *output = info->outputs + i;
      if (stream != info->buffer_to_stream[output->buffer])
         continue;

      nir_alu_type *output_type;
      nir_def **output_data =
         get_output_and_type(out, output->location, output->high_16bits, &output_type);

      nir_def *vec[4] = {undef, undef, undef, undef};
      uint8_t mask = 0;
      u_foreach_bit(j, output->component_mask) {
         nir_def *data = output_data[j];

         if (data) {
            if (data->bit_size < 32) {
               /* we need output type to convert non-32bit output to 32bit */
               assert(output_type);

               nir_alu_type base_type = nir_alu_type_get_base_type(output_type[j]);
               data = nir_convert_to_bit_size(b, data, base_type, 32);
            }

            unsigned comp = j - output->component_offset;
            vec[comp] = data;
            mask |= 1 << comp;
         }
      }

      if (!mask)
         continue;

      unsigned buffer = output->buffer;
      nir_def *data = nir_vec(b, vec, util_last_bit(mask));
      nir_def *zero = nir_imm_int(b, 0);
      nir_store_buffer_amd(b, data, so_buffers[buffer], so_write_offset[buffer], zero, zero,
                           .base = output->offset, .write_mask = mask,
                           .access = ACCESS_COHERENT | ACCESS_NON_TEMPORAL);
   }

   nir_pop_if(b, NULL);
}

nir_shader *
ac_nir_create_gs_copy_shader(const nir_shader *gs_nir,
                             enum amd_gfx_level gfx_level,
                             uint32_t clip_cull_mask,
                             const uint8_t *param_offsets,
                             bool has_param_exports,
                             bool disable_streamout,
                             bool kill_pointsize,
                             bool kill_layer,
                             bool force_vrs,
                             ac_nir_gs_output_info *output_info)
{
   nir_builder b = nir_builder_init_simple_shader(
      MESA_SHADER_VERTEX, gs_nir->options, "gs_copy");

   nir_foreach_shader_out_variable(var, gs_nir)
      nir_shader_add_variable(b.shader, nir_variable_clone(var, b.shader));

   b.shader->info.outputs_written = gs_nir->info.outputs_written;
   b.shader->info.outputs_written_16bit = gs_nir->info.outputs_written_16bit;

   nir_def *gsvs_ring = nir_load_ring_gsvs_amd(&b);

   nir_xfb_info *info = gs_nir->xfb_info;
   nir_def *stream_id = NULL;
   if (!disable_streamout && info)
      stream_id = nir_ubfe_imm(&b, nir_load_streamout_config_amd(&b), 24, 2);

   nir_def *vtx_offset = nir_imul_imm(&b, nir_load_vertex_id_zero_base(&b), 4);
   nir_def *zero = nir_imm_zero(&b, 1, 32);

   for (unsigned stream = 0; stream < 4; stream++) {
      if (stream > 0 && (!stream_id || !(info->streams_written & BITFIELD_BIT(stream))))
         continue;

      if (stream_id)
         nir_push_if(&b, nir_ieq_imm(&b, stream_id, stream));

      uint32_t offset = 0;
      ac_nir_prerast_out out = {0};
      if (output_info->types_16bit_lo)
         memcpy(&out.types_16bit_lo, output_info->types_16bit_lo, sizeof(out.types_16bit_lo));
      if (output_info->types_16bit_hi)
         memcpy(&out.types_16bit_hi, output_info->types_16bit_hi, sizeof(out.types_16bit_hi));

      u_foreach_bit64 (i, gs_nir->info.outputs_written) {
         const uint8_t usage_mask = output_info->varying_mask[i] | output_info->sysval_mask[i];
         out.infos[i].components_mask = usage_mask;
         out.infos[i].as_varying_mask = output_info->varying_mask[i];
         out.infos[i].as_sysval_mask = output_info->sysval_mask[i];

         u_foreach_bit (j, usage_mask) {
            if (((output_info->streams[i] >> (j * 2)) & 0x3) != stream)
               continue;

            out.outputs[i][j] =
               nir_load_buffer_amd(&b, 1, 32, gsvs_ring, vtx_offset, zero, zero,
                                   .base = offset,
                                   .access = ACCESS_COHERENT | ACCESS_NON_TEMPORAL);

            /* clamp legacy color output */
            if (i == VARYING_SLOT_COL0 || i == VARYING_SLOT_COL1 ||
                i == VARYING_SLOT_BFC0 || i == VARYING_SLOT_BFC1) {
               nir_def *color = out.outputs[i][j];
               nir_def *clamp = nir_load_clamp_vertex_color_amd(&b);
               out.outputs[i][j] = nir_bcsel(&b, clamp, nir_fsat(&b, color), color);
            }

            offset += gs_nir->info.gs.vertices_out * 16 * 4;
         }
      }

      u_foreach_bit (i, gs_nir->info.outputs_written_16bit) {
         out.infos_16bit_lo[i].components_mask = output_info->varying_mask_16bit_lo[i];
         out.infos_16bit_lo[i].as_varying_mask = output_info->varying_mask_16bit_lo[i];
         out.infos_16bit_hi[i].components_mask = output_info->varying_mask_16bit_hi[i];
         out.infos_16bit_hi[i].as_varying_mask = output_info->varying_mask_16bit_hi[i];

         for (unsigned j = 0; j < 4; j++) {
            out.infos[i].as_varying_mask = output_info->varying_mask[i];
            out.infos[i].as_sysval_mask = output_info->sysval_mask[i];

            bool has_lo_16bit = (output_info->varying_mask_16bit_lo[i] & (1 << j)) &&
               ((output_info->streams_16bit_lo[i] >> (j * 2)) & 0x3) == stream;
            bool has_hi_16bit = (output_info->varying_mask_16bit_hi[i] & (1 << j)) &&
               ((output_info->streams_16bit_hi[i] >> (j * 2)) & 0x3) == stream;
            if (!has_lo_16bit && !has_hi_16bit)
               continue;

            nir_def *data =
               nir_load_buffer_amd(&b, 1, 32, gsvs_ring, vtx_offset, zero, zero,
                                   .base = offset,
                                   .access = ACCESS_COHERENT | ACCESS_NON_TEMPORAL);

            if (has_lo_16bit)
               out.outputs_16bit_lo[i][j] = nir_unpack_32_2x16_split_x(&b, data);

            if (has_hi_16bit)
               out.outputs_16bit_hi[i][j] = nir_unpack_32_2x16_split_y(&b, data);

            offset += gs_nir->info.gs.vertices_out * 16 * 4;
         }
      }

      if (stream_id)
         emit_streamout(&b, stream, info, &out);

      if (stream == 0) {
         uint64_t export_outputs = b.shader->info.outputs_written | VARYING_BIT_POS;
         if (kill_pointsize)
            export_outputs &= ~VARYING_BIT_PSIZ;
         if (kill_layer)
            export_outputs &= ~VARYING_BIT_LAYER;

         ac_nir_export_position(&b, gfx_level, clip_cull_mask, !has_param_exports,
                                force_vrs, true, export_outputs, &out, NULL);

         if (has_param_exports) {
            ac_nir_export_parameters(&b, param_offsets,
                                     b.shader->info.outputs_written,
                                     b.shader->info.outputs_written_16bit,
                                     &out);
         }
      }

      if (stream_id)
         nir_push_else(&b, NULL);
   }

   b.shader->info.clip_distance_array_size = gs_nir->info.clip_distance_array_size;
   b.shader->info.cull_distance_array_size = gs_nir->info.cull_distance_array_size;

   return b.shader;
}

static void
gather_outputs(nir_builder *b, nir_function_impl *impl, ac_nir_prerast_out *out)
{
   /* Assume:
    * - the shader used nir_lower_io_to_temporaries
    * - 64-bit outputs are lowered
    * - no indirect indexing is present
    */
   nir_foreach_block (block, impl) {
      nir_foreach_instr_safe (instr, block) {
         if (instr->type != nir_instr_type_intrinsic)
            continue;

         nir_intrinsic_instr *intrin = nir_instr_as_intrinsic(instr);
         if (intrin->intrinsic != nir_intrinsic_store_output)
            continue;

         ac_nir_gather_prerast_store_output_info(b, intrin, out);
         nir_instr_remove(instr);
      }
   }
}

void
ac_nir_lower_legacy_vs(nir_shader *nir,
                       enum amd_gfx_level gfx_level,
                       uint32_t clip_cull_mask,
                       const uint8_t *param_offsets,
                       bool has_param_exports,
                       bool export_primitive_id,
                       bool disable_streamout,
                       bool kill_pointsize,
                       bool kill_layer,
                       bool force_vrs)
{
   nir_function_impl *impl = nir_shader_get_entrypoint(nir);
   nir_metadata preserved = nir_metadata_control_flow;

   nir_builder b = nir_builder_at(nir_after_impl(impl));

   ac_nir_prerast_out out = {0};
   gather_outputs(&b, impl, &out);
   b.cursor = nir_after_impl(impl);

   if (export_primitive_id) {
      /* When the primitive ID is read by FS, we must ensure that it's exported by the previous
       * vertex stage because it's implicit for VS or TES (but required by the Vulkan spec for GS
       * or MS).
       */
      out.outputs[VARYING_SLOT_PRIMITIVE_ID][0] = nir_load_primitive_id(&b);
      out.infos[VARYING_SLOT_PRIMITIVE_ID].as_varying_mask = 0x1;

      /* Update outputs_written to reflect that the pass added a new output. */
      nir->info.outputs_written |= BITFIELD64_BIT(VARYING_SLOT_PRIMITIVE_ID);
   }

   if (!disable_streamout && nir->xfb_info) {
      emit_streamout(&b, 0, nir->xfb_info, &out);
      preserved = nir_metadata_none;
   }

   uint64_t export_outputs = nir->info.outputs_written | VARYING_BIT_POS;
   if (kill_pointsize)
      export_outputs &= ~VARYING_BIT_PSIZ;
   if (kill_layer)
      export_outputs &= ~VARYING_BIT_LAYER;

   ac_nir_export_position(&b, gfx_level, clip_cull_mask, !has_param_exports,
                          force_vrs, true, export_outputs, &out, NULL);

   if (has_param_exports) {
      ac_nir_export_parameters(&b, param_offsets,
                               nir->info.outputs_written,
                               nir->info.outputs_written_16bit,
                               &out);
   }

   nir_metadata_preserve(impl, preserved);
}

static nir_def *
ac_nir_accum_ior(nir_builder *b, nir_def *accum_result, nir_def *new_term)
{
   return accum_result ? nir_ior(b, accum_result, new_term) : new_term;
}

bool
ac_nir_gs_shader_query(nir_builder *b,
                       bool has_gen_prim_query,
                       bool has_gs_invocations_query,
                       bool has_gs_primitives_query,
                       unsigned num_vertices_per_primitive,
                       unsigned wave_size,
                       nir_def *vertex_count[4],
                       nir_def *primitive_count[4])
{
   nir_def *pipeline_query_enabled = NULL;
   nir_def *prim_gen_query_enabled = NULL;
   nir_def *any_query_enabled = NULL;

   if (has_gen_prim_query) {
      prim_gen_query_enabled = nir_load_prim_gen_query_enabled_amd(b);
      any_query_enabled = ac_nir_accum_ior(b, any_query_enabled, prim_gen_query_enabled);
   }

   if (has_gs_invocations_query || has_gs_primitives_query) {
      pipeline_query_enabled = nir_load_pipeline_stat_query_enabled_amd(b);
      any_query_enabled = ac_nir_accum_ior(b, any_query_enabled, pipeline_query_enabled);
   }

   if (!any_query_enabled) {
      /* has no query */
      return false;
   }

   nir_if *if_shader_query = nir_push_if(b, any_query_enabled);

   nir_def *active_threads_mask = nir_ballot(b, 1, wave_size, nir_imm_true(b));
   nir_def *num_active_threads = nir_bit_count(b, active_threads_mask);

   /* Calculate the "real" number of emitted primitives from the emitted GS vertices and primitives.
    * GS emits points, line strips or triangle strips.
    * Real primitives are points, lines or triangles.
    */
   nir_def *num_prims_in_wave[4] = {0};
   u_foreach_bit (i, b->shader->info.gs.active_stream_mask) {
      assert(vertex_count[i] && primitive_count[i]);

      nir_scalar vtx_cnt = nir_get_scalar(vertex_count[i], 0);
      nir_scalar prm_cnt = nir_get_scalar(primitive_count[i], 0);

      if (nir_scalar_is_const(vtx_cnt) && nir_scalar_is_const(prm_cnt)) {
         unsigned gs_vtx_cnt = nir_scalar_as_uint(vtx_cnt);
         unsigned gs_prm_cnt = nir_scalar_as_uint(prm_cnt);
         unsigned total_prm_cnt = gs_vtx_cnt - gs_prm_cnt * (num_vertices_per_primitive - 1u);
         if (total_prm_cnt == 0)
            continue;

         num_prims_in_wave[i] = nir_imul_imm(b, num_active_threads, total_prm_cnt);
      } else {
         nir_def *gs_vtx_cnt = vtx_cnt.def;
         nir_def *gs_prm_cnt = prm_cnt.def;
         if (num_vertices_per_primitive > 1)
            gs_prm_cnt = nir_iadd(b, nir_imul_imm(b, gs_prm_cnt, -1u * (num_vertices_per_primitive - 1)), gs_vtx_cnt);
         num_prims_in_wave[i] = nir_reduce(b, gs_prm_cnt, .reduction_op = nir_op_iadd);
      }
   }

   /* Store the query result to query result using an atomic add. */
   nir_if *if_first_lane = nir_push_if(b, nir_elect(b, 1));
   {
      if (has_gs_invocations_query || has_gs_primitives_query) {
         nir_if *if_pipeline_query = nir_push_if(b, pipeline_query_enabled);
         {
            nir_def *count = NULL;

            /* Add all streams' number to the same counter. */
            for (int i = 0; i < 4; i++) {
               if (num_prims_in_wave[i]) {
                  if (count)
                     count = nir_iadd(b, count, num_prims_in_wave[i]);
                  else
                     count = num_prims_in_wave[i];
               }
            }

            if (has_gs_primitives_query && count)
               nir_atomic_add_gs_emit_prim_count_amd(b, count);

            if (has_gs_invocations_query)
               nir_atomic_add_shader_invocation_count_amd(b, num_active_threads);
         }
         nir_pop_if(b, if_pipeline_query);
      }

      if (has_gen_prim_query) {
         nir_if *if_prim_gen_query = nir_push_if(b, prim_gen_query_enabled);
         {
            /* Add to the counter for this stream. */
            for (int i = 0; i < 4; i++) {
               if (num_prims_in_wave[i])
                  nir_atomic_add_gen_prim_count_amd(b, num_prims_in_wave[i], .stream_id = i);
            }
         }
         nir_pop_if(b, if_prim_gen_query);
      }
   }
   nir_pop_if(b, if_first_lane);

   nir_pop_if(b, if_shader_query);
   return true;
}

typedef struct {
   nir_def *outputs[64][4];
   nir_def *outputs_16bit_lo[16][4];
   nir_def *outputs_16bit_hi[16][4];

   ac_nir_gs_output_info *info;

   nir_def *vertex_count[4];
   nir_def *primitive_count[4];
} lower_legacy_gs_state;

static bool
lower_legacy_gs_store_output(nir_builder *b, nir_intrinsic_instr *intrin,
                             lower_legacy_gs_state *s)
{
   /* Assume:
    * - the shader used nir_lower_io_to_temporaries
    * - 64-bit outputs are lowered
    * - no indirect indexing is present
    */
   assert(nir_src_is_const(intrin->src[1]) && !nir_src_as_uint(intrin->src[1]));

   b->cursor = nir_before_instr(&intrin->instr);

   unsigned component = nir_intrinsic_component(intrin);
   unsigned write_mask = nir_intrinsic_write_mask(intrin);
   nir_io_semantics sem = nir_intrinsic_io_semantics(intrin);

   nir_def **outputs;
   if (sem.location < VARYING_SLOT_VAR0_16BIT) {
      outputs = s->outputs[sem.location];
   } else {
      unsigned index = sem.location - VARYING_SLOT_VAR0_16BIT;
      if (sem.high_16bits)
         outputs = s->outputs_16bit_hi[index];
      else
         outputs = s->outputs_16bit_lo[index];
   }

   nir_def *store_val = intrin->src[0].ssa;
   /* 64bit output has been lowered to 32bit */
   assert(store_val->bit_size <= 32);

   /* 16-bit output stored in a normal varying slot that isn't a dedicated 16-bit slot. */
   const bool non_dedicated_16bit = sem.location < VARYING_SLOT_VAR0_16BIT && store_val->bit_size == 16;

   u_foreach_bit (i, write_mask) {
      unsigned comp = component + i;
      nir_def *store_component = nir_channel(b, store_val, i);

      if (non_dedicated_16bit) {
         if (sem.high_16bits) {
            nir_def *lo = outputs[comp] ? nir_unpack_32_2x16_split_x(b, outputs[comp]) : nir_imm_intN_t(b, 0, 16);
            outputs[comp] = nir_pack_32_2x16_split(b, lo, store_component);
         } else {
            nir_def *hi = outputs[comp] ? nir_unpack_32_2x16_split_y(b, outputs[comp]) : nir_imm_intN_t(b, 0, 16);
            outputs[comp] = nir_pack_32_2x16_split(b, store_component, hi);
         }
      } else {
         outputs[comp] = store_component;
      }
   }

   nir_instr_remove(&intrin->instr);
   return true;
}

static bool
lower_legacy_gs_emit_vertex_with_counter(nir_builder *b, nir_intrinsic_instr *intrin,
                                         lower_legacy_gs_state *s)
{
   b->cursor = nir_before_instr(&intrin->instr);

   unsigned stream = nir_intrinsic_stream_id(intrin);
   nir_def *vtxidx = intrin->src[0].ssa;

   nir_def *gsvs_ring = nir_load_ring_gsvs_amd(b, .stream_id = stream);
   nir_def *soffset = nir_load_ring_gs2vs_offset_amd(b);

   unsigned offset = 0;
   u_foreach_bit64 (i, b->shader->info.outputs_written) {
      for (unsigned j = 0; j < 4; j++) {
         nir_def *output = s->outputs[i][j];
         /* Next vertex emit need a new value, reset all outputs. */
         s->outputs[i][j] = NULL;

         const uint8_t usage_mask = s->info->varying_mask[i] | s->info->sysval_mask[i];

         if (!(usage_mask & (1 << j)) ||
             ((s->info->streams[i] >> (j * 2)) & 0x3) != stream)
            continue;

         unsigned base = offset * b->shader->info.gs.vertices_out * 4;
         offset++;

         /* no one set this output, skip the buffer store */
         if (!output)
            continue;

         nir_def *voffset = nir_ishl_imm(b, vtxidx, 2);

         /* extend 8/16 bit to 32 bit, 64 bit has been lowered */
         nir_def *data = nir_u2uN(b, output, 32);

         nir_store_buffer_amd(b, data, gsvs_ring, voffset, soffset, nir_imm_int(b, 0),
                              .access = ACCESS_COHERENT | ACCESS_NON_TEMPORAL |
                                        ACCESS_IS_SWIZZLED_AMD,
                              .base = base,
                              /* For ACO to not reorder this store around EmitVertex/EndPrimitve */
                              .memory_modes = nir_var_shader_out);
      }
   }

   u_foreach_bit (i, b->shader->info.outputs_written_16bit) {
      for (unsigned j = 0; j < 4; j++) {
         nir_def *output_lo = s->outputs_16bit_lo[i][j];
         nir_def *output_hi = s->outputs_16bit_hi[i][j];
         /* Next vertex emit need a new value, reset all outputs. */
         s->outputs_16bit_lo[i][j] = NULL;
         s->outputs_16bit_hi[i][j] = NULL;

         bool has_lo_16bit = (s->info->varying_mask_16bit_lo[i] & (1 << j)) &&
            ((s->info->streams_16bit_lo[i] >> (j * 2)) & 0x3) == stream;
         bool has_hi_16bit = (s->info->varying_mask_16bit_hi[i] & (1 << j)) &&
            ((s->info->streams_16bit_hi[i] >> (j * 2)) & 0x3) == stream;
         if (!has_lo_16bit && !has_hi_16bit)
            continue;

         unsigned base = offset * b->shader->info.gs.vertices_out;
         offset++;

         bool has_lo_16bit_out = has_lo_16bit && output_lo;
         bool has_hi_16bit_out = has_hi_16bit && output_hi;

         /* no one set needed output, skip the buffer store */
         if (!has_lo_16bit_out && !has_hi_16bit_out)
            continue;

         if (!has_lo_16bit_out)
            output_lo = nir_undef(b, 1, 16);

         if (!has_hi_16bit_out)
            output_hi = nir_undef(b, 1, 16);

         nir_def *voffset = nir_iadd_imm(b, vtxidx, base);
         voffset = nir_ishl_imm(b, voffset, 2);

         nir_store_buffer_amd(b, nir_pack_32_2x16_split(b, output_lo, output_hi),
                              gsvs_ring, voffset, soffset, nir_imm_int(b, 0),
                              .access = ACCESS_COHERENT | ACCESS_NON_TEMPORAL |
                                        ACCESS_IS_SWIZZLED_AMD,
                              /* For ACO to not reorder this store around EmitVertex/EndPrimitve */
                              .memory_modes = nir_var_shader_out);
      }
   }

   /* Signal vertex emission. */
   nir_sendmsg_amd(b, nir_load_gs_wave_id_amd(b),
                   .base = AC_SENDMSG_GS_OP_EMIT | AC_SENDMSG_GS | (stream << 8));

   nir_instr_remove(&intrin->instr);
   return true;
}

static bool
lower_legacy_gs_set_vertex_and_primitive_count(nir_builder *b, nir_intrinsic_instr *intrin,
                                               lower_legacy_gs_state *s)
{
   b->cursor = nir_before_instr(&intrin->instr);

   unsigned stream = nir_intrinsic_stream_id(intrin);

   s->vertex_count[stream] = intrin->src[0].ssa;
   s->primitive_count[stream] = intrin->src[1].ssa;

   nir_instr_remove(&intrin->instr);
   return true;
}

static bool
lower_legacy_gs_end_primitive_with_counter(nir_builder *b, nir_intrinsic_instr *intrin,
                                               lower_legacy_gs_state *s)
{
   b->cursor = nir_before_instr(&intrin->instr);
   const unsigned stream = nir_intrinsic_stream_id(intrin);

   /* Signal primitive emission. */
   nir_sendmsg_amd(b, nir_load_gs_wave_id_amd(b),
                   .base = AC_SENDMSG_GS_OP_CUT | AC_SENDMSG_GS | (stream << 8));

   nir_instr_remove(&intrin->instr);
   return true;
}

static bool
lower_legacy_gs_intrinsic(nir_builder *b, nir_instr *instr, void *state)
{
   lower_legacy_gs_state *s = (lower_legacy_gs_state *) state;

   if (instr->type != nir_instr_type_intrinsic)
      return false;

   nir_intrinsic_instr *intrin = nir_instr_as_intrinsic(instr);

   if (intrin->intrinsic == nir_intrinsic_store_output)
      return lower_legacy_gs_store_output(b, intrin, s);
   else if (intrin->intrinsic == nir_intrinsic_emit_vertex_with_counter)
      return lower_legacy_gs_emit_vertex_with_counter(b, intrin, s);
   else if (intrin->intrinsic == nir_intrinsic_end_primitive_with_counter)
      return lower_legacy_gs_end_primitive_with_counter(b, intrin, s);
   else if (intrin->intrinsic == nir_intrinsic_set_vertex_and_primitive_count)
      return lower_legacy_gs_set_vertex_and_primitive_count(b, intrin, s);

   return false;
}

void
ac_nir_lower_legacy_gs(nir_shader *nir,
                       bool has_gen_prim_query,
                       bool has_pipeline_stats_query,
                       ac_nir_gs_output_info *output_info)
{
   lower_legacy_gs_state s = {
      .info = output_info,
   };

   unsigned num_vertices_per_primitive = 0;
   switch (nir->info.gs.output_primitive) {
   case MESA_PRIM_POINTS:
      num_vertices_per_primitive = 1;
      break;
   case MESA_PRIM_LINE_STRIP:
      num_vertices_per_primitive = 2;
      break;
   case MESA_PRIM_TRIANGLE_STRIP:
      num_vertices_per_primitive = 3;
      break;
   default:
      unreachable("Invalid GS output primitive.");
      break;
   }

   nir_shader_instructions_pass(nir, lower_legacy_gs_intrinsic,
                                nir_metadata_control_flow, &s);

   nir_function_impl *impl = nir_shader_get_entrypoint(nir);

   nir_builder builder = nir_builder_at(nir_after_impl(impl));
   nir_builder *b = &builder;

   /* Emit shader query for mix use legacy/NGG GS */
   bool progress = ac_nir_gs_shader_query(b,
                                          has_gen_prim_query,
                                          has_pipeline_stats_query,
                                          has_pipeline_stats_query,
                                          num_vertices_per_primitive,
                                          64,
                                          s.vertex_count,
                                          s.primitive_count);

   /* Wait for all stores to finish. */
   nir_barrier(b, .execution_scope = SCOPE_INVOCATION,
                      .memory_scope = SCOPE_DEVICE,
                      .memory_semantics = NIR_MEMORY_RELEASE,
                      .memory_modes = nir_var_shader_out | nir_var_mem_ssbo |
                                      nir_var_mem_global | nir_var_image);

   /* Signal that the GS is done. */
   nir_sendmsg_amd(b, nir_load_gs_wave_id_amd(b),
                   .base = AC_SENDMSG_GS_OP_NOP | AC_SENDMSG_GS_DONE);

   if (progress)
      nir_metadata_preserve(impl, nir_metadata_none);
}

/* Shader logging function for printing nir_def values. The driver prints this after
 * command submission.
 *
 * Ring buffer layout: {uint32_t num_dwords; vec4; vec4; vec4; ... }
 * - The buffer size must be 2^N * 16 + 4
 * - num_dwords is incremented atomically and the ring wraps around, removing
 *   the oldest entries.
 */
void
ac_nir_store_debug_log_amd(nir_builder *b, nir_def *uvec4)
{
   nir_def *buf = nir_load_debug_log_desc_amd(b);
   nir_def *zero = nir_imm_int(b, 0);

   nir_def *max_index =
      nir_iadd_imm(b, nir_ushr_imm(b, nir_iadd_imm(b, nir_channel(b, buf, 2), -4), 4), -1);
   nir_def *index = nir_ssbo_atomic(b, 32, buf, zero, nir_imm_int(b, 1),
                                    .atomic_op = nir_atomic_op_iadd);
   index = nir_iand(b, index, max_index);
   nir_def *offset = nir_iadd_imm(b, nir_imul_imm(b, index, 16), 4);
   nir_store_buffer_amd(b, uvec4, buf, offset, zero, zero);
}

static bool
needs_rounding_mode_16_64(nir_instr *instr)
{
   if (instr->type != nir_instr_type_alu)
      return false;
   nir_alu_instr *alu = nir_instr_as_alu(instr);
   if (alu->op == nir_op_fquantize2f16)
      return true;
   if (alu->def.bit_size != 16 && alu->def.bit_size != 64)
      return false;
   if (nir_alu_type_get_base_type(nir_op_infos[alu->op].output_type) != nir_type_float)
      return false;

   switch (alu->op) {
   case nir_op_f2f64:
   case nir_op_b2f64:
   case nir_op_f2f16_rtz:
   case nir_op_b2f16:
   case nir_op_fsat:
   case nir_op_fabs:
   case nir_op_fneg:
   case nir_op_fsign:
   case nir_op_ftrunc:
   case nir_op_fceil:
   case nir_op_ffloor:
   case nir_op_ffract:
   case nir_op_fround_even:
   case nir_op_fmin:
   case nir_op_fmax:
      return false;
   default:
      return true;
   }
}

static bool
can_use_fmamix(nir_scalar s, enum amd_gfx_level gfx_level)
{
   s = nir_scalar_chase_movs(s);
   if (!list_is_singular(&s.def->uses))
      return false;

   if (nir_scalar_is_intrinsic(s) &&
       nir_scalar_intrinsic_op(s) == nir_intrinsic_load_interpolated_input)
      return gfx_level >= GFX11;

   if (!nir_scalar_is_alu(s))
      return false;

   switch (nir_scalar_alu_op(s)) {
   case nir_op_fmul:
   case nir_op_ffma:
   case nir_op_fadd:
   case nir_op_fsub:
      return true;
   case nir_op_fsat:
      return can_use_fmamix(nir_scalar_chase_alu_src(s, 0), gfx_level);
   default:
      return false;
   }
}

static bool
split_pack_half(nir_builder *b, nir_instr *instr, void *param)
{
   enum amd_gfx_level gfx_level = *(enum amd_gfx_level *)param;

   if (instr->type != nir_instr_type_alu)
      return false;
   nir_alu_instr *alu = nir_instr_as_alu(instr);
   if (alu->op != nir_op_pack_half_2x16_rtz_split && alu->op != nir_op_pack_half_2x16_split)
      return false;

   nir_scalar s = nir_get_scalar(&alu->def, 0);

   if (!can_use_fmamix(nir_scalar_chase_alu_src(s, 0), gfx_level) ||
       !can_use_fmamix(nir_scalar_chase_alu_src(s, 1), gfx_level))
      return false;

   b->cursor = nir_before_instr(instr);

   /* Split pack_half into two f2f16 to create v_fma_mix{lo,hi}_f16
    * in the backend.
    */
   nir_def *lo = nir_f2f16(b, nir_ssa_for_alu_src(b, alu, 0));
   nir_def *hi = nir_f2f16(b, nir_ssa_for_alu_src(b, alu, 1));
   nir_def_replace(&alu->def, nir_pack_32_2x16_split(b, lo, hi));
   return true;
}

bool
ac_nir_opt_pack_half(nir_shader *shader, enum amd_gfx_level gfx_level)
{
   if (gfx_level < GFX10)
      return false;

   unsigned exec_mode = shader->info.float_controls_execution_mode;
   bool set_mode = false;
   if (!nir_is_rounding_mode_rtz(exec_mode, 16)) {
      nir_foreach_function_impl(impl, shader) {
         nir_foreach_block(block, impl) {
            nir_foreach_instr(instr, block) {
               if (needs_rounding_mode_16_64(instr))
                  return false;
            }
         }
      }
      set_mode = true;
   }

   bool progress = nir_shader_instructions_pass(shader, split_pack_half,
                                                nir_metadata_control_flow,
                                                &gfx_level);

   if (set_mode && progress) {
      exec_mode &= ~(FLOAT_CONTROLS_ROUNDING_MODE_RTE_FP16 | FLOAT_CONTROLS_ROUNDING_MODE_RTE_FP64);
      exec_mode |= FLOAT_CONTROLS_ROUNDING_MODE_RTZ_FP16 | FLOAT_CONTROLS_ROUNDING_MODE_RTZ_FP64;
      shader->info.float_controls_execution_mode = exec_mode;
   }
   return progress;
}

nir_def *
ac_average_samples(nir_builder *b, nir_def **samples, unsigned num_samples)
{
   /* This works like add-reduce by computing the sum of each pair independently, and then
    * computing the sum of each pair of sums, and so on, to get better instruction-level
    * parallelism.
    */
   if (num_samples == 16) {
      for (unsigned i = 0; i < 8; i++)
         samples[i] = nir_fadd(b, samples[i * 2], samples[i * 2 + 1]);
   }
   if (num_samples >= 8) {
      for (unsigned i = 0; i < 4; i++)
         samples[i] = nir_fadd(b, samples[i * 2], samples[i * 2 + 1]);
   }
   if (num_samples >= 4) {
      for (unsigned i = 0; i < 2; i++)
         samples[i] = nir_fadd(b, samples[i * 2], samples[i * 2 + 1]);
   }
   if (num_samples >= 2)
      samples[0] = nir_fadd(b, samples[0], samples[1]);

   return nir_fmul_imm(b, samples[0], 1.0 / num_samples); /* average the sum */
}

void
ac_optimization_barrier_vgpr_array(const struct radeon_info *info, nir_builder *b,
                                   nir_def **array, unsigned num_elements,
                                   unsigned num_components)
{
   /* We use the optimization barrier to force LLVM to form VMEM clauses by constraining its
    * instruction scheduling options.
    *
    * VMEM clauses are supported since GFX10. It's not recommended to use the optimization
    * barrier in the compute blit for GFX6-8 because the lack of A16 combined with optimization
    * barriers would unnecessarily increase VGPR usage for MSAA resources.
    */
   if (!b->shader->info.use_aco_amd && info->gfx_level >= GFX10) {
      for (unsigned i = 0; i < num_elements; i++) {
         unsigned prev_num = array[i]->num_components;
         array[i] = nir_trim_vector(b, array[i], num_components);
         array[i] = nir_optimization_barrier_vgpr_amd(b, array[i]->bit_size, array[i]);
         array[i] = nir_pad_vector(b, array[i], prev_num);
      }
   }
}

nir_def *
ac_get_global_ids(nir_builder *b, unsigned num_components, unsigned bit_size)
{
   unsigned mask = BITFIELD_MASK(num_components);

   nir_def *local_ids = nir_channels(b, nir_load_local_invocation_id(b), mask);
   nir_def *block_ids = nir_channels(b, nir_load_workgroup_id(b), mask);
   nir_def *block_size = nir_channels(b, nir_load_workgroup_size(b), mask);

   assert(bit_size == 32 || bit_size == 16);
   if (bit_size == 16) {
      local_ids = nir_i2iN(b, local_ids, bit_size);
      block_ids = nir_i2iN(b, block_ids, bit_size);
      block_size = nir_i2iN(b, block_size, bit_size);
   }

   return nir_iadd(b, nir_imul(b, block_ids, block_size), local_ids);
}

unsigned
ac_nir_varying_expression_max_cost(nir_shader *producer, nir_shader *consumer)
{
   switch (consumer->info.stage) {
   case MESA_SHADER_TESS_CTRL:
      /* VS->TCS
       * Non-amplifying shaders can always have their varying expressions
       * moved into later shaders.
       */
      return UINT_MAX;

   case MESA_SHADER_GEOMETRY:
      /* VS->GS, TES->GS */
      return consumer->info.gs.vertices_in == 1 ? UINT_MAX :
             consumer->info.gs.vertices_in == 2 ? 20 : 14;

   case MESA_SHADER_TESS_EVAL:
      /* TCS->TES and VS->TES (OpenGL only) */
   case MESA_SHADER_FRAGMENT:
      /* Up to 3 uniforms and 5 ALUs. */
      return 12;

   default:
      unreachable("unexpected shader stage");
   }
}

typedef struct {
   enum amd_gfx_level gfx_level;
   bool use_llvm;
   bool after_lowering;
} mem_access_cb_data;

static bool
use_smem_for_load(nir_builder *b, nir_intrinsic_instr *intrin, void *cb_data_)
{
   const mem_access_cb_data *cb_data = (mem_access_cb_data *)cb_data_;

   switch (intrin->intrinsic) {
   case nir_intrinsic_load_ssbo:
   case nir_intrinsic_load_global:
   case nir_intrinsic_load_global_constant:
   case nir_intrinsic_load_global_amd:
   case nir_intrinsic_load_constant:
      if (cb_data->use_llvm)
         return false;
      break;
   case nir_intrinsic_load_ubo:
      break;
   default:
      return false;
   }

   if (intrin->def.divergent || (cb_data->after_lowering && intrin->def.bit_size < 32))
      return false;

   enum gl_access_qualifier access = nir_intrinsic_access(intrin);
   bool glc = access & (ACCESS_VOLATILE | ACCESS_COHERENT);
   bool reorder = nir_intrinsic_can_reorder(intrin) || ((access & ACCESS_NON_WRITEABLE) && !(access & ACCESS_VOLATILE));
   if (!reorder || (glc && cb_data->gfx_level < GFX8))
      return false;

   nir_intrinsic_set_access(intrin, access | ACCESS_SMEM_AMD);
   return true;
}

static nir_mem_access_size_align
lower_mem_access_cb(nir_intrinsic_op intrin, uint8_t bytes, uint8_t bit_size, uint32_t align_mul, uint32_t align_offset,
                    bool offset_is_const, enum gl_access_qualifier access, const void *cb_data_)
{
   const mem_access_cb_data *cb_data = (mem_access_cb_data *)cb_data_;
   const bool is_load = nir_intrinsic_infos[intrin].has_dest;
   const bool is_smem = intrin == nir_intrinsic_load_push_constant || (access & ACCESS_SMEM_AMD);
   const uint32_t combined_align = nir_combined_align(align_mul, align_offset);

   /* Make 8-bit accesses 16-bit if possible */
   if (is_load && bit_size == 8 && combined_align >= 2 && bytes % 2 == 0)
      bit_size = 16;

   unsigned max_components = 4;
   if (cb_data->use_llvm && access & (ACCESS_COHERENT | ACCESS_VOLATILE) &&
       (intrin == nir_intrinsic_load_global || intrin == nir_intrinsic_store_global))
      max_components = 1;
   else if (is_smem)
      max_components = MIN2(512 / bit_size, 16);

   nir_mem_access_size_align res;
   res.num_components = MIN2(bytes / (bit_size / 8), max_components);
   res.bit_size = bit_size;
   res.align = MIN2(bit_size / 8, 4); /* 64-bit access only requires 4 byte alignment. */
   res.shift = nir_mem_access_shift_method_shift64;

   if (!is_load)
      return res;

   /* Lower 8/16-bit loads to 32-bit, unless it's a VMEM scalar load. */

   const bool support_subdword = res.num_components == 1 && !is_smem &&
                                 (!cb_data->use_llvm || intrin != nir_intrinsic_load_ubo);

   if (res.bit_size >= 32 || support_subdword)
      return res;

   const uint32_t max_pad = 4 - MIN2(combined_align, 4);

   /* Global loads don't have bounds checking, so increasing the size might not be safe. */
   if (intrin == nir_intrinsic_load_global || intrin == nir_intrinsic_load_global_constant) {
      if (align_mul < 4) {
         /* If we split the load, only lower it to 32-bit if this is a SMEM load. */
         const unsigned chunk_bytes = align(bytes, 4) - max_pad;
         if (!is_smem && chunk_bytes < bytes)
            return res;
      }

      res.num_components = DIV_ROUND_UP(bytes, 4);
   } else {
      res.num_components = DIV_ROUND_UP(bytes + max_pad, 4);
   }
   res.num_components = MIN2(res.num_components, max_components);
   res.bit_size = 32;
   res.align = 4;
   res.shift = is_smem ? res.shift : nir_mem_access_shift_method_bytealign_amd;

   return res;
}

bool
ac_nir_flag_smem_for_loads(nir_shader *shader, enum amd_gfx_level gfx_level, bool use_llvm, bool after_lowering)
{
   mem_access_cb_data cb_data = {
      .gfx_level = gfx_level,
      .use_llvm = use_llvm,
      .after_lowering = after_lowering,
   };
   return nir_shader_intrinsics_pass(shader, &use_smem_for_load, nir_metadata_all, &cb_data);
}

bool
ac_nir_lower_mem_access_bit_sizes(nir_shader *shader, enum amd_gfx_level gfx_level, bool use_llvm)
{
   mem_access_cb_data cb_data = {
      .gfx_level = gfx_level,
      .use_llvm = use_llvm,
   };
   nir_lower_mem_access_bit_sizes_options lower_mem_access_options = {
      .callback = &lower_mem_access_cb,
      .modes = nir_var_mem_ubo | nir_var_mem_push_const | nir_var_mem_ssbo |
               nir_var_mem_global | nir_var_mem_constant | nir_var_mem_shared |
               nir_var_shader_temp,
      .may_lower_unaligned_stores_to_atomics = false,
      .cb_data = &cb_data,
   };
   return nir_lower_mem_access_bit_sizes(shader, &lower_mem_access_options);
}
