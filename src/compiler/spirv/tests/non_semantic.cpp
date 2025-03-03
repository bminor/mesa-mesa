/*
 * Copyright © 2025 Valve Corporation
 *
 * SPDX-License-Identifier: MIT
 */
#include "helpers.h"

class NonSemantic : public spirv_test {};

TEST_F(NonSemantic, debug_break)
{
   /*
               OpCapability Shader
               OpExtension "SPV_KHR_non_semantic_info"
          %1 = OpExtInstImport "GLSL.std.450"
          %6 = OpExtInstImport "NonSemantic.DebugBreak"
               OpMemoryModel Logical GLSL450
               OpEntryPoint GLCompute %main "main"
               OpExecutionMode %main LocalSize 1 1 1
               OpSource GLSL 460
               OpSourceExtension "GL_EXT_spirv_intrinsics"
               OpName %main "main"
       %void = OpTypeVoid
          %3 = OpTypeFunction %void
       %main = OpFunction %void None %3
          %5 = OpLabel
          %7 = OpExtInst %void %6 1
               OpReturn
               OpFunctionEnd
   */
   static const uint32_t words[] = {
      0x07230203, 0x00010000, 0x0008000b, 0x00000008, 0x00000000, 0x00020011,
      0x00000001, 0x0008000a, 0x5f565053, 0x5f52484b, 0x5f6e6f6e, 0x616d6573,
      0x6369746e, 0x666e695f, 0x0000006f, 0x0006000b, 0x00000001, 0x4c534c47,
      0x6474732e, 0x3035342e, 0x00000000, 0x0008000b, 0x00000006, 0x536e6f4e,
      0x6e616d65, 0x2e636974, 0x75626544, 0x65724267, 0x00006b61, 0x0003000e,
      0x00000000, 0x00000001, 0x0005000f, 0x00000005, 0x00000004, 0x6e69616d,
      0x00000000, 0x00060010, 0x00000004, 0x00000011, 0x00000001, 0x00000001,
      0x00000001, 0x00030003, 0x00000002, 0x000001cc, 0x00070004, 0x455f4c47,
      0x735f5458, 0x76726970, 0x746e695f, 0x736e6972, 0x00736369, 0x00040005,
      0x00000004, 0x6e69616d, 0x00000000, 0x00020013, 0x00000002, 0x00030021,
      0x00000003, 0x00000002, 0x00050036, 0x00000002, 0x00000004, 0x00000000,
      0x00000003, 0x000200f8, 0x00000005, 0x0005000c, 0x00000002, 0x00000007,
      0x00000006, 0x00000001, 0x000100fd, 0x00010038,
   };

   spirv_options.emit_debug_break = true;

   get_nir(sizeof(words) / sizeof(words[0]), words);

   nir_intrinsic_instr *intrinsic = find_intrinsic(nir_intrinsic_debug_break, 0);
   ASSERT_NE(intrinsic, nullptr);
}
