# Copyright © 2025 Imagination Technologies Ltd.
# SPDX-License-Identifier: MIT

pco_usclib_shader_files = files(
  'common.cl',
)

# We need to set -fmacro-prefix-map properly for reproducability.
fs = import('fs')
relative_dir = fs.relative_to(meson.global_source_root(), meson.global_build_root()) + '/'

pco_usclib_spv = custom_target(
  'pco_usclib.spv',
  input : pco_usclib_shader_files,
  output : 'pco_usclib.spv',
  command : [
    prog_mesa_clc,
    '-o', '@OUTPUT@', '--depfile', '@DEPFILE@',
    pco_usclib_shader_files, '--',
    '-cl-std=cl2.0', '-D__OPENCL_VERSION__=200',
    '-I' + join_paths(meson.project_source_root(), 'include'),
    '-I' + join_paths(meson.project_source_root(), 'src/compiler/libcl'),
    '-I' + join_paths(meson.current_source_dir(), '.'),
    '-I' + join_paths(meson.current_source_dir(), '../../../'),
    '-fmacro-prefix-map=@0@='.format(relative_dir),
    ],
  env: ['MESA_SHADER_CACHE_DISABLE=true'],
  depfile : 'pco_usclib.h.d',
)

pco_usclib = custom_target(
  'pco_usclib.h',
  input : pco_usclib_spv,
  output : ['pco_usclib.cpp', 'pco_usclib.h'],
  command : [prog_vtn_bindgen2, pco_usclib_spv, '@OUTPUT0@', '@OUTPUT1@'],
)

idep_pco_usclib = declare_dependency(
  sources : [pco_usclib],
  include_directories : include_directories('.'),
)
