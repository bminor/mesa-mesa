# Copyright © 2025 Valve Corporation
# SPDX-License-Identifier: MIT

vklayer_files = files(
  'anti_lag_layer.c',
  'anti_lag_layer_interface.c',
)

shared_library(
  'VkLayer_MESA_anti_lag',
  vklayer_files,
  c_args : [no_override_init_args],
  gnu_symbol_visibility : 'hidden',
  dependencies : [
    idep_vulkan_util, idep_mesautil,
  ],
  include_directories : [inc_include, inc_util, inc_src],
  link_args : cc.get_supported_link_arguments(['-Wl,-Bsymbolic-functions', '-Wl,-z,relro']),
  install : true
)

install_data(
  files('VkLayer_MESA_anti_lag.json'),
  install_dir : join_paths(get_option('datadir'), 'vulkan', 'implicit_layer.d'),
  install_tag : 'runtime',
)
