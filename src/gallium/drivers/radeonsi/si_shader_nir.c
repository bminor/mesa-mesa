/*
 * Copyright 2017 Advanced Micro Devices, Inc.
 *
 * SPDX-License-Identifier: MIT
 */

#include "nir_builder.h"
#include "nir_xfb_info.h"
#include "si_pipe.h"
#include "ac_nir.h"
#include "si_shader_internal.h"

void si_nir_opts(struct si_screen *sscreen, struct nir_shader *nir, bool has_array_temps)
{
   bool progress;

   do {
      progress = false;
      bool lower_alu_to_scalar = false;
      bool lower_phis_to_scalar = false;

      NIR_PASS(progress, nir, nir_lower_vars_to_ssa);
      NIR_PASS(progress, nir, nir_lower_alu_to_scalar, nir->options->lower_to_scalar_filter, NULL);
      NIR_PASS(progress, nir, nir_lower_phis_to_scalar, NULL, NULL);

      if (has_array_temps) {
         NIR_PASS(progress, nir, nir_split_array_vars, nir_var_function_temp);
         NIR_PASS(lower_alu_to_scalar, nir, nir_shrink_vec_array_vars, nir_var_function_temp);
         NIR_PASS(progress, nir, nir_opt_find_array_copies);
      }
      NIR_PASS(progress, nir, nir_opt_copy_prop_vars);
      NIR_PASS(progress, nir, nir_opt_dead_write_vars);

      NIR_PASS(lower_alu_to_scalar, nir, nir_opt_loop);
      /* (Constant) copy propagation is needed for txf with offsets. */
      NIR_PASS(progress, nir, nir_opt_copy_prop);
      NIR_PASS(progress, nir, nir_opt_remove_phis);
      NIR_PASS(progress, nir, nir_opt_dce);
      /* nir_opt_if_optimize_phi_true_false is disabled on LLVM14 (#6976) */
      NIR_PASS(lower_phis_to_scalar, nir, nir_opt_if,
               nir_opt_if_optimize_phi_true_false);
      NIR_PASS(progress, nir, nir_opt_dead_cf);

      if (lower_alu_to_scalar) {
         NIR_PASS(_, nir, nir_lower_alu_to_scalar, nir->options->lower_to_scalar_filter, NULL);
      }
      if (lower_phis_to_scalar)
         NIR_PASS(_, nir, nir_lower_phis_to_scalar, NULL, NULL);
      progress |= lower_alu_to_scalar | lower_phis_to_scalar;

      NIR_PASS(progress, nir, nir_opt_cse);

      nir_opt_peephole_select_options peephole_select_options = {
         .limit = 8,
         .indirect_load_ok = true,
         .expensive_alu_ok = true,
      };
      NIR_PASS(progress, nir, nir_opt_peephole_select, &peephole_select_options);

      /* Needed for algebraic lowering */
      NIR_PASS(progress, nir, nir_opt_algebraic);
      NIR_PASS(progress, nir, nir_opt_generate_bfi);
      NIR_PASS(progress, nir, nir_opt_constant_folding);

      if (!nir->info.flrp_lowered) {
         unsigned lower_flrp = (nir->options->lower_flrp16 ? 16 : 0) |
                               (nir->options->lower_flrp32 ? 32 : 0) |
                               (nir->options->lower_flrp64 ? 64 : 0);
         assert(lower_flrp);
         NIR_PASS(progress, nir, nir_lower_flrp, lower_flrp, false /* always_precise */);

         /* Nothing should rematerialize any flrps, so we only
          * need to do this lowering once.
          */
         nir->info.flrp_lowered = true;
      }

      NIR_PASS(progress, nir, nir_opt_undef);
      NIR_PASS(progress, nir, nir_opt_shrink_vectors, true);

      nir_opt_peephole_select_options peephole_discard_options = {
         .limit = 0,
         .discard_ok = true,
      };
      NIR_PASS(progress, nir, nir_opt_peephole_select, &peephole_discard_options);
      if (nir->options->max_unroll_iterations) {
         NIR_PASS(progress, nir, nir_opt_loop_unroll);
      }

      if (nir->info.stage == MESA_SHADER_FRAGMENT)
         NIR_PASS(_, nir, nir_opt_move_discards_to_top);
   } while (progress);

   NIR_PASS(_, nir, nir_lower_var_copies);
}

void si_nir_late_opts(nir_shader *nir)
{
   bool more_late_algebraic = true;
   while (more_late_algebraic) {
      more_late_algebraic = false;
      NIR_PASS(more_late_algebraic, nir, nir_opt_algebraic_late);
      NIR_PASS(_, nir, nir_opt_constant_folding);
      NIR_PASS(_, nir, nir_opt_copy_prop);
      NIR_PASS(_, nir, nir_opt_dce);
      NIR_PASS(_, nir, nir_opt_cse);
   }
}

