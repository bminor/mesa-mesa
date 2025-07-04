# Copyright © 2024 Google
# SPDX-License-Identifier: MIT

project(
  'libc',
  'rust',
  version : '0.2.168',
  license : 'MIT OR Apache-2.0',
)

libc_args = [
  '--cfg', 'feature="default"',
  '--cfg', 'feature="extra_traits"',
  '--cfg', 'feature="std"',
  '--cfg', 'freebsd11',
  '--cfg', 'freebsd11',
  '--cfg', 'freebsd11',
  '--cfg', 'libc_align',
  '--cfg', 'libc_cfg_target_vendor',
  '--cfg', 'libc_const_extern_fn',
  '--cfg', 'libc_const_size_of',
  '--cfg', 'libc_core_cvoid',
  '--cfg', 'libc_int128',
  '--cfg', 'libc_long_array',
  '--cfg', 'libc_non_exhaustive',
  '--cfg', 'libc_packedN',
  '--cfg', 'libc_priv_mod_use',
  '--cfg', 'libc_ptr_addr_of',
  '--cfg', 'libc_underscore_const_names',
  '--cfg', 'libc_union',
]

lib = static_library(
  'libc',
  'src/lib.rs',
  rust_abi : 'rust',
  override_options : ['rust_std=2021', 'build.rust_std=2021'],
  native : true,
  rust_args: libc_args,
)

dep_libc = declare_dependency(
  link_with : [lib]
)
