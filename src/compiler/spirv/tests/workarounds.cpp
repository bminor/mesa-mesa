/*
 * Copyright © 2025 Valve Corporation
 *
 * SPDX-License-Identifier: MIT
 */
#include "helpers.h"

class Workarounds : public spirv_test {};

TEST_F(Workarounds, force_ssbo_non_uniform)
{
   /*
               OpCapability Shader
          %1 = OpExtInstImport "GLSL.std.450"
               OpMemoryModel Logical GLSL450
               OpEntryPoint GLCompute %main "main"
               OpExecutionMode %main LocalSize 1 1 1
               OpSource GLSL 460
               OpSourceExtension "GL_EXT_buffer_reference"
               OpName %main "main"
               OpName %data "data"
               OpMemberName %data 0 "v"
               OpName %ssbo "ssbo"
               OpDecorate %_runtimearr_float ArrayStride 4
               OpDecorate %data BufferBlock
               OpMemberDecorate %data 0 Offset 0
               OpDecorate %ssbo Binding 0
               OpDecorate %ssbo DescriptorSet 0
       %void = OpTypeVoid
          %3 = OpTypeFunction %void
      %float = OpTypeFloat 32
%_runtimearr_float = OpTypeRuntimeArray %float
       %data = OpTypeStruct %_runtimearr_float
%_ptr_Uniform_data = OpTypePointer Uniform %data
       %ssbo = OpVariable %_ptr_Uniform_data Uniform
        %int = OpTypeInt 32 1
      %int_0 = OpConstant %int 0
    %float_0 = OpConstant %float 0
%_ptr_Uniform_float = OpTypePointer Uniform %float
       %main = OpFunction %void None %3
          %5 = OpLabel
         %15 = OpAccessChain %_ptr_Uniform_float %ssbo %int_0 %int_0
               OpStore %15 %float_0
               OpReturn
               OpFunctionEnd
   */
   static const uint32_t words[] = {
      0x07230203, 0x00010000, 0x0008000b, 0x00000010, 0x00000000, 0x00020011,
      0x00000001, 0x0006000b, 0x00000001, 0x4c534c47, 0x6474732e, 0x3035342e,
      0x00000000, 0x0003000e, 0x00000000, 0x00000001, 0x0005000f, 0x00000005,
      0x00000004, 0x6e69616d, 0x00000000, 0x00060010, 0x00000004, 0x00000011,
      0x00000001, 0x00000001, 0x00000001, 0x00030003, 0x00000002, 0x000001cc,
      0x00070004, 0x455f4c47, 0x625f5458, 0x65666675, 0x65725f72, 0x65726566,
      0x0065636e, 0x00040005, 0x00000004, 0x6e69616d, 0x00000000, 0x00040005,
      0x00000008, 0x61746164, 0x00000000, 0x00040006, 0x00000008, 0x00000000,
      0x00000076, 0x00040005, 0x0000000a, 0x6f627373, 0x00000000, 0x00040047,
      0x00000007, 0x00000006, 0x00000004, 0x00030047, 0x00000008, 0x00000003,
      0x00050048, 0x00000008, 0x00000000, 0x00000023, 0x00000000, 0x00040047,
      0x0000000a, 0x00000021, 0x00000000, 0x00040047, 0x0000000a, 0x00000022,
      0x00000000, 0x00020013, 0x00000002, 0x00030021, 0x00000003, 0x00000002,
      0x00030016, 0x00000006, 0x00000020, 0x0003001d, 0x00000007, 0x00000006,
      0x0003001e, 0x00000008, 0x00000007, 0x00040020, 0x00000009, 0x00000002,
      0x00000008, 0x0004003b, 0x00000009, 0x0000000a, 0x00000002, 0x00040015,
      0x0000000b, 0x00000020, 0x00000001, 0x0004002b, 0x0000000b, 0x0000000c,
      0x00000000, 0x0004002b, 0x00000006, 0x0000000d, 0x00000000, 0x00040020,
      0x0000000e, 0x00000002, 0x00000006, 0x00050036, 0x00000002, 0x00000004,
      0x00000000, 0x00000003, 0x000200f8, 0x00000005, 0x00060041, 0x0000000e,
      0x0000000f, 0x0000000a, 0x0000000c, 0x0000000c, 0x0003003e, 0x0000000f,
      0x0000000d, 0x000100fd, 0x00010038,
   };

   spirv_options.workarounds.force_ssbo_non_uniform = true;

   get_nir(sizeof(words) / sizeof(words[0]), words, MESA_SHADER_COMPUTE);

   nir_intrinsic_instr *intrinsic = find_intrinsic(nir_intrinsic_store_deref, 0);
   ASSERT_NE(intrinsic, nullptr);

   ASSERT_EQ(nir_intrinsic_access(intrinsic), ACCESS_NON_UNIFORM);
}

