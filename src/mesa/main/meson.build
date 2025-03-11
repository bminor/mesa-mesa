# Copyright © 2017 Intel Corporation
# SPDX-License-Identifier: MIT

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
