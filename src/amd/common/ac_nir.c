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


/* Set NIR options shared by ACO, LLVM, RADV, and radeonsi. */
void ac_nir_set_options(struct radeon_info *info, bool use_llvm,
                        nir_shader_compiler_options *options)
{
   /*        |---------------------------------- Performance & Availability --------------------------------|
    *        |MAD/MAC/MADAK/MADMK|MAD_LEGACY|MAC_LEGACY|    FMA     |FMAC/FMAAK/FMAMK|FMA_LEGACY|PK_FMA_F16,|Best choice
    * Arch   |    F32,F16,F64    | F32,F16  | F32,F16  |F32,F16,F64 |    F32,F16     |   F32    |PK_FMAC_F16|F16,F32,F64
    * ------------------------------------------------------------------------------------------------------------------
    * gfx6,7 |     1 , - , -     |  1 , -   |  1 , -   |1/4, - ,1/16|     - , -      |    -     |   - , -   | - ,MAD,FMA
    * gfx8   |     1 , 1 , -     |  1 , -   |  - , -   |1/4, 1 ,1/16|     - , -      |    -     |   - , -   |MAD,MAD,FMA
    * gfx9   |     1 ,1|0, -     |  1 , -   |  - , -   | 1 , 1 ,1/16|    0|1, -      |    -     |   2 , -   |FMA,MAD,FMA
    * gfx10  |     1 , - , -     |  1 , -   |  1 , -   | 1 , 1 ,1/16|     1 , 1      |    -     |   2 , 2   |FMA,MAD,FMA
    * gfx10.3|     - , - , -     |  - , -   |  - , -   | 1 , 1 ,1/16|     1 , 1      |    1     |   2 , 2   |  all FMA
    * gfx11  |     - , - , -     |  - , -   |  - , -   | 2 , 2 ,1/16|     2 , 2      |    2     |   2 , 2   |  all FMA
    *
    * Tahiti, Hawaii, Carrizo, Vega20: FMA_F32 is full rate, FMA_F64 is 1/4
    * gfx9 supports MAD_F16 only on Vega10, Raven, Raven2, Renoir.
    * gfx9 supports FMAC_F32 only on Vega20, but doesn't support FMAAK and FMAMK.
    *
    * gfx8 prefers MAD for F16 because of MAC/MADAK/MADMK.
    * gfx9 and newer prefer FMA for F16 because of the packed instruction.
    * gfx10 and older prefer MAD for F32 because of the legacy instruction.
    */

   memset(options, 0, sizeof(*options));
   options->vertex_id_zero_based = true;
   options->lower_scmp = true;
   options->lower_flrp16 = true;
   options->lower_flrp32 = true;
   options->lower_flrp64 = true;
   options->lower_device_index_to_zero = true;
   options->lower_fdiv = true;
   options->lower_fmod = true;
   options->lower_ineg = true;
   options->lower_bitfield_insert = true;
   options->lower_bitfield_extract = true;
   options->lower_pack_snorm_4x8 = true;
   options->lower_pack_unorm_4x8 = true;
   options->lower_pack_half_2x16 = true;
   options->lower_pack_64_2x32 = true;
   options->lower_pack_64_4x16 = true;
   options->lower_pack_32_2x16 = true;
   options->lower_unpack_snorm_2x16 = true;
   options->lower_unpack_snorm_4x8 = true;
   options->lower_unpack_unorm_2x16 = true;
   options->lower_unpack_unorm_4x8 = true;
   options->lower_unpack_half_2x16 = true;
   options->lower_fpow = true;
   options->lower_mul_2x32_64 = true;
   options->lower_iadd_sat = info->gfx_level <= GFX8;
   options->lower_hadd = true;
   options->lower_mul_32x16 = true;
   options->has_bfe = true;
   options->has_bfm = true;
   options->has_bitfield_select = true;
   options->has_fneo_fcmpu = true;
   options->has_ford_funord = true;
   options->has_fsub = true;
   options->has_isub = true;
   options->has_sdot_4x8 = info->has_accelerated_dot_product;
   options->has_sudot_4x8 = info->has_accelerated_dot_product && info->gfx_level >= GFX11;
   options->has_udot_4x8 = info->has_accelerated_dot_product;
   options->has_sdot_4x8_sat = info->has_accelerated_dot_product;
   options->has_sudot_4x8_sat = info->has_accelerated_dot_product && info->gfx_level >= GFX11;
   options->has_udot_4x8_sat = info->has_accelerated_dot_product;
   options->has_dot_2x16 = info->has_accelerated_dot_product && info->gfx_level < GFX11;
   options->has_find_msb_rev = true;
   options->has_pack_32_4x8 = true;
   options->has_pack_half_2x16_rtz = true;
   options->has_bit_test = !use_llvm;
   options->has_fmulz = true;
   options->has_msad = true;
   options->has_shfr32 = true;
   options->lower_int64_options = nir_lower_imul64 | nir_lower_imul_high64 | nir_lower_imul_2x32_64 | nir_lower_divmod64 |
                                  nir_lower_minmax64 | nir_lower_iabs64 | nir_lower_iadd_sat64 | nir_lower_conv64;
   options->divergence_analysis_options = nir_divergence_view_index_uniform;
   options->optimize_quad_vote_to_reduce = !use_llvm;
   options->lower_fisnormal = true;
   options->support_16bit_alu = info->gfx_level >= GFX8;
   options->vectorize_vec2_16bit = info->has_packed_math_16bit;
   options->discard_is_demote = true;
   options->optimize_sample_mask_in = true;
   options->optimize_load_front_face_fsign = true;
   options->io_options = nir_io_has_flexible_input_interpolation_except_flat |
                         (info->gfx_level >= GFX8 ? nir_io_16bit_input_output_support : 0) |
                         nir_io_prefer_scalar_fs_inputs |
                         nir_io_mix_convergent_flat_with_interpolated |
                         nir_io_vectorizer_ignores_types |
                         nir_io_compaction_rotates_color_channels;
   options->lower_layer_fs_input_to_sysval = true;
   options->scalarize_ddx = true;
   options->skip_lower_packing_ops =
      BITFIELD_BIT(nir_lower_packing_op_unpack_64_2x32) |
      BITFIELD_BIT(nir_lower_packing_op_unpack_64_4x16) |
      BITFIELD_BIT(nir_lower_packing_op_unpack_32_2x16) |
      BITFIELD_BIT(nir_lower_packing_op_pack_32_4x8) |
      BITFIELD_BIT(nir_lower_packing_op_unpack_32_4x8);
}

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

