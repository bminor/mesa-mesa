/* Copyright 2025 Advanced Micro Devices, Inc.
 * SPDX-License-Identifier: MIT
 */

#include "si_shader_internal.h"
#include "nir_builder.h"

bool si_nir_lower_color_inputs_to_sysvals(nir_shader *nir)
{
   nir_function_impl *impl = nir_shader_get_entrypoint(nir);
   bool progress = false;

   nir_builder b = nir_builder_create(impl);

   nir_foreach_block(block, impl) {
      nir_foreach_instr_safe(instr, block) {
         if (instr->type != nir_instr_type_intrinsic)
            continue;

         nir_intrinsic_instr *intrin = nir_instr_as_intrinsic(instr);

         if (intrin->intrinsic != nir_intrinsic_load_input &&
             intrin->intrinsic != nir_intrinsic_load_interpolated_input)
            continue;

         nir_io_semantics sem = nir_intrinsic_io_semantics(intrin);

         if (sem.location != VARYING_SLOT_COL0 &&
             sem.location != VARYING_SLOT_COL1)
            continue;

         b.cursor = nir_before_instr(instr);
         nir_def *load = NULL;

         if (sem.location == VARYING_SLOT_COL0) {
            load = nir_load_color0_amd(&b, 32);
         } else {
            assert(sem.location == VARYING_SLOT_COL1);
            load = nir_load_color1_amd(&b, 32);
         }

         if (intrin->num_components != 4) {
            unsigned start = nir_intrinsic_component(intrin);
            unsigned count = intrin->num_components;
            load = nir_channels(&b, load, BITFIELD_RANGE(start, count));
         }

         nir_def_replace(&intrin->def, load);
         progress = true;
      }
   }

   return nir_progress(progress, impl, nir_metadata_control_flow);
}
