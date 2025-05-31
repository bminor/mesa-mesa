/*
 * Copyright 2017 Advanced Micro Devices, Inc.
 *
 * SPDX-License-Identifier: MIT
 */

#include "si_pipe.h"
#include "si_query.h"
#include "si_shader_internal.h"

static bool gfx10_ngg_writes_user_edgeflags(struct si_shader *shader)
{
   return gfx10_has_variable_edgeflags(shader) &&
          shader->selector->info.writes_edgeflag;
}

bool gfx10_ngg_export_prim_early(struct si_shader *shader)
{
   struct si_shader_selector *sel = shader->selector;

   assert(shader->key.ge.as_ngg && !shader->key.ge.as_es);

   return sel->stage != MESA_SHADER_GEOMETRY &&
          !gfx10_ngg_writes_user_edgeflags(shader) &&
          sel->screen->info.gfx_level < GFX11;
}

/**
 * Determine subgroup information like maximum number of vertices and prims.
 *
 * This happens before the shader is uploaded, since LDS relocations during
 * upload depend on the subgroup size.
 */
bool gfx10_ngg_calculate_subgroup_info(struct si_shader *shader)
{
   const struct si_shader_selector *gs_sel = shader->selector;
   const struct si_shader_selector *es_sel =
      shader->previous_stage_sel ? shader->previous_stage_sel : gs_sel;
   const gl_shader_stage gs_stage = gs_sel->stage;
   const unsigned input_prim = si_get_input_prim(gs_sel, &shader->key, false);
   unsigned gs_vertices_out = gs_stage == MESA_SHADER_GEOMETRY ? gs_sel->info.base.gs.vertices_out : 0;
   unsigned gs_invocations = gs_stage == MESA_SHADER_GEOMETRY ? gs_sel->info.base.gs.invocations : 0;

   return ac_ngg_compute_subgroup_info(gs_sel->screen->info.gfx_level, es_sel->stage,
                                       gs_sel->stage == MESA_SHADER_GEOMETRY,
                                       input_prim, gs_vertices_out, gs_invocations,
                                       si_get_max_workgroup_size(shader), shader->wave_size,
                                       es_sel->info.esgs_vertex_stride, shader->info.ngg_lds_vertex_size,
                                       shader->info.ngg_lds_scratch_size, gs_sel->tess_turns_off_ngg,
                                       &shader->ngg.info);
}
