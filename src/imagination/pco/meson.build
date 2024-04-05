# Copyright © 2024 Imagination Technologies Ltd.
# SPDX-License-Identifier: MIT

inc_powervr_compiler = include_directories(['.'])

libpowervr_compiler_files = files(
  'pco.c',
  'pco_binary.c',
  'pco_ir.c',
  'pco_nir.c',
  'pco_trans_nir.c',
)

libpowervr_compiler = static_library(
  'powervr_compiler',
  [libpowervr_compiler_files],
  include_directories : [
    inc_imagination,
    inc_powervr_compiler,
    inc_include,
    inc_compiler,
    inc_src,
  ],
  c_args : [imagination_c_args, no_override_init_args],
  dependencies : [idep_mesautil, idep_nir],
  gnu_symbol_visibility : 'hidden',
  install : false,
)
