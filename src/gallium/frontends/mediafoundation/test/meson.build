mediafoundation_test_st = static_library(
    'mediafoundation_test_st',
    files(
        'hmft_test_helpers.cpp',
        'hmft_codecapi_test.cpp',
        'hmft_simple_encode_test.cpp',
        'stub.cpp',
    ),
    cpp_args : mf_cpp_args,
    override_options: ['cpp_std=c++20'],
    include_directories: [inc_include, inc_src, inc_gallium, inc_gallium_aux],
    dependencies: [idep_gtest, dep_dxheaders],
)