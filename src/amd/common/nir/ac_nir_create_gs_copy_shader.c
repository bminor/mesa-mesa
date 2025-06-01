/*
 * Copyright © 2021 Valve Corporation
 *
 * SPDX-License-Identifier: MIT
 */

#include "ac_nir.h"
#include "ac_nir_helpers.h"

#include "nir_builder.h"
#include "nir_xfb_info.h"

nir_shader *
ac_nir_create_gs_copy_shader(const nir_shader *gs_nir, ac_nir_lower_legacy_gs_options *options,
                             ac_nir_prerast_out *out)
{
   nir_builder b = nir_builder_init_simple_shader(
      MESA_SHADER_VERTEX, gs_nir->options, "gs_copy");

   b.shader->info.outputs_written = gs_nir->info.outputs_written;
   b.shader->info.outputs_written_16bit = gs_nir->info.outputs_written_16bit;

   nir_def *gsvs_ring = nir_load_ring_gsvs_amd(&b);

   nir_xfb_info *info = ac_nir_get_sorted_xfb_info(gs_nir);
   nir_def *stream_id = NULL;
   if (!options->disable_streamout && info)
      stream_id = nir_ubfe_imm(&b, nir_load_streamout_config_amd(&b), 24, 2);

   nir_def *vtx_offset = nir_imul_imm(&b, nir_load_vertex_id_zero_base(&b), 4);
   nir_def *zero = nir_imm_zero(&b, 1, 32);

   for (unsigned stream = 0; stream < 4; stream++) {
      if (stream > 0 && (!stream_id || !(info->streams_written & BITFIELD_BIT(stream))))
         continue;

      if (stream_id)
         nir_push_if(&b, nir_ieq_imm(&b, stream_id, stream));

      unsigned offset = 0;

      u_foreach_bit64 (i, gs_nir->info.outputs_written) {
         u_foreach_bit (j, out->infos[i].components_mask) {
            if (((out->infos[i].stream >> (j * 2)) & 0x3) != stream)
               continue;

            if (ac_nir_is_const_output(out, i, j)) {
               out->outputs[i][j] = ac_nir_get_const_output(&b, 32, out, i, j);
               continue;
            }

            unsigned base = offset * gs_nir->info.gs.vertices_out * 16;
            out->outputs[i][j] =
               nir_load_buffer_amd(&b, 1, 32, gsvs_ring, vtx_offset, zero, zero,
                                   .base = base,
                                   .access = ACCESS_COHERENT | ACCESS_NON_TEMPORAL);
            offset += 4;
         }
      }

      u_foreach_bit (i, gs_nir->info.outputs_written_16bit) {
         unsigned mask = out->infos_16bit_lo[i].components_mask |
                         out->infos_16bit_hi[i].components_mask;

         u_foreach_bit (j, mask) {
            bool has_lo_16bit = ((out->infos_16bit_lo[i].stream >> (j * 2)) & 0x3) == stream;
            bool has_hi_16bit = ((out->infos_16bit_hi[i].stream >> (j * 2)) & 0x3) == stream;

            if (!has_lo_16bit && !has_hi_16bit)
               continue;

            nir_def *load_val;

            if (ac_nir_is_const_output(out, VARYING_SLOT_VAR0_16BIT + i, j)) {
               load_val = ac_nir_get_const_output(&b, 32, out, i, j);
            } else {
               unsigned base = offset * gs_nir->info.gs.vertices_out * 16;
               load_val = nir_load_buffer_amd(&b, 1, 32, gsvs_ring, vtx_offset, zero, zero,
                                              .base = base,
                                              .access = ACCESS_COHERENT | ACCESS_NON_TEMPORAL);
               offset += 4;
            }

            if (has_lo_16bit)
               out->outputs_16bit_lo[i][j] = nir_unpack_32_2x16_split_x(&b, load_val);

            if (has_hi_16bit)
               out->outputs_16bit_hi[i][j] = nir_unpack_32_2x16_split_y(&b, load_val);
         }
      }

      if (stream_id)
         ac_nir_emit_legacy_streamout(&b, stream, info, out);

      /* This should be after streamout and before exports. */
      ac_nir_clamp_vertex_color_outputs(&b, out);

      if (stream == 0) {
         ac_nir_export_position(&b, options->gfx_level, options->export_clipdist_mask, false,
                                options->write_pos_to_clipvertex, options->pack_clip_cull_distances,
                                !options->has_param_exports, options->force_vrs,
                                b.shader->info.outputs_written | VARYING_BIT_POS,
                                out, NULL);

         if (options->has_param_exports) {
            ac_nir_export_parameters(&b, options->param_offsets,
                                     b.shader->info.outputs_written,
                                     b.shader->info.outputs_written_16bit,
                                     out);
         }
      }

      if (stream_id)
         nir_push_else(&b, NULL);
   }

   b.shader->info.clip_distance_array_size = gs_nir->info.clip_distance_array_size;
   b.shader->info.cull_distance_array_size = gs_nir->info.cull_distance_array_size;

   return b.shader;
}
