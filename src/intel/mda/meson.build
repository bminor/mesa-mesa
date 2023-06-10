# Copyright 2024 Intel Corporation
# SPDX-License-Identifier: MIT

_libmda = static_library(
  'mda',
  [
    'debug_archiver.h',
    'debug_archiver.c',
    'slice.h',
    'slice.c',
    'tar.h',
    'tar.c',
  ],
  include_directories: [inc_src],
  c_args: [c_msvc_compat_args],
  gnu_symbol_visibility: 'hidden',
)

idep_mda = declare_dependency(
  link_with: _libmda,
)

if with_tests
  test('mda_tests',
    executable(
      'mda_tests',
      [
        'tar_test.cpp',
        'slice_test.cpp',
      ],
      dependencies: [
        idep_gtest,
        idep_mda,
        idep_mesautil,
      ],
      c_args: [c_msvc_compat_args],
    ),
    suite: 'mda',
    protocol: 'gtest',
  )
endif