static bool
lower_intrinsic_filter(const nir_instr *instr, const void *dummy)
{
   return instr->type == nir_instr_type_intrinsic;
}

static nir_def *
lower_intrinsic_instr(nir_builder *b, nir_instr *instr, void *dummy)
{
   nir_intrinsic_instr *intrin = nir_instr_as_intrinsic(instr);

   switch (intrin->intrinsic) {
   case nir_intrinsic_is_sparse_texels_resident:
      /* code==0 means sparse texels are resident */
      return nir_ieq_imm(b, intrin->src[0].ssa, 0);
   case nir_intrinsic_sparse_residency_code_and:
      return nir_ior(b, intrin->src[0].ssa, intrin->src[1].ssa);
   default:
      return NULL;
   }
}

static bool si_lower_intrinsics(nir_shader *nir)
{
   return nir_shader_lower_instructions(nir,
                                        lower_intrinsic_filter,
                                        lower_intrinsic_instr,
                                        NULL);
}

/**
 * Perform "lowering" operations on the NIR that are run once when the shader
 * selector is created.
 */
static void si_lower_nir(struct si_screen *sscreen, struct nir_shader *nir)
{
   /* Perform lowerings (and optimizations) of code.
    *
    * Performance considerations aside, we must:
    * - lower certain ALU operations
    * - ensure constant offsets for texture instructions are folded
    *   and copy-propagated
    */
   const struct nir_lower_tex_options lower_tex_options = {
      .lower_txp = ~0u,
      .lower_txf_offset = true,
      .lower_txs_cube_array = true,
      .lower_invalid_implicit_lod = true,
      .lower_tg4_offsets = true,
      .lower_to_fragment_fetch_amd = sscreen->info.gfx_level < GFX11,
      .lower_1d = sscreen->info.gfx_level == GFX9,
      .optimize_txd = true,
   };
   NIR_PASS(_, nir, nir_lower_tex, &lower_tex_options);

   const struct nir_lower_image_options lower_image_options = {
      .lower_cube_size = true,
      .lower_to_fragment_mask_load_amd = sscreen->info.gfx_level < GFX11 &&
                                         !(sscreen->debug_flags & DBG(NO_FMASK)),
   };
   NIR_PASS(_, nir, nir_lower_image, &lower_image_options);

   NIR_PASS(_, nir, si_lower_intrinsics);

   NIR_PASS(_, nir, ac_nir_lower_sin_cos);

   /* Lower load constants to scalar and then clean up the mess */
   NIR_PASS(_, nir, nir_lower_load_const_to_scalar);
   NIR_PASS(_, nir, nir_lower_var_copies);
   NIR_PASS(_, nir, nir_opt_intrinsics);
   NIR_PASS(_, nir, nir_lower_system_values);

   si_nir_opts(sscreen, nir, true);
   /* Run late optimizations to fuse ffma and eliminate 16-bit conversions. */
   si_nir_late_opts(nir);

   NIR_PASS(_, nir, nir_remove_dead_variables, nir_var_function_temp, NULL);
}

void si_finalize_nir(struct pipe_screen *screen, struct nir_shader *nir,
                     bool optimize)
{
   struct si_screen *sscreen = (struct si_screen *)screen;

   if (nir->info.io_lowered) {
      nir_foreach_variable_with_modes(var, nir, nir_var_shader_in | nir_var_shader_out) {
         UNREACHABLE("no IO variables should be present with lowered IO");
      }
   } else {
      nir_lower_io_passes(nir, false);
      NIR_PASS(_, nir, nir_remove_dead_variables, nir_var_shader_in | nir_var_shader_out, NULL);
   }

   if (nir->info.stage == MESA_SHADER_FRAGMENT) {
      NIR_PASS(_, nir, si_nir_lower_color_inputs_to_sysvals);
      /* We must renumber FS input bases after lowering color inputs to sysvals. */
      NIR_PASS(_, nir, nir_recompute_io_bases, nir_var_shader_in | nir_var_shader_out);
   }

   /* Remove dead derefs, so that we can remove uniforms. */
   NIR_PASS(_, nir, nir_opt_dce);

   /* Remove uniforms because those should have been lowered to UBOs already. */
   nir_foreach_variable_with_modes_safe(var, nir, nir_var_uniform) {
      if (!glsl_type_get_image_count(var->type) &&
          !glsl_type_get_texture_count(var->type) &&
          !glsl_type_get_sampler_count(var->type))
         exec_node_remove(&var->node);
   }

   si_lower_nir(sscreen, nir);
   nir_shader_gather_info(nir, nir_shader_get_entrypoint(nir));

   if (sscreen->options.inline_uniforms)
      nir_find_inlinable_uniforms(nir);
}
