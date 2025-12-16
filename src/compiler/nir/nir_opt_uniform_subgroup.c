/*
 * Copyright 2023 Intel Corporation
 * SPDX-License-Identifier: MIT
 */

/**
 * \file
 * Optimize subgroup operations with uniform sources.
 */

#include "nir/nir.h"
#include "nir/nir_builder.h"

struct select_info {
   nir_def *cond;
   nir_def *values[2];
};

static bool
parse_select_of_con_values(nir_builder *b, nir_def *def, struct select_info *info)
{
   if (!nir_def_is_alu(def))
      return false;

   nir_alu_instr *alu = nir_def_as_alu(def);
   unsigned bit_size = def->bit_size;
   unsigned num_components = def->num_components;
   nir_block *use_block = nir_cursor_current_block(b->cursor);

   switch (alu->op) {
   case nir_op_b2f16:
   case nir_op_b2f32:
   case nir_op_b2f64: {
      info->cond = nir_mov_alu(b, alu->src[0], num_components);
      for (unsigned i = 0; i < 2; i++)
         info->values[i] = nir_imm_floatN_t(b, i ? 0.0 : 1.0, bit_size);

      return true;
   }
   case nir_op_b2i8:
   case nir_op_b2i16:
   case nir_op_b2i32:
   case nir_op_b2i64: {
      info->cond = nir_mov_alu(b, alu->src[0], num_components);
      for (unsigned i = 0; i < 2; i++)
         info->values[i] = nir_imm_intN_t(b, i ? 0 : 1, bit_size);

      return true;
   }
   case nir_op_fneg:
   case nir_op_ineg: {
      /* nir_opt_algebraic canonicalizes a ? -1 : 0 to neg(b2f/b2i(a)),
       * so look for this here.
       */
      nir_alu_instr *b2t = nir_def_as_alu_or_null(alu->src[0].src.ssa);

      bool is_float = alu->op == nir_op_fneg;
      nir_alu_type dest_type = (is_float ? nir_type_float : nir_type_uint) | bit_size;

      nir_op b2t_op = nir_type_conversion_op(nir_type_bool1, dest_type, nir_rounding_mode_undef);

      if (!b2t || b2t->op != b2t_op)
         return false;

      nir_alu_src neg_src = { NIR_SRC_INIT };
      neg_src.src = nir_src_for_ssa(nir_mov_alu(b, b2t->src[0], b2t->def.num_components));
      memcpy(neg_src.swizzle, alu->src[0].swizzle, sizeof(alu->src[0].swizzle));

      info->cond = nir_mov_alu(b, neg_src, num_components);

      for (unsigned i = 0; i < 2; i++) {
         if (is_float)
            info->values[i] = nir_imm_floatN_t(b, i ? -0.0 : -1.0, bit_size);
         else
            info->values[i] = nir_imm_intN_t(b, i ? 0 : -1, bit_size);
      }

      return true;
   }
   case nir_op_bcsel: {
      for (unsigned i = 0; i < 2; i++) {
         if (nir_def_is_divergent_at_use_block(alu->src[1 + i].src.ssa, use_block))
            return false;
      }

      info->cond = nir_mov_alu(b, alu->src[0], num_components);
      for (unsigned i = 0; i < 2; i++)
         info->values[i] = nir_mov_alu(b, alu->src[1 + i], num_components);

      return true;
   }
   default:
      return false;
   }
}

static nir_def *
ballot_bit_count(nir_builder *b, nir_def *ballot)
{
   return ballot->num_components == 1
             ? nir_bit_count(b, ballot)
             : nir_ballot_bit_count_reduce(b, ballot);
}

static nir_def *
count_active_invocations(nir_builder *b, nir_def *value, bool inclusive,
                         const nir_lower_subgroups_options *options)
{
   /* For the non-inclusive case, the two paths are functionally the same.
    * For the inclusive case, the are similar but very subtly different.
    *
    * The bit_count path will mask "value" with the subgroup LE mask instead
    * of the subgroup LT mask. This is the definition of the inclusive count.
    *
    * AMD's mbcnt instruction always uses the subgroup LT mask. To perform the
    * inclusive count using mbcnt, two assumptions are made. First, trivially,
    * the current invocation is active. Second, the bit for the current
    * invocation in "value" is set.  Since "value" is assumed to be the result
    * of ballot(true), the second condition will also be met.
    *
    * When those conditions are met, the inclusive count is the exclusive
    * count plus one.
    */
   if (options->lower_ballot_bit_count_to_mbcnt_amd) {
      return nir_mbcnt_amd(b, value, nir_imm_int(b, (int)inclusive));
   } else {
      nir_def *mask =
         inclusive ? nir_load_subgroup_le_mask(b, options->ballot_components,
                                               options->ballot_bit_size)
                   : nir_load_subgroup_lt_mask(b, options->ballot_components,
                                               options->ballot_bit_size);

      return ballot_bit_count(b, nir_iand(b, value, mask));
   }
}

