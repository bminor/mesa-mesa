# Copyright © 2017 Intel Corporation
# SPDX-License-Identifier: MIT

main_dispatch_h = custom_target(
  'dispatch.h',
  input : ['../../mapi/glapi/gen/gl_table.py', '../../mapi/glapi/gen/gl_and_es_API.xml'],
  output : 'dispatch.h',
  command : [prog_python, '@INPUT0@', '-f', '@INPUT1@', '-m', 'dispatch'],
  depend_files : glapi_xml_py_deps,
  capture : true,
)

idep_dispatch_h = declare_dependency(sources : [main_dispatch_h])

main_marshal_generated_h = custom_target(
  'marshal_generated.h',
  input : ['../../mapi/glapi/gen/marshal_generated_h.py', '../../mapi/glapi/gen/gl_and_es_API.xml'],
  output : 'marshal_generated.h',
  command : [prog_python, '@INPUT0@', '@INPUT1@', sizeof_pointer],
  depend_files : glapi_xml_py_deps,
  capture : true,
)

if _shader_replacement != ''
  # shader replacement
  shader_replacement_h = custom_target(
    'shader_replacement.h',
    input: [files(_shader_replacement + '/process_shaders.py')],
    output: 'shader_replacement.h',
    command: [prog_python, '@INPUT0@', _shader_replacement, '@OUTPUT@'],
    build_always: true,
  )
endif