nir_def *
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

static int
sort_xfb(const void *_a, const void *_b)
{
   const nir_xfb_output_info *a = (const nir_xfb_output_info *)_a;
   const nir_xfb_output_info *b = (const nir_xfb_output_info *)_b;

   if (a->buffer != b->buffer)
      return a->buffer > b->buffer ? 1 : -1;

   assert(a->offset != b->offset);
   return a->offset > b->offset ? 1 : -1;
}

/* Return XFB info sorted by buffer and offset, so that we can generate vec4
 * stores by iterating over outputs only once.
 */
nir_xfb_info *
ac_nir_get_sorted_xfb_info(const nir_shader *nir)
{
   if (!nir->xfb_info)
      return NULL;

   unsigned xfb_info_size = nir_xfb_info_size(nir->xfb_info->output_count);
   nir_xfb_info *info = rzalloc_size(nir, xfb_info_size);

   memcpy(info, nir->xfb_info, xfb_info_size);
   qsort(info->outputs, info->output_count, sizeof(info->outputs[0]), sort_xfb);
   return info;
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

void
ac_nir_emit_legacy_streamout(nir_builder *b, unsigned stream, nir_xfb_info *info, ac_nir_prerast_out *out)
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

   nir_def *zero = nir_imm_int(b, 0);
   unsigned num_values = 0, store_offset = 0, store_buffer_index = 0;
   nir_def *values[4];

   for (unsigned i = 0; i < info->output_count; i++) {
      const nir_xfb_output_info *output = info->outputs + i;
      if (stream != info->buffer_to_stream[output->buffer])
         continue;

      nir_alu_type *output_type;
      nir_def **output_data =
         get_output_and_type(out, output->location, output->high_16bits, &output_type);

      u_foreach_bit(out_comp, output->component_mask) {
         if (!output_data[out_comp])
            continue;

         nir_def *data = output_data[out_comp];

         if (data->bit_size < 32) {
            /* Convert the 16-bit output to 32 bits. */
            assert(output_type);

            nir_alu_type base_type = nir_alu_type_get_base_type(output_type[out_comp]);
            data = nir_convert_to_bit_size(b, data, base_type, 32);
         }

         assert(out_comp >= output->component_offset);
         const unsigned store_comp = out_comp - output->component_offset;
         const unsigned store_comp_offset = output->offset + store_comp * 4;
         const bool has_hole = store_offset + num_values * 4 != store_comp_offset;

         /* Flush the gathered components to memory as a vec4 store or less if there is a hole. */
         if (num_values && (num_values == 4 || store_buffer_index != output->buffer || has_hole)) {
            nir_store_buffer_amd(b, nir_vec(b, values, num_values), so_buffers[store_buffer_index],
                                 so_write_offset[store_buffer_index], zero, zero,
                                 .base = store_offset,
                                 .access = ACCESS_NON_TEMPORAL);
            num_values = 0;
         }

         /* Initialize the buffer index and offset if we are beginning a new vec4 store. */
         if (num_values == 0) {
            store_buffer_index = output->buffer;
            store_offset = store_comp_offset;
         }

         values[num_values++] = data;
      }
   }

   if (num_values) {
      /* Flush the remaining components to memory (as an up to vec4 store) */
      nir_store_buffer_amd(b, nir_vec(b, values, num_values), so_buffers[store_buffer_index],
                           so_write_offset[store_buffer_index], zero, zero,
                           .base = store_offset,
                           .access = ACCESS_NON_TEMPORAL);
   }

   nir_pop_if(b, NULL);
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

bool
ac_nir_optimize_uniform_atomics(nir_shader *nir)
{
   bool progress = false;
   NIR_PASS(progress, nir, ac_nir_opt_shared_append);

   nir_divergence_analysis(nir);
   NIR_PASS(progress, nir, nir_opt_uniform_atomics, false);

   return progress;
}

unsigned
ac_nir_lower_bit_size_callback(const nir_instr *instr, void *data)
{
   enum amd_gfx_level chip = *(enum amd_gfx_level *)data;

   if (instr->type != nir_instr_type_alu)
      return 0;
   nir_alu_instr *alu = nir_instr_as_alu(instr);

   /* If an instruction is not scalarized by this point,
    * it can be emitted as packed instruction */
   if (alu->def.num_components > 1)
      return 0;

   if (alu->def.bit_size & (8 | 16)) {
      unsigned bit_size = alu->def.bit_size;
      switch (alu->op) {
      case nir_op_bitfield_select:
      case nir_op_imul_high:
      case nir_op_umul_high:
      case nir_op_uadd_carry:
      case nir_op_usub_borrow:
         return 32;
      case nir_op_iabs:
      case nir_op_imax:
      case nir_op_umax:
      case nir_op_imin:
      case nir_op_umin:
      case nir_op_ishr:
      case nir_op_ushr:
      case nir_op_ishl:
      case nir_op_isign:
      case nir_op_uadd_sat:
      case nir_op_usub_sat:
         return (bit_size == 8 || !(chip >= GFX8 && alu->def.divergent)) ? 32 : 0;
      case nir_op_iadd_sat:
      case nir_op_isub_sat:
         return bit_size == 8 || !alu->def.divergent ? 32 : 0;

      default:
         return 0;
      }
   }

   if (nir_src_bit_size(alu->src[0].src) & (8 | 16)) {
      unsigned bit_size = nir_src_bit_size(alu->src[0].src);
      switch (alu->op) {
      case nir_op_bit_count:
      case nir_op_find_lsb:
      case nir_op_ufind_msb:
         return 32;
      case nir_op_ilt:
      case nir_op_ige:
      case nir_op_ieq:
      case nir_op_ine:
      case nir_op_ult:
      case nir_op_uge:
      case nir_op_bitz:
      case nir_op_bitnz:
         return (bit_size == 8 || !(chip >= GFX8 && alu->def.divergent)) ? 32 : 0;
      default:
         return 0;
      }
   }

   return 0;
}

static unsigned
align_load_store_size(enum amd_gfx_level gfx_level, unsigned size, bool uses_smem, bool is_shared)
{
   /* LDS can't overfetch because accesses that are partially out of range would be dropped
    * entirely, so all unaligned LDS accesses are always split.
    */
   if (is_shared)
      return size;

   /* Align the size to what the hw supports. Out of range access due to alignment is OK because
    * range checking is per dword for untyped instructions. This assumes that the compiler backend
    * overfetches due to load size alignment instead of splitting the load.
    *
    * GFX6-11 don't have 96-bit SMEM loads.
    * GFX6 doesn't have 96-bit untyped VMEM loads.
    */
   if (gfx_level >= (uses_smem ? GFX12 : GFX7) && size == 96)
      return size;
   else
      return util_next_power_of_two(size);
}

bool
ac_nir_mem_vectorize_callback(unsigned align_mul, unsigned align_offset, unsigned bit_size,
                              unsigned num_components, int64_t hole_size, nir_intrinsic_instr *low,
                              nir_intrinsic_instr *high, void *data)
{
   struct ac_nir_config *config = (struct ac_nir_config *)data;
   bool uses_smem = (nir_intrinsic_has_access(low) &&
                     nir_intrinsic_access(low) & ACCESS_SMEM_AMD) ||
                    /* These don't have the "access" field. */
                    low->intrinsic == nir_intrinsic_load_smem_amd ||
                    low->intrinsic == nir_intrinsic_load_push_constant;
   bool is_store = !nir_intrinsic_infos[low->intrinsic].has_dest;
   bool is_scratch = low->intrinsic == nir_intrinsic_load_stack ||
                     low->intrinsic == nir_intrinsic_store_stack ||
                     low->intrinsic == nir_intrinsic_load_scratch ||
                     low->intrinsic == nir_intrinsic_store_scratch;
   bool is_shared = low->intrinsic == nir_intrinsic_load_shared ||
                    low->intrinsic == nir_intrinsic_store_shared ||
                    low->intrinsic == nir_intrinsic_load_deref ||
                    low->intrinsic == nir_intrinsic_store_deref;

   assert(!is_store || hole_size <= 0);

   /* If we get derefs here, only shared memory derefs are expected. */
   assert((low->intrinsic != nir_intrinsic_load_deref &&
           low->intrinsic != nir_intrinsic_store_deref) ||
          nir_deref_mode_is(nir_src_as_deref(low->src[0]), nir_var_mem_shared));

   /* Don't vectorize descriptor loads for LLVM due to excessive SGPR and VGPR spilling. */
   if (!config->uses_aco && low->intrinsic == nir_intrinsic_load_smem_amd)
      return false;

   /* Reject opcodes we don't vectorize. */
   switch (low->intrinsic) {
   case nir_intrinsic_load_smem_amd:
   case nir_intrinsic_load_push_constant:
   case nir_intrinsic_load_ubo:
   case nir_intrinsic_load_stack:
   case nir_intrinsic_store_stack:
   case nir_intrinsic_load_scratch:
   case nir_intrinsic_store_scratch:
   case nir_intrinsic_load_global_constant:
   case nir_intrinsic_load_global:
   case nir_intrinsic_store_global:
   case nir_intrinsic_load_ssbo:
   case nir_intrinsic_store_ssbo:
   case nir_intrinsic_load_deref:
   case nir_intrinsic_store_deref:
   case nir_intrinsic_load_shared:
   case nir_intrinsic_store_shared:
      break;
   default:
      return false;
   }

   /* Align the size to what the hw supports. */
   unsigned unaligned_new_size = num_components * bit_size;
   unsigned aligned_new_size = align_load_store_size(config->gfx_level, unaligned_new_size,
                                                     uses_smem, is_shared);

   if (uses_smem) {
      /* Maximize SMEM vectorization except for LLVM, which suffers from SGPR and VGPR spilling.
       * GFX6-7 have fewer hw SGPRs, so merge only up to 128 bits to limit SGPR usage.
       */
      if (aligned_new_size > (config->gfx_level >= GFX8 ? (config->uses_aco ? 512 : 256) : 128))
         return false;
   } else {
      if (aligned_new_size > 128)
         return false;

      /* GFX6-8 only support 32-bit scratch loads/stores. */
      if (config->gfx_level <= GFX8 && is_scratch && aligned_new_size > 32)
         return false;
   }

   if (!is_store) {
      /* Non-descriptor loads. */
      if (low->intrinsic != nir_intrinsic_load_ubo &&
          low->intrinsic != nir_intrinsic_load_ssbo) {
         /* Only increase the size of loads if doing so doesn't extend into a new page.
          * Here we set alignment to MAX because we don't know the alignment of global
          * pointers before adding the offset.
          */
         uint32_t resource_align = low->intrinsic == nir_intrinsic_load_global_constant ||
                                   low->intrinsic == nir_intrinsic_load_global ? NIR_ALIGN_MUL_MAX : 4;
         uint32_t page_size = 4096;
         uint32_t mul = MIN3(align_mul, page_size, resource_align);
         unsigned end = (align_offset + unaligned_new_size / 8u) & (mul - 1);
         if ((aligned_new_size - unaligned_new_size) / 8u > (mul - end))
            return false;
      }

      /* Only allow SMEM loads to overfetch by 32 bits:
       *
       * Examples (the hole is indicated by parentheses, the numbers are  in bytes, the maximum
       * overfetch size is 4):
       *    4  | (4) | 4   ->  hw loads 12  : ALLOWED    (4 over)
       *    4  | (4) | 4   ->  hw loads 16  : DISALLOWED (8 over)
       *    4  |  4  | 4   ->  hw loads 16  : ALLOWED    (4 over)
       *    4  | (4) | 8   ->  hw loads 16  : ALLOWED    (4 over)
       *    16 |  4        ->  hw loads 32  : DISALLOWED (12 over)
       *    16 |  8        ->  hw loads 32  : DISALLOWED (8 over)
       *    16 | 12        ->  hw loads 32  : ALLOWED    (4 over)
       *    16 | (4) | 12  ->  hw loads 32  : ALLOWED    (4 over)
       *    32 | 16        ->  hw loads 64  : DISALLOWED (16 over)
       *    32 | 28        ->  hw loads 64  : ALLOWED    (4 over)
       *    32 | (4) | 28  ->  hw loads 64  : ALLOWED    (4 over)
       *
       * Note that we can overfetch by more than 4 bytes if we merge more than 2 loads, e.g.:
       *    4  | (4) | 8 | (4) | 12  ->  hw loads 32  : ALLOWED (4 + 4 over)
       *
       * That's because this callback is called twice in that case, each time allowing only 4 over.
       *
       * This is only enabled for ACO. LLVM spills SGPRs and VGPRs too much.
       */
      unsigned overfetch_size = 0;

      if (config->uses_aco && uses_smem && aligned_new_size >= 128)
         overfetch_size = 32;

      int64_t aligned_unvectorized_size =
         align_load_store_size(config->gfx_level, low->num_components * low->def.bit_size,
                               uses_smem, is_shared) +
         align_load_store_size(config->gfx_level, high->num_components * high->def.bit_size,
                               uses_smem, is_shared);

      if (aligned_new_size > aligned_unvectorized_size + overfetch_size)
         return false;
   }

   uint32_t align;
   if (align_offset)
      align = 1 << (ffs(align_offset) - 1);
   else
      align = align_mul;

   /* Validate the alignment and number of components. */
   if (!is_shared) {
      unsigned max_components;
      if (align % 4 == 0)
         max_components = NIR_MAX_VEC_COMPONENTS;
      else if (align % 2 == 0)
         max_components = 16u / bit_size;
      else
         max_components = 8u / bit_size;
      return (align % (bit_size / 8u)) == 0 && num_components <= max_components;
   } else {
      if (bit_size * num_components == 96) { /* 96 bit loads require 128 bit alignment and are split otherwise */
         return align % 16 == 0;
      } else if (bit_size == 16 && (align % 4)) {
         /* AMD hardware can't do 2-byte aligned f16vec2 loads, but they are useful for ALU
          * vectorization, because our vectorizer requires the scalar IR to already contain vectors.
          */
         return (align % 2 == 0) && num_components <= 2;
      } else {
         if (num_components == 3) {
            /* AMD hardware can't do 3-component loads except for 96-bit loads, handled above. */
            return false;
         }
         unsigned req = bit_size * num_components;
         if (req == 64 || req == 128) /* 64-bit and 128-bit loads can use ds_read2_b{32,64} */
            req /= 2u;
         return align % (req / 8u) == 0;
      }
   }
   return false;
}

bool ac_nir_scalarize_overfetching_loads_callback(const nir_instr *instr, const void *data)
{
   nir_intrinsic_instr *intr = nir_instr_as_intrinsic(instr);

   /* Reject opcodes we don't scalarize. */
   switch (intr->intrinsic) {
   case nir_intrinsic_load_ubo:
   case nir_intrinsic_load_ssbo:
   case nir_intrinsic_load_global:
   case nir_intrinsic_load_global_constant:
   case nir_intrinsic_load_shared:
      break;
   default:
      return false;
   }

   bool uses_smem = nir_intrinsic_has_access(intr) &&
                    nir_intrinsic_access(intr) & ACCESS_SMEM_AMD;
   bool is_shared = intr->intrinsic == nir_intrinsic_load_shared;

   enum amd_gfx_level gfx_level = *(enum amd_gfx_level *)data;
   unsigned comp_size = intr->def.bit_size / 8;
   unsigned load_size = intr->def.num_components * comp_size;
   unsigned used_load_size = util_bitcount(nir_def_components_read(&intr->def)) * comp_size;

   /* Scalarize if the load overfetches. That includes loads that overfetch due to load size
    * alignment, e.g. when only a power-of-two load is available. The scalarized loads are expected
    * to be later vectorized to optimal sizes.
    */
   return used_load_size < align_load_store_size(gfx_level, load_size, uses_smem, is_shared);
}

/* Get chip-agnostic memory instruction access flags (as opposed to chip-specific GLC/DLC/SLC)
 * from a NIR memory intrinsic.
 */
enum gl_access_qualifier ac_nir_get_mem_access_flags(const nir_intrinsic_instr *instr)
{
   enum gl_access_qualifier access =
      nir_intrinsic_has_access(instr) ? nir_intrinsic_access(instr) : 0;

   /* Determine ACCESS_MAY_STORE_SUBDWORD. (for the GFX6 TC L1 bug workaround) */
   if (!nir_intrinsic_infos[instr->intrinsic].has_dest) {
      switch (instr->intrinsic) {
      case nir_intrinsic_bindless_image_store:
         access |= ACCESS_MAY_STORE_SUBDWORD;
         break;

      case nir_intrinsic_store_ssbo:
      case nir_intrinsic_store_buffer_amd:
      case nir_intrinsic_store_global:
      case nir_intrinsic_store_global_amd:
         if (access & ACCESS_USES_FORMAT_AMD ||
             (nir_intrinsic_has_align_offset(instr) && nir_intrinsic_align(instr) % 4 != 0) ||
             ((instr->src[0].ssa->bit_size / 8) * instr->src[0].ssa->num_components) % 4 != 0)
            access |= ACCESS_MAY_STORE_SUBDWORD;
         break;

      default:
         unreachable("unexpected store instruction");
      }
   }

   return access;
}