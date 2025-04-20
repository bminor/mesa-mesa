/*
 * Copyright © 2023 Igalia S.L.
 * SPDX-License-Identifier: MIT
 */

#include "etnaviv_nir.h"

static bool
lower_txs(nir_builder *b, nir_tex_instr *tex, UNUSED void *data)
{
   if (tex->op != nir_texop_txs)
      return false;

   b->cursor = nir_instr_remove(&tex->instr);

   nir_def *idx = nir_imm_int(b, tex->texture_index);
   nir_def *sizes = nir_load_texture_size_etna(b, 32, idx);

   nir_def_rewrite_uses(&tex->def, sizes);

   return true;
}

static bool
legalize_txf_lod(nir_builder *b, nir_tex_instr *tex, UNUSED void *data)
{
   if (tex->op != nir_texop_txf)
      return false;

   b->cursor = nir_before_instr(&tex->instr);

   int lod_index = nir_tex_instr_src_index(tex, nir_tex_src_lod);
   assert(lod_index >= 0);
   nir_def *lod = tex->src[lod_index].src.ssa;

   nir_src_rewrite(&tex->src[lod_index].src, nir_i2f32(b, lod));

   return true;
}

static bool
legalize_txd_derivatives(nir_builder *b, nir_tex_instr *tex, UNUSED void *data)
{
   if (tex->op != nir_texop_txd)
      return false;

   b->cursor = nir_before_instr(&tex->instr);

   int coord_index = nir_tex_instr_src_index(tex, nir_tex_src_coord);
   int ddx_index = nir_tex_instr_src_index(tex, nir_tex_src_ddx);
   int ddy_index = nir_tex_instr_src_index(tex, nir_tex_src_ddy);

   assert(coord_index >= 0);
   assert(ddx_index >= 0);
   assert(ddy_index >= 0);

   nir_def *coord = tex->src[coord_index].src.ssa;
   nir_def *ddx = tex->src[ddx_index].src.ssa;
   nir_def *ddy = tex->src[ddy_index].src.ssa;

   coord = nir_trim_vector(b, coord, ddx->num_components);

   nir_src_rewrite(&tex->src[ddx_index].src, nir_fadd(b, coord, ddx));
   nir_src_rewrite(&tex->src[ddy_index].src, nir_fadd(b, coord, ddy));

   return true;
}

bool
etna_nir_lower_texture(nir_shader *s, struct etna_shader_key *key)
{
   bool progress = false;

   nir_lower_tex_options lower_tex_options = {
      .lower_txp = ~0u,
      .lower_txs_lod = true,
      .lower_invalid_implicit_lod = true,
   };

   NIR_PASS(progress, s, nir_lower_tex, &lower_tex_options);

   if (key->has_sample_tex_compare)
      NIR_PASS(progress, s, nir_lower_tex_shadow, key->num_texture_states,
                                                  key->tex_compare_func,
                                                  key->tex_swizzle,
                                                  true);

   NIR_PASS(progress, s, nir_shader_tex_pass, lower_txs,
         nir_metadata_control_flow, NULL);

   NIR_PASS(progress, s, nir_shader_tex_pass, legalize_txf_lod,
      nir_metadata_control_flow, NULL);

   NIR_PASS(progress, s, nir_shader_tex_pass, legalize_txd_derivatives,
      nir_metadata_control_flow, NULL);

   return progress;
}
