/*
 * Copyright © 2025 Valve Corporation
 *
 * SPDX-License-Identifier: MIT
 */

#include "nir/nir.h"
#include "nir/nir_builder.h"

#include "radv_nir.h"

static bool
set_can_speculate(nir_builder *b, nir_intrinsic_instr *intr, void *data)
{
   switch (intr->intrinsic) {
   case nir_intrinsic_load_ubo:
   case nir_intrinsic_load_ssbo:
      if (!intr->src[0].ssa->parent_instr->pass_flags)
         return false;
      break;
   case nir_intrinsic_load_constant:
      break;
   default:
      return false;
   }

   unsigned access = nir_intrinsic_access(intr);
   if (!(access & ACCESS_SMEM_AMD))
      return false;

   nir_intrinsic_set_access(intr, access | ACCESS_CAN_SPECULATE);
   return true;
}

/* Detect descriptors that are used in top level control flow, and mark all smem users as CAN_SPECULATE. */
bool
radv_nir_opt_access_can_speculate(nir_shader *shader)
{
   bool had_terminate = false;
   nir_foreach_function_impl (impl, shader) {
      nir_foreach_block (block, impl) {
         bool top_level = block->cf_node.parent->type == nir_cf_node_function;
         nir_foreach_instr (instr, block) {
            instr->pass_flags = 0;
            if (had_terminate)
               continue;

            if (instr->type != nir_instr_type_intrinsic)
               continue;
            nir_intrinsic_instr *intr = nir_instr_as_intrinsic(instr);
            switch (intr->intrinsic) {
            case nir_intrinsic_terminate:
            case nir_intrinsic_terminate_if:
               had_terminate = true;
               break;
            case nir_intrinsic_load_ssbo:
            case nir_intrinsic_load_ubo:
               if (top_level)
                  intr->src[0].ssa->parent_instr->pass_flags = 1;
               break;
            default:
               break;
            }
         }
      }
   }

   return nir_shader_intrinsics_pass(shader, set_can_speculate, nir_metadata_all, NULL);
}
