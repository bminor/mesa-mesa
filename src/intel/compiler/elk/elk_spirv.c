/*
 * Copyright © 2025 Intel Corporation
 * SPDX-License-Identifier: MIT
 */

#include "elk_nir.h"
#include "elk_nir_options.h"

#include "../intel_nir.h"
#include "compiler/nir/nir_builder.h"
#include "compiler/spirv/nir_spirv.h"
#include "compiler/spirv/spirv_info.h"
#include "dev/intel_debug.h"
#include "util/u_dynarray.h"

static nir_def *
rebuild_value_from_store(struct util_dynarray *stores,
                         nir_def *value, unsigned read_offset)
{
   unsigned read_size = value->num_components * value->bit_size / 8;

   util_dynarray_foreach(stores, nir_intrinsic_instr *, _store) {
      nir_intrinsic_instr *store = *_store;

      unsigned write_offset = nir_src_as_uint(store->src[1]);
      unsigned write_size = nir_src_num_components(store->src[0]) *
                            nir_src_bit_size(store->src[0]) / 8;
      if (write_offset <= read_offset &&
          (write_offset + write_size) >= (read_offset + read_size)) {
         assert(nir_block_dominates(store->instr.block, value->parent_instr->block));
         assert(write_size == read_size);
         return store->src[0].ssa;
      }
   }
   unreachable("Matching scratch store not found");
}

/**
 * Remove temporary variables stored to scratch to be then reloaded
 * immediately. Remap the load to the store SSA value.
 *
 * This workaround is only meant to be applied to shaders in src/intel/shaders
 * were we know there should be no issue. More complex cases might not work
 * with this approach.
 */
static bool
nir_remove_llvm17_scratch(nir_shader *nir)
{
   struct util_dynarray scratch_stores;
   void *mem_ctx = ralloc_context(NULL);

   util_dynarray_init(&scratch_stores, mem_ctx);

   nir_foreach_function_impl(func, nir) {
      nir_foreach_block(block, func) {
         nir_foreach_instr(instr, block) {
            if (instr->type != nir_instr_type_intrinsic)
               continue;

            nir_intrinsic_instr *intrin = nir_instr_as_intrinsic(instr);

            if (intrin->intrinsic != nir_intrinsic_store_scratch)
               continue;

            nir_const_value *offset = nir_src_as_const_value(intrin->src[1]);
            if (offset != NULL) {
               util_dynarray_append(&scratch_stores, nir_intrinsic_instr *, intrin);
            }
         }
      }
   }

   bool progress = false;
   if (util_dynarray_num_elements(&scratch_stores, nir_intrinsic_instr *) > 0) {
      nir_foreach_function_impl(func, nir) {
         nir_foreach_block(block, func) {
            nir_foreach_instr_safe(instr, block) {
               if (instr->type != nir_instr_type_intrinsic)
                  continue;

               nir_intrinsic_instr *intrin = nir_instr_as_intrinsic(instr);

               if (intrin->intrinsic != nir_intrinsic_load_scratch)
                  continue;

               nir_const_value *offset = nir_src_as_const_value(intrin->src[0]);
               if (offset == NULL)
                  continue;

               nir_def_replace(&intrin->def,
                               rebuild_value_from_store(&scratch_stores, &intrin->def, nir_src_as_uint(intrin->src[0])));

               progress = true;
            }
         }
      }
   }

   util_dynarray_foreach(&scratch_stores, nir_intrinsic_instr *, _store) {
      nir_intrinsic_instr *store = *_store;
      nir_instr_remove(&store->instr);
   }

   /* Quick sanity check */
   assert(util_dynarray_num_elements(&scratch_stores, nir_intrinsic_instr *) == 0 ||
          progress);

   ralloc_free(mem_ctx);

   return progress;
}

static void
cleanup_llvm17_scratch(nir_shader *nir)
{
   {
      bool progress;
      do {
         progress = false;
         NIR_PASS(progress, nir, nir_copy_prop);
         NIR_PASS(progress, nir, nir_opt_dce);
         NIR_PASS(progress, nir, nir_opt_constant_folding);
         NIR_PASS(progress, nir, nir_opt_cse);
         NIR_PASS(progress, nir, nir_opt_algebraic);
      } while (progress);
   }

   nir_remove_llvm17_scratch(nir);

   {
      bool progress;
      do {
         progress = false;
         NIR_PASS(progress, nir, nir_copy_prop);
         NIR_PASS(progress, nir, nir_opt_dce);
         NIR_PASS(progress, nir, nir_opt_constant_folding);
         NIR_PASS(progress, nir, nir_opt_cse);
         NIR_PASS(progress, nir, nir_opt_algebraic);
      } while (progress);
   }
}

