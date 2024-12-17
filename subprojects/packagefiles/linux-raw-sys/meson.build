# Copyright © 2025 Google
# SPDX-License-Identifier: MIT

project(
  'linux-raw-sys',
  'rust',
  version : '0.4.14',
  license : 'MIT OR Apache-2.0 OR Apache-2.0 WITH LLVM-exception',
)

rust_args = [
  '--cfg', 'feature="elf"',
  '--cfg', 'feature="errno"',
  '--cfg', 'feature="general"',
  '--cfg', 'feature="if_ether"',
  '--cfg', 'feature="ioctl"',
  '--cfg', 'feature="net"',
  '--cfg', 'feature="netlink"',
  '--cfg', 'feature="no_std"',
  '--cfg', 'feature="xdp"',
]

lib = static_library(
  'linux_raw_sys',
  'src/lib.rs',
  rust_args : rust_args,
  override_options : ['rust_std=2021', 'build.rust_std=2021'],
  rust_abi : 'rust',
  native : true,
)

dep_linux_raw_sys = declare_dependency(
  link_with : [lib]
)
