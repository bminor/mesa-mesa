# Copyright 2024 Valve Corporation
# SPDX-License-Identifier: MIT

libagx_shader_files = files(
  'libagx.h',
  'compression.cl',
  'draws.cl',
  'geometry.cl',
  'geometry.h',
  'query.cl',
  'query.h',
  'tessellation.cl',
  'tessellator.cl',
  'tessellator.h',
  'texture.cl',
  'helper.cl',
  'libagx_dgc.h',
)

prepended_input_args = []
foreach input_arg : libagx_shader_files
  prepended_input_args += ['--in', input_arg]
endforeach
libagx_spv = custom_target(
  'libagx.spv',
  input : libagx_shader_files,
  output : 'libagx.spv',
  command : [
    prog_mesa_clc,
    prepended_input_args, '-o', '@OUTPUT@', '--',
    '-cl-std=cl2.0', '-D__OPENCL_VERSION__=200',
    '-I' + join_paths(meson.current_source_dir(), '.'),
    '-I' + join_paths(meson.current_source_dir(), '../../'),
    '-I' + join_paths(meson.current_source_dir(), 'shaders'),
    '-I' + join_paths(meson.current_build_dir(), '../genxml'),
    ],
  env: ['MESA_SHADER_CACHE_DISABLE=true'],
  depends : [prog_mesa_clc, agx_pack],
)

libagx_h = custom_target(
  'libagx.h',
  input : libagx_spv,
  output : 'libagx.h',
  command : [prog_vtn_bindgen, libagx_spv, '@OUTPUT@'],
  depends : [prog_vtn_bindgen],
)

idep_libagx = declare_dependency(
  sources : [libagx_h],
  include_directories : include_directories('.'),
)
