# Copyright © Microsoft Corporation
#
# Permission is hereby granted, free of charge, to any person obtaining a
# copy of this software and associated documentation files (the "Software"),
# to deal in the Software without restriction, including without limitation
# the rights to use, copy, modify, merge, publish, distribute, sublicense,
# and/or sell copies of the Software, and to permit persons to whom the
# Software is furnished to do so, subject to the following conditions:
#
# The above copyright notice and this permission notice (including the next
# paragraph) shall be included in all copies or substantial portions of the
# Software.
#
# THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
# IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
# FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
# THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
# LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
# FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
# IN THE SOFTWARE.

fs = import('fs')

mf_cpp_args = cc.get_supported_arguments([
    '/we4267',
    '/we4996',
    '/we4146',
    '/we4244',
    '/we4389',
    '/we4838',
    '/we4302',
    '/we4018',
    '/we4056',
    '/we4305',
    '/we4351',
    '/we4756',
    '/we4800',
    '/we4291',
    '/we4624',
    '/we4309',
    '/we5105',
    '/we4020',
    '/we4024',
    '/we4189',
])
if get_option('mediafoundation-store-dll')
  mf_cpp_args += '-DBUILD_FOR_MSDK' # Can use #if BUILD_FOR_MSDK in .cpp code
endif

mediafoundation_files = files(
  'codecapi.cpp',
  'encode.cpp',
  'encode_av1.cpp',
  'encode_h264.cpp',
  'encode_hevc.cpp',
  'encoder_capabilities.cpp',
  'hmft_entrypoints.cpp',
  'mfbufferhelp.cpp',
  'mfd3dmanager.cpp',
  'mfmediaeventgenerator.cpp',
  'mfpipeinterop.cpp',
  'mfrealtimeclientex.cpp',
  'mfshutdown.cpp',
  'mftransform.cpp',
  'dpb_buffer_manager.cpp',
  'reference_frames_tracker_av1.cpp',
  'reference_frames_tracker_h264.cpp',
  'reference_frames_tracker_hevc.cpp',
  'videobufferlock.cpp',
  'wpptrace.cpp',
)

mediafoundation_wpp_args = ''
foreach f : mediafoundation_files
  mediafoundation_wpp_args += fs.relative_to(f, 'a:\\').replace('/', '\\') + '\r\n'
endforeach

mediafoundation_wpp_rsp = configure_file(
  input : meson.current_source_dir() + '/wppconfig/wpp_args.rsp.template',
  output : 'mediafoundation_wpp_args.rsp',
  configuration : {'WPP_ARGS' : mediafoundation_wpp_args}
)

mediafoundation_wpp_preprocess = custom_target(
  'mediafoundation_wpp_preprocess',
  capture : true,
  build_always: true,
  input : mediafoundation_files,
  output : 'mediafoundation_wpp_preprocess.dummy.h',
  command : ['tracewpp', '-cfgdir:@CURRENT_SOURCE_DIR@/wppconfig/rev1', '-scan:@CURRENT_SOURCE_DIR@/wpptrace.h', '-odir:@OUTDIR@', '@' + fs.relative_to(mediafoundation_wpp_rsp, 'a:\\')]
)

gallium_mf_name = get_option('mediafoundation-windows-dll-name')

mediafoundation_st = static_library(
  'mediafoundation_st',
  mediafoundation_files,
  mediafoundation_wpp_preprocess,
  gnu_symbol_visibility : 'hidden',
  cpp_args : mf_cpp_args,
  override_options: ['cpp_std=c++20'],
  include_directories : [inc_include, inc_src, inc_mesa, inc_gallium, inc_gallium_aux],
  dependencies : [idep_mesautil, dep_dxheaders],
)

if with_gallium_mediafoundation_test
  subdir('test')
endif
