# Copyright © 2017 Intel Corporation
# SPDX-License-Identifier: MIT

files_shared_glapi = files(
  '../entry.c',
  '../u_current.c',
  'glapi.c',
  'stub.c',
  'stub.h',
  'table.c',
  'table.h',
)

shared_glapi_mapi_tmp_h = custom_target(
  'shared_glapi_mapi_tmp.h',
  input : ['../mapi_abi.py', '../glapi/gen/gl_and_es_API.xml'],
  output : 'glapi_mapi_tmp.h',
  command : [prog_python, '@INPUT0@', '--printer', 'shared-glapi', '@INPUT1@'],
  depend_files : glapi_xml_py_deps,
  capture : true,
)

libglapi = static_library(
  'glapi',
  [files_shared_glapi, shared_glapi_mapi_tmp_h],
  c_args : [
    c_msvc_compat_args,
    '-DMAPI_MODE_GLAPI',
    '-DMAPI_ABI_HEADER="@0@"'.format(shared_glapi_mapi_tmp_h.full_path()),
    gcc_lto_quirk,
  ],
  gnu_symbol_visibility : 'hidden',
  link_args : [ld_args_gc_sections],
  include_directories : [inc_src, inc_include, inc_mapi],
  dependencies : [dep_thread, idep_mesautil],
  install : false,
)
libglapi_build_dir = meson.current_build_dir()
