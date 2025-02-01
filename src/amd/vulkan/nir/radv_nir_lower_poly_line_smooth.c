/*
 * Copyright © 2023 Valve Corporation
 *
 * SPDX-License-Identifier: MIT
 */

#include "nir.h"
#include "nir_builder.h"
#include "radv_nir.h"
#include "radv_pipeline_graphics.h"

void
radv_nir_lower_poly_line_smooth(nir_shader *nir, const struct radv_graphics_state_key *gfx_state)
{
   bool progress = false;

   if (!gfx_state->dynamic_line_rast_mode)
      return false;

   NIR_PASS(progress, nir, nir_lower_poly_line_smooth, RADV_NUM_SMOOTH_AA_SAMPLES);
   if (progress)
      nir_shader_gather_info(nir, nir_shader_get_entrypoint(nir));
}