static bool
opt_uniform_subgroup_instr(nir_builder *b, nir_intrinsic_instr *intrin, void *_state)
{
   const nir_lower_subgroups_options *options = (nir_lower_subgroups_options *)_state;

   b->cursor = nir_before_instr(&intrin->instr);

   nir_def *replacement = NULL;
   switch (intrin->intrinsic) {
   case nir_intrinsic_quad_swizzle_amd:
   case nir_intrinsic_masked_swizzle_amd:
      if (!nir_intrinsic_fetch_inactive(intrin))
         return false;
      FALLTHROUGH;
   case nir_intrinsic_shuffle:
   case nir_intrinsic_shuffle_up:
   case nir_intrinsic_shuffle_down:
   case nir_intrinsic_rotate:
   case nir_intrinsic_read_invocation:
   case nir_intrinsic_read_first_invocation:
   case nir_intrinsic_quad_broadcast:
   case nir_intrinsic_quad_swap_horizontal:
   case nir_intrinsic_quad_swap_vertical:
   case nir_intrinsic_quad_swap_diagonal:
   case nir_intrinsic_vote_all:
   case nir_intrinsic_vote_any:
   case nir_intrinsic_quad_vote_all:
   case nir_intrinsic_quad_vote_any:
   case nir_intrinsic_vote_feq:
   case nir_intrinsic_vote_ieq:
      if (nir_src_is_divergent(&intrin->src[0]))
         return false;

      if (intrin->intrinsic == nir_intrinsic_vote_feq) {
         nir_def *x = intrin->src[0].ssa;
         b->exact = true;
         replacement = nir_feq(b, x, x);
         b->exact = false;
      } else if (intrin->intrinsic == nir_intrinsic_vote_ieq) {
         replacement = nir_imm_true(b);
      } else {
         replacement = intrin->src[0].ssa;
      }
      break;

   case nir_intrinsic_reduce:
   case nir_intrinsic_exclusive_scan:
   case nir_intrinsic_inclusive_scan: {
      const nir_op reduction_op = (nir_op)nir_intrinsic_reduction_op(intrin);

      switch (reduction_op) {
      case nir_op_iadd:
      case nir_op_fadd:
      case nir_op_ixor: {
         if (nir_src_is_divergent(&intrin->src[0]))
            return false;
         if (nir_intrinsic_has_cluster_size(intrin) && nir_intrinsic_cluster_size(intrin))
            return false;
         nir_def *count;

         nir_def *ballot = nir_ballot(b, options->ballot_components,
                                      options->ballot_bit_size, nir_imm_true(b));

         if (intrin->intrinsic == nir_intrinsic_reduce) {
            count = ballot_bit_count(b, ballot);
         } else {
            count = count_active_invocations(b, ballot,
                                             intrin->intrinsic == nir_intrinsic_inclusive_scan,
                                             options);
         }

         const unsigned bit_size = intrin->src[0].ssa->bit_size;

         if (reduction_op == nir_op_iadd) {
            replacement = nir_imul(b, nir_u2uN(b, count, bit_size),
                                   intrin->src[0].ssa);
         } else if (reduction_op == nir_op_fadd) {
            replacement = nir_fmul(b, nir_u2fN(b, count, bit_size),
                                   intrin->src[0].ssa);
         } else {
            replacement = nir_imul(b,
                                   nir_u2uN(b,
                                            nir_iand(b, count, nir_imm_int(b, 1)),
                                            bit_size),
                                   intrin->src[0].ssa);
         }
         break;
      }

      case nir_op_imin:
      case nir_op_umin:
      case nir_op_fmin:
      case nir_op_imax:
      case nir_op_umax:
      case nir_op_fmax:
      case nir_op_iand:
      case nir_op_ior:
         if (intrin->intrinsic == nir_intrinsic_exclusive_scan)
            return false;
         if (!nir_src_is_divergent(&intrin->src[0])) {
            replacement = intrin->src[0].ssa;
         } else {
            if (intrin->intrinsic != nir_intrinsic_reduce)
               return false;
            if (nir_intrinsic_cluster_size(intrin))
               return false;
            if (intrin->def.num_components != 1)
               return false;

            struct select_info sel;
            if (!parse_select_of_con_values(b, intrin->src[0].ssa, &sel))
               return false;

            nir_def *mix_value = nir_build_alu2(b, reduction_op, sel.values[0], sel.values[1]);

            replacement = nir_bcsel(b, nir_vote_any(b, 1, sel.cond),
                                    nir_bcsel(b, nir_vote_all(b, 1, sel.cond), sel.values[0], mix_value),
                                    sel.values[1]);
         }
         break;

      default:
         return false;
      }
      break;
   }
   default:
      return false;
   }

   nir_def_replace(&intrin->def, replacement);
   return true;
}

bool
nir_opt_uniform_subgroup(nir_shader *shader,
                         const nir_lower_subgroups_options *options)
{
   nir_divergence_analysis(shader);

   return nir_shader_intrinsics_pass(shader,
                                     opt_uniform_subgroup_instr,
                                     nir_metadata_control_flow,
                                     (void *)options);
}
