/*
 * Copyright © 2025 Imagination Technologies Ltd.
 * SPDX-License-Identifier: MIT
 */

#ifndef __pvr_address_type
#define __pvr_address_type uint64_t
#define __pvr_get_address(addr) addr
#define __pvr_make_address(addr) addr
#endif /* __pvr_address_type */

#include "csbgen/rogue_texstate.h"
#include "libcl.h"


uint
usclib_tex_state_levels(uint4 tex_state)
{
   uint32_t texstate_image_word1[] = {tex_state.z, tex_state.w};
   struct ROGUE_TEXSTATE_IMAGE_WORD1 texstate_image_struct;

   ROGUE_TEXSTATE_IMAGE_WORD1_unpack(texstate_image_word1, &texstate_image_struct);

   return texstate_image_struct.num_mip_levels;
}

uint
usclib_tex_state_samples(uint4 tex_state)
{
   uint32_t texstate_image_word0[] = {tex_state.x, tex_state.y};
   struct ROGUE_TEXSTATE_IMAGE_WORD0 texstate_image_struct;

   ROGUE_TEXSTATE_IMAGE_WORD0_unpack(texstate_image_word0, &texstate_image_struct);

   return 1 << texstate_image_struct.smpcnt;
}

uint2
usclib_tex_state_address(uint4 tex_state)
{
   uint32_t texstate_image_word1[] = {tex_state.z, tex_state.w};
   struct ROGUE_TEXSTATE_IMAGE_WORD1 texstate_image_struct;

   ROGUE_TEXSTATE_IMAGE_WORD1_unpack(texstate_image_word1, &texstate_image_struct);

   uint64_t addr = texstate_image_struct.texaddr;

   return (uint2)(addr & 0xffffffff, addr >> 32);
}

uint
usclib_tex_state_array_max(uint4 tex_state)
{
   uint32_t texstate_image_word1[] = {tex_state.z, tex_state.w};
   struct ROGUE_TEXSTATE_IMAGE_WORD1 texstate_image_struct;

   ROGUE_TEXSTATE_IMAGE_WORD1_unpack(texstate_image_word1, &texstate_image_struct);

   return texstate_image_struct.depth;
}

uint3
usclib_tex_state_size(uint4 tex_state, uint num_comps, bool is_1d, bool is_array, uint lod)
{
   uint32_t texstate_image_word0[] = {tex_state.x, tex_state.y};
   uint32_t texstate_image_word1[] = {tex_state.z, tex_state.w};
   struct ROGUE_TEXSTATE_IMAGE_WORD0 texstate_image_struct0;
   struct ROGUE_TEXSTATE_IMAGE_WORD1 texstate_image_struct1;

   ROGUE_TEXSTATE_IMAGE_WORD0_unpack(texstate_image_word0, &texstate_image_struct0);
   ROGUE_TEXSTATE_IMAGE_WORD1_unpack(texstate_image_word1, &texstate_image_struct1);

   lod += texstate_image_struct1.baselevel;

   uint3 size_comps = (uint3)(texstate_image_struct0.width + 1,
                              texstate_image_struct0.height + 1,
                              texstate_image_struct1.depth + 1);

   if (is_array)
      --num_comps;

   for (unsigned u = 0; u < num_comps; ++u)
      size_comps[u] = MAX2(size_comps[u] >> lod, 1);

   if (is_1d && is_array)
      size_comps.y = size_comps.z;

   return size_comps;
}