static const struct spirv_capabilities spirv_caps = {
   .Addresses = true,
   .Float16 = true,
   .Float64 = true,
   .Groups = true,
   .StorageImageWriteWithoutFormat = true,
   .Int8 = true,
   .Int16 = true,
   .Int64 = true,
   .Int64Atomics = true,
   .Kernel = true,
   .Linkage = true, /* We receive linked kernel from clc */
   .DenormFlushToZero = true,
   .DenormPreserve = true,
   .SignedZeroInfNanPreserve = true,
   .RoundingModeRTE = true,
   .RoundingModeRTZ = true,
   .GenericPointer = true,
   .GroupNonUniform = true,
   .GroupNonUniformArithmetic = true,
   .GroupNonUniformClustered = true,
   .GroupNonUniformBallot = true,
   .GroupNonUniformQuad = true,
   .GroupNonUniformShuffle = true,
   .GroupNonUniformVote = true,
   .SubgroupDispatch = true,
};

nir_shader *
elk_nir_from_spirv(void *mem_ctx, unsigned gfx_version, const uint32_t *spirv,
                   size_t spirv_size, bool llvm17_wa)
{
   assert(gfx_version < 9);

   struct spirv_to_nir_options spirv_options = {
      .environment = NIR_SPIRV_OPENCL,
      .capabilities = &spirv_caps,
      .printf = true,
      .shared_addr_format = nir_address_format_62bit_generic,
      .global_addr_format = nir_address_format_62bit_generic,
      .temp_addr_format = nir_address_format_62bit_generic,
      .constant_addr_format = nir_address_format_64bit_global,
      .create_library = true,
   };

   assert(spirv_size % 4 == 0);

   const nir_shader_compiler_options *nir_options = &elk_scalar_nir_options;

   nir_shader *nir =
      spirv_to_nir(spirv, spirv_size / 4, NULL, 0, MESA_SHADER_KERNEL,
                   "library", &spirv_options, nir_options);
   nir_validate_shader(nir, "after spirv_to_nir");
   nir_validate_ssa_dominance(nir, "after spirv_to_nir");
   ralloc_steal(mem_ctx, nir);
   nir->info.name = ralloc_strdup(nir, "library");

   if (INTEL_DEBUG(DEBUG_CS)) {
      /* Re-index SSA defs so we print more sensible numbers. */
      nir_foreach_function_impl(impl, nir) {
         nir_index_ssa_defs(impl);
      }

      fprintf(stderr, "NIR (from SPIR-V) for kernel\n");
      nir_print_shader(nir, stderr);
   }

   nir_lower_printf_options printf_opts = {
      .ptr_bit_size               = 64,
      .use_printf_base_identifier = true,
   };
   NIR_PASS_V(nir, nir_lower_printf, &printf_opts);

   NIR_PASS_V(nir, nir_link_shader_functions, spirv_options.clc_shader);

   /* We have to lower away local constant initializers right before we
    * inline functions.  That way they get properly initialized at the top
    * of the function and not at the top of its caller.
    */
   NIR_PASS_V(nir, nir_lower_variable_initializers, ~(nir_var_shader_temp |
                                                      nir_var_function_temp));
   NIR_PASS_V(nir, nir_remove_dead_variables, nir_var_uniform | nir_var_mem_ubo |
              nir_var_mem_constant | nir_var_function_temp | nir_var_image, NULL);
   {
      bool progress;
      do
      {
         progress = false;
         NIR_PASS(progress, nir, nir_copy_prop);
         NIR_PASS(progress, nir, nir_opt_copy_prop_vars);
         NIR_PASS(progress, nir, nir_opt_deref);
         NIR_PASS(progress, nir, nir_opt_dce);
         NIR_PASS(progress, nir, nir_opt_undef);
         NIR_PASS(progress, nir, nir_opt_constant_folding);
         NIR_PASS(progress, nir, nir_opt_cse);
         NIR_PASS(progress, nir, nir_lower_vars_to_ssa);
         NIR_PASS(progress, nir, nir_opt_algebraic);
      } while (progress);
   }

   NIR_PASS_V(nir, nir_lower_variable_initializers, nir_var_function_temp);
   NIR_PASS_V(nir, nir_lower_returns);
   NIR_PASS_V(nir, nir_inline_functions);

   assert(nir->scratch_size == 0);
   NIR_PASS_V(nir, nir_lower_vars_to_explicit_types, nir_var_function_temp, glsl_get_cl_type_size_align);

   {
      bool progress;
      do
      {
         progress = false;
         NIR_PASS(progress, nir, nir_copy_prop);
         NIR_PASS(progress, nir, nir_opt_copy_prop_vars);
         NIR_PASS(progress, nir, nir_opt_deref);
         NIR_PASS(progress, nir, nir_opt_dce);
         NIR_PASS(progress, nir, nir_opt_undef);
         NIR_PASS(progress, nir, nir_opt_constant_folding);
         NIR_PASS(progress, nir, nir_opt_cse);
         NIR_PASS(progress, nir, nir_split_var_copies);
         NIR_PASS(progress, nir, nir_lower_var_copies);
         NIR_PASS(progress, nir, nir_lower_vars_to_ssa);
         NIR_PASS(progress, nir, nir_opt_algebraic);
         NIR_PASS(progress, nir, nir_opt_if, nir_opt_if_optimize_phi_true_false);
         NIR_PASS(progress, nir, nir_opt_dead_cf);
         NIR_PASS(progress, nir, nir_opt_remove_phis);
         NIR_PASS(progress, nir, nir_opt_peephole_select, 8, true, true);
         NIR_PASS(progress, nir, nir_lower_vec3_to_vec4, nir_var_mem_generic | nir_var_uniform);
         NIR_PASS(progress, nir, nir_opt_memcpy);
      } while (progress);
   }

   NIR_PASS_V(nir, nir_scale_fdiv);

   NIR_PASS_V(nir, nir_remove_dead_variables, nir_var_uniform | nir_var_mem_ubo |
              nir_var_mem_constant | nir_var_function_temp | nir_var_image, NULL);


   NIR_PASS_V(nir, nir_remove_dead_variables, nir_var_mem_shared | nir_var_function_temp, NULL);

   nir->scratch_size = 0;
   NIR_PASS_V(nir, nir_lower_vars_to_explicit_types,
              nir_var_mem_shared | nir_var_function_temp | nir_var_shader_temp |
              nir_var_mem_global | nir_var_mem_constant,
              glsl_get_cl_type_size_align);

   // Lower memcpy - needs to wait until types are sized
   {
      bool progress;
      do {
         progress = false;
         NIR_PASS(progress, nir, nir_opt_memcpy);
         NIR_PASS(progress, nir, nir_copy_prop);
         NIR_PASS(progress, nir, nir_opt_copy_prop_vars);
         NIR_PASS(progress, nir, nir_opt_deref);
         NIR_PASS(progress, nir, nir_opt_dce);
         NIR_PASS(progress, nir, nir_split_var_copies);
         NIR_PASS(progress, nir, nir_lower_var_copies);
         NIR_PASS(progress, nir, nir_lower_vars_to_ssa);
         NIR_PASS(progress, nir, nir_opt_constant_folding);
         NIR_PASS(progress, nir, nir_opt_cse);
      } while (progress);
   }
   NIR_PASS_V(nir, nir_lower_memcpy);

   NIR_PASS_V(nir, nir_lower_explicit_io,
              nir_var_mem_shared | nir_var_function_temp | nir_var_shader_temp | nir_var_uniform,
              nir_address_format_32bit_offset_as_64bit);

   NIR_PASS_V(nir, nir_lower_system_values);

   /* Hopefully we can drop this once lower_vars_to_ssa has improved to not
    * lower everything to scratch.
    */
   if (llvm17_wa)
      cleanup_llvm17_scratch(nir);

   /* Lower again, this time after dead-variables to get more compact variable
    * layouts.
    */
   nir->global_mem_size = 0;
   nir->scratch_size = 0;
   nir->info.shared_size = 0;
   NIR_PASS_V(nir, nir_lower_vars_to_explicit_types,
              nir_var_mem_shared | nir_var_mem_global | nir_var_mem_constant,
              glsl_get_cl_type_size_align);
   if (nir->constant_data_size > 0) {
      assert(nir->constant_data == NULL);
      nir->constant_data = rzalloc_size(nir, nir->constant_data_size);
      nir_gather_explicit_io_initializers(nir, nir->constant_data,
                                          nir->constant_data_size,
                                          nir_var_mem_constant);
   }

   NIR_PASS_V(nir, nir_lower_explicit_io, nir_var_mem_constant,
              nir_address_format_64bit_global);

   NIR_PASS_V(nir, nir_lower_explicit_io, nir_var_uniform,
              nir_address_format_32bit_offset_as_64bit);

   NIR_PASS_V(nir, nir_lower_explicit_io,
              nir_var_shader_temp | nir_var_function_temp |
              nir_var_mem_shared | nir_var_mem_global,
              nir_address_format_62bit_generic);

   if (INTEL_DEBUG(DEBUG_CS)) {
      /* Re-index SSA defs so we print more sensible numbers. */
      nir_foreach_function_impl(impl, nir) {
         nir_index_ssa_defs(impl);
      }

      fprintf(stderr, "NIR (before I/O lowering) for kernel\n");
      nir_print_shader(nir, stderr);
   }

   return nir;
}
