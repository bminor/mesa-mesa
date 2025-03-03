# Copyright © 2017 Intel Corporation
# SPDX-License-Identifier: MIT

subdir('gen')

inc_glapi = include_directories('.')

bridge_glapi_files = files(
  '../entry.c',
  '../entry.h',
  '../entry_x86-64_tls.h',
  '../entry_x86_tls.h',
  '../entry_ppc64le_tls.h',
  '../mapi_tmp.h',
)
bridge_glapi_files += glapi_mapi_tmp_h

if with_dri and ['apple', 'windows'].contains(with_dri_platform)
  bridge_glapi_files += [glapi_gentable_c, main_dispatch_h]
endif

bridge_glapi_args = [
  '-DMAPI_MODE_BRIDGE',
  '-DMAPI_ABI_HEADER="@0@"'.format(glapi_mapi_tmp_h.full_path()),
  gcc_lto_quirk,
]
if with_platform_windows
  bridge_glapi_args += ['-D_GDI32_']
endif

libglapi_bridge = static_library(
  'glapi_bridge',
  bridge_glapi_files,
  include_directories : [inc_mesa, inc_include, inc_src, inc_mapi, inc_gallium],
  c_args : [c_msvc_compat_args, bridge_glapi_args],
  dependencies : [dep_thread, idep_mesautil],
  build_by_default : false,
)