TEST_F(Workarounds, force_tex_non_uniform)
{
   /*
               OpCapability Shader
          %1 = OpExtInstImport "GLSL.std.450"
               OpMemoryModel Logical GLSL450
               OpEntryPoint Fragment %main "main" %color %coord
               OpExecutionMode %main OriginUpperLeft
               OpSource GLSL 460
               OpName %main "main"
               OpName %color "color"
               OpName %samplerColor "samplerColor"
               OpName %coord "coord"
               OpDecorate %color Location 0
               OpDecorate %samplerColor Binding 0
               OpDecorate %samplerColor DescriptorSet 0
               OpDecorate %coord Location 0
       %void = OpTypeVoid
          %3 = OpTypeFunction %void
      %float = OpTypeFloat 32
    %v4float = OpTypeVector %float 4
%_ptr_Output_v4float = OpTypePointer Output %v4float
      %color = OpVariable %_ptr_Output_v4float Output
         %10 = OpTypeImage %float 2D 0 0 0 1 Unknown
         %11 = OpTypeSampledImage %10
%_ptr_UniformConstant_11 = OpTypePointer UniformConstant %11
%samplerColor = OpVariable %_ptr_UniformConstant_11 UniformConstant
    %v2float = OpTypeVector %float 2
%_ptr_Input_v2float = OpTypePointer Input %v2float
      %coord = OpVariable %_ptr_Input_v2float Input
       %main = OpFunction %void None %3
          %5 = OpLabel
         %14 = OpLoad %11 %samplerColor
         %18 = OpLoad %v2float %coord
         %19 = OpImageSampleImplicitLod %v4float %14 %18
               OpStore %color %19
               OpReturn
               OpFunctionEnd
   */
   static const uint32_t words[] = {
      0x07230203, 0x00010000, 0x0008000b, 0x00000014, 0x00000000, 0x00020011,
      0x00000001, 0x0006000b, 0x00000001, 0x4c534c47, 0x6474732e, 0x3035342e,
      0x00000000, 0x0003000e, 0x00000000, 0x00000001, 0x0007000f, 0x00000004,
      0x00000004, 0x6e69616d, 0x00000000, 0x00000009, 0x00000011, 0x00030010,
      0x00000004, 0x00000007, 0x00030003, 0x00000002, 0x000001cc, 0x00040005,
      0x00000004, 0x6e69616d, 0x00000000, 0x00040005, 0x00000009, 0x6f6c6f63,
      0x00000072, 0x00060005, 0x0000000d, 0x706d6173, 0x4372656c, 0x726f6c6f,
      0x00000000, 0x00040005, 0x00000011, 0x726f6f63, 0x00000064, 0x00040047,
      0x00000009, 0x0000001e, 0x00000000, 0x00040047, 0x0000000d, 0x00000021,
      0x00000000, 0x00040047, 0x0000000d, 0x00000022, 0x00000000, 0x00040047,
      0x00000011, 0x0000001e, 0x00000000, 0x00020013, 0x00000002, 0x00030021,
      0x00000003, 0x00000002, 0x00030016, 0x00000006, 0x00000020, 0x00040017,
      0x00000007, 0x00000006, 0x00000004, 0x00040020, 0x00000008, 0x00000003,
      0x00000007, 0x0004003b, 0x00000008, 0x00000009, 0x00000003, 0x00090019,
      0x0000000a, 0x00000006, 0x00000001, 0x00000000, 0x00000000, 0x00000000,
      0x00000001, 0x00000000, 0x0003001b, 0x0000000b, 0x0000000a, 0x00040020,
      0x0000000c, 0x00000000, 0x0000000b, 0x0004003b, 0x0000000c, 0x0000000d,
      0x00000000, 0x00040017, 0x0000000f, 0x00000006, 0x00000002, 0x00040020,
      0x00000010, 0x00000001, 0x0000000f, 0x0004003b, 0x00000010, 0x00000011,
      0x00000001, 0x00050036, 0x00000002, 0x00000004, 0x00000000, 0x00000003,
      0x000200f8, 0x00000005, 0x0004003d, 0x0000000b, 0x0000000e, 0x0000000d,
      0x0004003d, 0x0000000f, 0x00000012, 0x00000011, 0x00050057, 0x00000007,
      0x00000013, 0x0000000e, 0x00000012, 0x0003003e, 0x00000009, 0x00000013,
      0x000100fd, 0x00010038,
   };

   spirv_options.workarounds.force_tex_non_uniform = true;

   get_nir(sizeof(words) / sizeof(words[0]), words, MESA_SHADER_FRAGMENT);

   nir_tex_instr *tex_instr = find_tex_instr(nir_texop_tex, 0);
   ASSERT_NE(tex_instr, nullptr);

   ASSERT_TRUE(tex_instr->texture_non_uniform);
}
