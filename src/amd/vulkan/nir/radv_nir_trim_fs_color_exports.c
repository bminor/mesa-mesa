/*
 * Copyright © 2025 Valve Corporation
 *
 * SPDX-License-Identifier: MIT
 */

#include "nir/nir.h"
#include "nir/nir_builder.h"
#include "radv_constants.h"
#include "radv_nir.h"

static bool
trim_fs_color_exports(nir_builder *b, nir_intrinsic_instr *intrin, void *state)
{
   const uint32_t colors_needed = *(uint32_t *)state;

   if (intrin->intrinsic != nir_intrinsic_store_output)
      return false;

   nir_io_semantics io_sem = nir_intrinsic_io_semantics(intrin);

   int index = mesa_frag_result_get_color_index(io_sem.location);

   if (index < 0)
      return false;

   const unsigned needed = (colors_needed >> (index * 4) & 0xf) >> nir_intrinsic_component(intrin);

   const unsigned write_mask = nir_intrinsic_write_mask(intrin);

   const unsigned new_write_mask = write_mask & needed;

   if (new_write_mask == write_mask)
      return false;

   if (!new_write_mask)
      nir_instr_remove(&intrin->instr);
   else
      nir_intrinsic_set_write_mask(intrin, new_write_mask);

   return true;
}

bool
radv_nir_trim_fs_color_exports(nir_shader *shader, uint32_t colors_needed)
{
   return nir_shader_intrinsics_pass(shader, trim_fs_color_exports, nir_metadata_control_flow, &colors_needed);
}
