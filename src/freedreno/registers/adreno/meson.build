# Copyright © 2019 Google, Inc
# SPDX-License-Identifier: MIT

xml_reg_files = [
  'a2xx.xml',
  'a3xx.xml',
  'a4xx.xml',
  'a5xx.xml',
  'a6xx.xml',
  'a6xx_enums.xml',
  'a6xx_descriptors.xml',
  'a6xx_perfcntrs.xml',
  'a7xx_enums.xml',
  'a7xx_perfcntrs.xml',
]

xml_files = xml_reg_files
xml_files += [
  'a6xx_gmu.xml',
  'ocmem.xml',
  'adreno_control_regs.xml',
  'adreno_pipe_regs.xml',
  'adreno_common.xml',
  'adreno_pm4.xml',
]

freedreno_py_header_files = []

foreach f : xml_files
  _name = f + '.h'
  freedreno_xml_header_files += custom_target(
    _name,
    input: [gen_header_py, f, freedreno_schema, freedreno_copyright],
    output: _name,
    command: [prog_python, '@INPUT0@', '--rnn', rnn_src_path, '--xml', '@INPUT1@', 'c-defines'],
    capture: true,
  )
  _gzname = f + '.gz'
  custom_target(
    _gzname,
    input: f,
    output: _gzname,
    command: [prog_gzip, '-kc', '@INPUT@'],
    capture: true,
    install_dir: rnn_install_path + '/adreno',
    install: install_fd_decode_tools,
    build_by_default: install_fd_decode_tools,
  )
endforeach

freedreno_xml_header_files += custom_target(
    'a6xx-pack.xml.h',
    input: [gen_header_py, 'a6xx.xml', freedreno_schema, freedreno_copyright],
    output: 'a6xx-pack.xml.h',
    command: [prog_python, '@INPUT0@', '--rnn', rnn_src_path, '--xml', '@INPUT1@', 'c-pack-structs'],
    capture: true,
)

freedreno_xml_header_files += custom_target(
    'adreno-pm4-pack.xml.h',
    input: [gen_header_py, 'adreno_pm4.xml', freedreno_schema, freedreno_copyright],
    output: 'adreno-pm4-pack.xml.h',
    command: [prog_python, '@INPUT0@', '--rnn', rnn_src_path, '--xml', '@INPUT1@', 'c-pack-structs'],
    capture: true,
)

foreach f : xml_reg_files
  _pyname = f.split('.')[0] + '.py'
  freedreno_py_header_files += custom_target(
    _pyname,
    input: [gen_header_py, f, freedreno_schema, freedreno_copyright],
    output: _pyname,
    command: [prog_python, '@INPUT0@', '--rnn', rnn_src_path, '--xml', '@INPUT1@', 'py-defines'],
    capture: true,
  )
endforeach
