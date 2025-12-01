# Copyright © 2025 Collabora, Ltd.
# SPDX-License-Identifier: MIT

libpanfrost_util_files = files(
  'pan_ir.c',
  'pan_ir.h',
  'pan_nir_collect_varyings.c',
  'pan_nir_lower_frag_coord_zw.c',
  'pan_nir_lower_framebuffer.c',
  'pan_nir_lower_helper_invocation.c',
  'pan_nir_lower_image_index.c',
  'pan_nir_lower_image_ms.c',
  'pan_nir_lower_noperspective.c',
  'pan_nir_lower_sample_position.c',
  'pan_nir_lower_store_component.c',
  'pan_nir_lower_vertex_id.c',
  'pan_nir_lower_writeout.c',
  'pan_nir_lower_xfb.c',
)

libpanfrost_util = static_library(
  'panfrost_util',
  [libpanfrost_util_files],
  include_directories : [inc_include, inc_src],
  dependencies: [idep_nir, idep_mesautil],
  c_args : [no_override_init_args],
  gnu_symbol_visibility : 'hidden',
  build_by_default : false,
)

subdir('bifrost')
subdir('midgard')
