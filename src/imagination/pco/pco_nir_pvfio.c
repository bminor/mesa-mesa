/*
 * Copyright © 2024 Imagination Technologies Ltd.
 *
 * SPDX-License-Identifier: MIT
 */

/**
 * \file pco_nir_pvfio.c
 *
 * \brief PCO NIR per-vertex/fragment input/output passes.
 */

#include "compiler/glsl_types.h"
#include "compiler/shader_enums.h"
#include "nir.h"
#include "nir_builder.h"
#include "nir_format_convert.h"
#include "pco.h"
#include "pco_builder.h"
#include "pco_internal.h"
#include "util/macros.h"
#include "util/u_dynarray.h"

#include <assert.h>
#include <stdbool.h>
#include <stdio.h>

/** Per-fragment output pass state. */
struct pfo_state {
   struct util_dynarray loads; /** List of fragment loads. */
   struct util_dynarray stores; /** List of fragment stores. */

   /* Src for depth feedback (NULL if unused). */
   nir_def *depth_feedback_src;

   nir_def *discard_cond_reg;
   bool has_discards;

   nir_intrinsic_instr *last_discard_store;

   bool has_sample_check;

   /* nir_instr *terminate; */

   pco_fs_data *fs; /** Fragment-specific data. */
};

/** Per-vertex input pass state. */
struct pvi_state {
   nir_def *attribs[MAX_VERTEX_GENERIC_ATTRIBS]; /** Loaded vertex attribs. */
   pco_vs_data *vs; /** Vertex-specific data. */
};

/**
 * \brief Returns the GLSL base type equivalent of a pipe format.
 *
 * \param[in] format Pipe format.
 * \return The GLSL base type, or GLSL_TYPE_ERROR if unsupported/invalid.
 */
static inline enum glsl_base_type base_type_from_fmt(enum pipe_format format)
{
   const struct util_format_description *desc = util_format_description(format);
   int chan = util_format_get_first_non_void_channel(format);
   if (chan < 0)
      return GLSL_TYPE_ERROR;

   switch (desc->channel[chan].type) {
   case UTIL_FORMAT_TYPE_UNSIGNED:
      return GLSL_TYPE_UINT;

   case UTIL_FORMAT_TYPE_SIGNED:
      return GLSL_TYPE_INT;

   case UTIL_FORMAT_TYPE_FLOAT:
      return GLSL_TYPE_FLOAT;

   default:
      break;
   }

   return GLSL_TYPE_ERROR;
}

static enum pipe_format
to_pbe_format(nir_builder *b, enum pipe_format format, nir_def **input)
{
   switch (format) {
   case PIPE_FORMAT_B5G6R5_UNORM:
      return PIPE_FORMAT_R8G8B8_UNORM;

   case PIPE_FORMAT_A4R4G4B4_UNORM:
   case PIPE_FORMAT_B5G5R5A1_UNORM:
   case PIPE_FORMAT_B8G8R8A8_UNORM:
      return PIPE_FORMAT_R8G8B8A8_UNORM;

   case PIPE_FORMAT_R8G8B8A8_SRGB:
   case PIPE_FORMAT_B8G8R8A8_SRGB:
      if (input)
         *input = nir_fsat(b, *input);
      FALLTHROUGH;

   case PIPE_FORMAT_R10G10B10A2_UNORM:
      return PIPE_FORMAT_R16G16B16A16_FLOAT;

   case PIPE_FORMAT_R11G11B10_FLOAT:
      return PIPE_FORMAT_R16G16B16_FLOAT;

   /* For loadops. */
   case PIPE_FORMAT_Z32_FLOAT:
   case PIPE_FORMAT_Z32_FLOAT_S8X24_UINT:
      assert(b->shader->info.internal);
      return PIPE_FORMAT_R32_FLOAT;

   default:
      break;
   }

   return format;
}

static nir_def *pack_to_format(nir_builder *b,
                               nir_def *input,
                               nir_alu_type src_type,
                               enum pipe_format format)
{
   const struct util_format_description *desc = util_format_description(format);

   src_type = nir_alu_type_get_base_type(src_type);

   nir_def *input_comps[4];
   for (unsigned u = 0; u < desc->nr_channels; ++u) {
      enum pipe_swizzle s = desc->swizzle[u];
      if (s <= PIPE_SWIZZLE_W) {
         input_comps[u] = nir_channel(b, input, s);
      } else if (s == PIPE_SWIZZLE_0) {
         input_comps[u] = nir_imm_int(b, 0);
      } else if (s == PIPE_SWIZZLE_1) {
         input_comps[u] = src_type == nir_type_float ? nir_imm_float(b, 1.0f)
                                                     : nir_imm_int(b, 1);
      } else {
         UNREACHABLE("");
      }
   }

   input = nir_vec(b, input_comps, desc->nr_channels);

   nir_def *zero = nir_imm_int(b, 0);
   nir_def *packed[4] = { zero, zero, zero, zero };
   switch (format) {
   case PIPE_FORMAT_R8_UNORM:
      assert(src_type == nir_type_float);
      packed[0] = nir_pack_unorm_8(b, input);
      break;

   case PIPE_FORMAT_R8G8_UNORM:
      assert(src_type == nir_type_float);
      packed[0] = nir_pack_unorm_8_8(b, input);
      break;

   case PIPE_FORMAT_R8G8B8_UNORM:
      assert(src_type == nir_type_float);
      packed[0] = nir_pack_unorm_8_8_8(b, input);
      break;

   case PIPE_FORMAT_R8G8B8A8_UNORM:
      assert(src_type == nir_type_float);
      packed[0] = nir_pack_unorm_4x8(b, input);
      break;

   case PIPE_FORMAT_R8_SNORM:
      assert(src_type == nir_type_float);
      packed[0] = nir_pack_snorm_8(b, input);
      break;

   case PIPE_FORMAT_R8G8_SNORM:
      assert(src_type == nir_type_float);
      packed[0] = nir_pack_snorm_8_8(b, input);
      break;

   case PIPE_FORMAT_R8G8B8_SNORM:
      assert(src_type == nir_type_float);
      packed[0] = nir_pack_snorm_8_8_8(b, input);
      break;

   case PIPE_FORMAT_R8G8B8A8_SNORM:
      assert(src_type == nir_type_float);
      packed[0] = nir_pack_snorm_4x8(b, input);
      break;

   case PIPE_FORMAT_R8G8B8A8_UINT:
   case PIPE_FORMAT_R8G8B8A8_SINT:
      packed[0] = nir_bitfield_insert_imm(b, packed[0], input_comps[3], 24, 8);
      FALLTHROUGH;

   case PIPE_FORMAT_R8G8B8_UINT:
   case PIPE_FORMAT_R8G8B8_SINT:
      packed[0] = nir_bitfield_insert_imm(b, packed[0], input_comps[2], 16, 8);
      FALLTHROUGH;

   case PIPE_FORMAT_R8G8_UINT:
   case PIPE_FORMAT_R8G8_SINT:
      packed[0] = nir_bitfield_insert_imm(b, packed[0], input_comps[1], 8, 8);
      FALLTHROUGH;

   case PIPE_FORMAT_R8_UINT:
   case PIPE_FORMAT_R8_SINT:
      assert(src_type != nir_type_float);
      /* TODO: sat/clamp? */
      packed[0] = nir_bitfield_insert_imm(b, packed[0], input_comps[0], 0, 8);
      break;

   case PIPE_FORMAT_R10G10B10A2_UINT:
      assert(src_type == nir_type_uint);
      /* TODO: sat/clamp? */
      packed[0] = nir_bitfield_insert_imm(b, packed[0], input_comps[0], 0, 10);
      packed[0] = nir_bitfield_insert_imm(b, packed[0], input_comps[1], 10, 10);
      packed[0] = nir_bitfield_insert_imm(b, packed[0], input_comps[2], 20, 10);
      packed[0] = nir_bitfield_insert_imm(b, packed[0], input_comps[3], 30, 2);
      break;

   case PIPE_FORMAT_R11G11B10_FLOAT:
      assert(src_type == nir_type_float);
      packed[0] = nir_pack_float_11_11_10(b, input_comps[0]);
      break;

   case PIPE_FORMAT_R16_UNORM:
      assert(src_type == nir_type_float);
      packed[0] = nir_pack_unorm_16(b, input_comps[0]);
      break;

   case PIPE_FORMAT_R16G16_UNORM:
      assert(src_type == nir_type_float);
      packed[0] = nir_pack_unorm_2x16(b, input);
      break;

   case PIPE_FORMAT_R16G16B16_UNORM:
      assert(src_type == nir_type_float);
      packed[0] = nir_pack_unorm_2x16(b, nir_channels(b, input, 0b0011));
      packed[1] = nir_pack_unorm_16(b, input_comps[2]);
      break;

   case PIPE_FORMAT_R16G16B16A16_UNORM:
      assert(src_type == nir_type_float);
      packed[0] = nir_pack_unorm_2x16(b, nir_channels(b, input, 0b0011));
      packed[1] = nir_pack_unorm_2x16(b, nir_channels(b, input, 0b1100));
      break;

   case PIPE_FORMAT_R16_SNORM:
      assert(src_type == nir_type_float);
      packed[0] = nir_pack_snorm_16(b, input_comps[0]);
      break;

   case PIPE_FORMAT_R16G16_SNORM:
      assert(src_type == nir_type_float);
      packed[0] = nir_pack_snorm_2x16(b, input);
      break;

   case PIPE_FORMAT_R16G16B16_SNORM:
      assert(src_type == nir_type_float);
      packed[0] = nir_pack_snorm_2x16(b, nir_channels(b, input, 0b0011));
      packed[1] = nir_pack_snorm_16(b, input_comps[2]);
      break;

   case PIPE_FORMAT_R16G16B16A16_SNORM:
      assert(src_type == nir_type_float);
      packed[0] = nir_pack_snorm_2x16(b, nir_channels(b, input, 0b0011));
      packed[1] = nir_pack_snorm_2x16(b, nir_channels(b, input, 0b1100));
      break;

   case PIPE_FORMAT_R16G16B16A16_UINT:
   case PIPE_FORMAT_R16G16B16A16_SINT:
      packed[1] = nir_bitfield_insert_imm(b, packed[1], input_comps[3], 16, 16);
      FALLTHROUGH;

   case PIPE_FORMAT_R16G16B16_UINT:
   case PIPE_FORMAT_R16G16B16_SINT:
      packed[1] = nir_bitfield_insert_imm(b, packed[1], input_comps[2], 0, 16);
      FALLTHROUGH;

   case PIPE_FORMAT_R16G16_UINT:
   case PIPE_FORMAT_R16G16_SINT:
      packed[0] = nir_bitfield_insert_imm(b, packed[0], input_comps[1], 16, 16);
      FALLTHROUGH;

   case PIPE_FORMAT_R16_UINT:
   case PIPE_FORMAT_R16_SINT:
      assert(src_type != nir_type_float);
      /* TODO: sat/clamp? */
      packed[0] = nir_bitfield_insert_imm(b, packed[0], input_comps[0], 0, 16);
      break;

   case PIPE_FORMAT_R16_FLOAT:
      assert(src_type == nir_type_float);
      packed[0] = nir_pack_half_16(b, input);
      break;

   case PIPE_FORMAT_R16G16_FLOAT:
      assert(src_type == nir_type_float);
      packed[0] = nir_pack_half_2x16(b, input);
      break;

   case PIPE_FORMAT_R16G16B16_FLOAT:
      assert(src_type == nir_type_float);
      packed[0] = nir_pack_half_2x16(b, nir_channels(b, input, 0b0011));
      packed[1] = nir_pack_half_16(b, input_comps[2]);
      break;

   case PIPE_FORMAT_R16G16B16A16_FLOAT:
      assert(src_type == nir_type_float);
      packed[0] = nir_pack_half_2x16(b, nir_channels(b, input, 0b0011));
      packed[1] = nir_pack_half_2x16(b, nir_channels(b, input, 0b1100));
      break;

   case PIPE_FORMAT_R32G32B32A32_UINT:
   case PIPE_FORMAT_R32G32B32A32_SINT:
   case PIPE_FORMAT_R32G32B32A32_FLOAT:
      packed[3] = input_comps[3];
      FALLTHROUGH;

   case PIPE_FORMAT_R32G32B32_UINT:
   case PIPE_FORMAT_R32G32B32_SINT:
   case PIPE_FORMAT_R32G32B32_FLOAT:
      packed[2] = input_comps[2];
      FALLTHROUGH;

   case PIPE_FORMAT_R32G32_UINT:
   case PIPE_FORMAT_R32G32_SINT:
   case PIPE_FORMAT_R32G32_FLOAT:
      packed[1] = input_comps[1];
      FALLTHROUGH;

   case PIPE_FORMAT_R32_UINT:
   case PIPE_FORMAT_R32_SINT:
   case PIPE_FORMAT_R32_FLOAT:
      packed[0] = input_comps[0];
      break;

   default:
      printf("Unsupported pack format %s.\n", util_format_name(format));
      UNREACHABLE("");
   }

   unsigned packed_comps = 1;

   if (packed[3] != zero)
      packed_comps = 4;
   else if (packed[2] != zero)
      packed_comps = 3;
   else if (packed[1] != zero)
      packed_comps = 2;

   assert(packed[0] != zero);

   return nir_vec(b, packed, packed_comps);
}

static nir_def *unpack_from_format(nir_builder *b,
                                   nir_def *input,
                                   nir_alu_type dest_type,
                                   enum pipe_format format)
{
   const struct util_format_description *desc = util_format_description(format);

   dest_type = nir_alu_type_get_base_type(dest_type);

   nir_def *input_comps[4] = {
      nir_channel(b, input, 0),
      nir_channel(b, input, 1),
      nir_channel(b, input, 2),
      nir_channel(b, input, 3),
   };

   nir_def *unpacked = nir_undef(b, 4, 32);
   switch (format) {
   case PIPE_FORMAT_R8_UNORM:
      assert(dest_type == nir_type_float);
      unpacked = nir_unpack_unorm_8(b, input);
      break;

   case PIPE_FORMAT_R8G8_UNORM:
      assert(dest_type == nir_type_float);
      unpacked = nir_unpack_unorm_8_8(b, input);
      break;

   case PIPE_FORMAT_R8G8B8_UNORM:
      assert(dest_type == nir_type_float);
      unpacked = nir_unpack_unorm_8_8_8(b, input);
      break;

   case PIPE_FORMAT_R8G8B8A8_UNORM:
   case PIPE_FORMAT_B8G8R8A8_UNORM:
      assert(dest_type == nir_type_float);
      unpacked = nir_unpack_unorm_4x8(b, input);
      break;

   case PIPE_FORMAT_R8_SNORM:
      assert(dest_type == nir_type_float);
      unpacked = nir_unpack_snorm_8(b, input);
      break;

   case PIPE_FORMAT_R8G8_SNORM:
      assert(dest_type == nir_type_float);
      unpacked = nir_unpack_snorm_8_8(b, input);
      break;

   case PIPE_FORMAT_R8G8B8_SNORM:
      assert(dest_type == nir_type_float);
      unpacked = nir_unpack_snorm_8_8_8(b, input);
      break;

   case PIPE_FORMAT_R8G8B8A8_SNORM:
      assert(dest_type == nir_type_float);
      unpacked = nir_unpack_snorm_4x8(b, input);
      break;

   case PIPE_FORMAT_R8_SSCALED:
      assert(dest_type == nir_type_float);
      unpacked = nir_unpack_sscaled_8(b, input);
      break;

   case PIPE_FORMAT_R8G8_SSCALED:
      assert(dest_type == nir_type_float);
      unpacked = nir_unpack_sscaled_8_8(b, input);
      break;

   case PIPE_FORMAT_R8G8B8_SSCALED:
      assert(dest_type == nir_type_float);
      unpacked = nir_unpack_sscaled_8_8_8(b, input);
      break;

   case PIPE_FORMAT_R8G8B8A8_SSCALED:
      assert(dest_type == nir_type_float);
      unpacked = nir_unpack_sscaled_8_8_8_8(b, input);
      break;

   case PIPE_FORMAT_R8G8B8A8_UINT:
      unpacked = nir_vector_insert_imm(
         b,
         unpacked,
         nir_ubitfield_extract_imm(b, input_comps[0], 24, 8),
         3);
      FALLTHROUGH;

   case PIPE_FORMAT_R8G8B8_UINT:
      unpacked = nir_vector_insert_imm(
         b,
         unpacked,
         nir_ubitfield_extract_imm(b, input_comps[0], 16, 8),
         2);
      FALLTHROUGH;

   case PIPE_FORMAT_R8G8_UINT:
      unpacked = nir_vector_insert_imm(
         b,
         unpacked,
         nir_ubitfield_extract_imm(b, input_comps[0], 8, 8),
         1);
      FALLTHROUGH;

   case PIPE_FORMAT_R8_UINT:
      assert(dest_type == nir_type_uint);
      unpacked = nir_vector_insert_imm(
         b,
         unpacked,
         nir_ubitfield_extract_imm(b, input_comps[0], 0, 8),
         0);
      break;

   case PIPE_FORMAT_R8G8B8A8_SINT:
      unpacked = nir_vector_insert_imm(
         b,
         unpacked,
         nir_ibitfield_extract_imm(b, input_comps[0], 24, 8),
         3);
      FALLTHROUGH;

   case PIPE_FORMAT_R8G8B8_SINT:
      unpacked = nir_vector_insert_imm(
         b,
         unpacked,
         nir_ibitfield_extract_imm(b, input_comps[0], 16, 8),
         2);
      FALLTHROUGH;

   case PIPE_FORMAT_R8G8_SINT:
      unpacked = nir_vector_insert_imm(
         b,
         unpacked,
         nir_ibitfield_extract_imm(b, input_comps[0], 8, 8),
         1);
      FALLTHROUGH;

   case PIPE_FORMAT_R8_SINT:
      assert(dest_type == nir_type_int);
      unpacked = nir_vector_insert_imm(
         b,
         unpacked,
         nir_ibitfield_extract_imm(b, input_comps[0], 0, 8),
         0);
      break;

   case PIPE_FORMAT_R10G10B10A2_UNORM:
      assert(dest_type == nir_type_float);
      unpacked = nir_unpack_unorm_10_10_10_2(b, input_comps[0]);
      break;

   case PIPE_FORMAT_R10G10B10A2_SNORM:
      assert(dest_type == nir_type_float);
      unpacked = nir_unpack_snorm_10_10_10_2(b, input_comps[0]);
      break;

   case PIPE_FORMAT_R10G10B10A2_UINT:
      assert(dest_type == nir_type_uint);
      unpacked = nir_vec4(b,
                          nir_ubitfield_extract_imm(b, input_comps[0], 0, 10),
                          nir_ubitfield_extract_imm(b, input_comps[0], 10, 10),
                          nir_ubitfield_extract_imm(b, input_comps[0], 20, 10),
                          nir_ubitfield_extract_imm(b, input_comps[0], 30, 2));
      break;

   case PIPE_FORMAT_R10G10B10A2_USCALED:
   case PIPE_FORMAT_B10G10R10A2_USCALED:
      assert(dest_type == nir_type_float);
      unpacked = nir_unpack_uscaled_10_10_10_2(b, input_comps[0]);
      break;

   case PIPE_FORMAT_R10G10B10A2_SSCALED:
   case PIPE_FORMAT_B10G10R10A2_SSCALED:
      assert(dest_type == nir_type_float);
      unpacked = nir_unpack_sscaled_10_10_10_2(b, input_comps[0]);
      break;

   case PIPE_FORMAT_R11G11B10_FLOAT:
      assert(dest_type == nir_type_float);
      unpacked = nir_unpack_float_11_11_10(b, input_comps[0]);
      break;

   case PIPE_FORMAT_R16_UNORM:
      assert(dest_type == nir_type_float);
      unpacked = nir_unpack_unorm_16(b, input_comps[0]);
      break;

   case PIPE_FORMAT_R16G16_UNORM:
      assert(dest_type == nir_type_float);
      unpacked = nir_unpack_unorm_2x16(b, input_comps[0]);
      break;

   case PIPE_FORMAT_R16G16B16_UNORM: {
      assert(dest_type == nir_type_float);
      nir_def *lo2 = nir_unpack_unorm_2x16(b, input_comps[0]);
      unpacked = nir_vector_insert_imm(b, unpacked, nir_channel(b, lo2, 0), 0);
      unpacked = nir_vector_insert_imm(b, unpacked, nir_channel(b, lo2, 1), 1);
      unpacked = nir_vector_insert_imm(b,
                                       unpacked,
                                       nir_unpack_unorm_16(b, input_comps[1]),
                                       2);
      break;
   }

   case PIPE_FORMAT_R16G16B16A16_UNORM:
      assert(dest_type == nir_type_float);
      nir_def *lo2 = nir_unpack_unorm_2x16(b, input_comps[0]);
      nir_def *hi2 = nir_unpack_unorm_2x16(b, input_comps[1]);
      unpacked = nir_vector_insert_imm(b, unpacked, nir_channel(b, lo2, 0), 0);
      unpacked = nir_vector_insert_imm(b, unpacked, nir_channel(b, lo2, 1), 1);
      unpacked = nir_vector_insert_imm(b, unpacked, nir_channel(b, hi2, 0), 2);
      unpacked = nir_vector_insert_imm(b, unpacked, nir_channel(b, hi2, 1), 3);
      break;

   case PIPE_FORMAT_R16_SNORM:
      assert(dest_type == nir_type_float);
      unpacked = nir_unpack_snorm_16(b, input_comps[0]);
      break;

   case PIPE_FORMAT_R16G16_SNORM:
      assert(dest_type == nir_type_float);
      unpacked = nir_unpack_snorm_2x16(b, input_comps[0]);
      break;

   case PIPE_FORMAT_R16G16B16_SNORM: {
      assert(dest_type == nir_type_float);
      nir_def *lo2 = nir_unpack_snorm_2x16(b, input_comps[0]);
      unpacked = nir_vector_insert_imm(b, unpacked, nir_channel(b, lo2, 0), 0);
      unpacked = nir_vector_insert_imm(b, unpacked, nir_channel(b, lo2, 1), 1);
      unpacked = nir_vector_insert_imm(b,
                                       unpacked,
                                       nir_unpack_snorm_16(b, input_comps[1]),
                                       2);
      break;
   }

   case PIPE_FORMAT_R16G16B16A16_SNORM: {
      assert(dest_type == nir_type_float);
      nir_def *lo2 = nir_unpack_snorm_2x16(b, input_comps[0]);
      nir_def *hi2 = nir_unpack_snorm_2x16(b, input_comps[1]);
      unpacked = nir_vector_insert_imm(b, unpacked, nir_channel(b, lo2, 0), 0);
      unpacked = nir_vector_insert_imm(b, unpacked, nir_channel(b, lo2, 1), 1);
      unpacked = nir_vector_insert_imm(b, unpacked, nir_channel(b, hi2, 0), 2);
      unpacked = nir_vector_insert_imm(b, unpacked, nir_channel(b, hi2, 1), 3);
      break;
   }

   case PIPE_FORMAT_R16G16B16A16_UINT:
      unpacked = nir_vector_insert_imm(
         b,
         unpacked,
         nir_ubitfield_extract_imm(b, input_comps[1], 16, 16),
         3);
      FALLTHROUGH;

   case PIPE_FORMAT_R16G16B16_UINT:
      unpacked = nir_vector_insert_imm(
         b,
         unpacked,
         nir_ubitfield_extract_imm(b, input_comps[1], 0, 16),
         2);
      FALLTHROUGH;

   case PIPE_FORMAT_R16G16_UINT:
      unpacked = nir_vector_insert_imm(
         b,
         unpacked,
         nir_ubitfield_extract_imm(b, input_comps[0], 16, 16),
         1);
      FALLTHROUGH;

   case PIPE_FORMAT_R16_UINT:
      assert(dest_type == nir_type_uint);
      unpacked = nir_vector_insert_imm(
         b,
         unpacked,
         nir_ubitfield_extract_imm(b, input_comps[0], 0, 16),
         0);
      break;

   case PIPE_FORMAT_R16G16B16A16_SINT:
      unpacked = nir_vector_insert_imm(
         b,
         unpacked,
         nir_ibitfield_extract_imm(b, input_comps[1], 16, 16),
         3);
      FALLTHROUGH;

   case PIPE_FORMAT_R16G16B16_SINT:
      unpacked = nir_vector_insert_imm(
         b,
         unpacked,
         nir_ibitfield_extract_imm(b, input_comps[1], 0, 16),
         2);
      FALLTHROUGH;

   case PIPE_FORMAT_R16G16_SINT:
      unpacked = nir_vector_insert_imm(
         b,
         unpacked,
         nir_ibitfield_extract_imm(b, input_comps[0], 16, 16),
         1);
      FALLTHROUGH;

   case PIPE_FORMAT_R16_SINT:
      assert(dest_type == nir_type_int);
      unpacked = nir_vector_insert_imm(
         b,
         unpacked,
         nir_ibitfield_extract_imm(b, input_comps[0], 0, 16),
         0);
      break;

   case PIPE_FORMAT_R16_FLOAT:
      assert(dest_type == nir_type_float);
      unpacked = nir_unpack_half_16(b, input_comps[0]);
      break;

   case PIPE_FORMAT_R16G16_FLOAT:
      assert(dest_type == nir_type_float);
      unpacked = nir_unpack_half_2x16(b, input_comps[0]);
      break;

   case PIPE_FORMAT_R16G16B16_FLOAT: {
      assert(dest_type == nir_type_float);
      nir_def *lo2 = nir_unpack_half_2x16(b, input_comps[0]);

      unpacked = nir_vector_insert_imm(b, unpacked, nir_channel(b, lo2, 0), 0);
      unpacked = nir_vector_insert_imm(b, unpacked, nir_channel(b, lo2, 1), 1);
      unpacked = nir_vector_insert_imm(b,
                                       unpacked,
                                       nir_unpack_half_16(b, input_comps[1]),
                                       2);
      break;
   }

   case PIPE_FORMAT_R16G16B16A16_FLOAT: {
      assert(dest_type == nir_type_float);
      nir_def *lo2 = nir_unpack_half_2x16(b, input_comps[0]);
      nir_def *hi2 = nir_unpack_half_2x16(b, input_comps[1]);

      unpacked = nir_vector_insert_imm(b, unpacked, nir_channel(b, lo2, 0), 0);
      unpacked = nir_vector_insert_imm(b, unpacked, nir_channel(b, lo2, 1), 1);
      unpacked = nir_vector_insert_imm(b, unpacked, nir_channel(b, hi2, 0), 2);
      unpacked = nir_vector_insert_imm(b, unpacked, nir_channel(b, hi2, 1), 3);

      break;
   }

   case PIPE_FORMAT_R32G32B32A32_UINT:
   case PIPE_FORMAT_R32G32B32A32_SINT:
   case PIPE_FORMAT_R32G32B32A32_FLOAT:
      unpacked = nir_vector_insert_imm(b, unpacked, input_comps[3], 3);
      FALLTHROUGH;

   case PIPE_FORMAT_R32G32B32_UINT:
   case PIPE_FORMAT_R32G32B32_SINT:
   case PIPE_FORMAT_R32G32B32_FLOAT:
      unpacked = nir_vector_insert_imm(b, unpacked, input_comps[2], 2);
      FALLTHROUGH;

   case PIPE_FORMAT_R32G32_UINT:
   case PIPE_FORMAT_R32G32_SINT:
   case PIPE_FORMAT_R32G32_FLOAT:
      unpacked = nir_vector_insert_imm(b, unpacked, input_comps[1], 1);
      FALLTHROUGH;

   case PIPE_FORMAT_R32_UINT:
   case PIPE_FORMAT_R32_SINT:
   case PIPE_FORMAT_R32_FLOAT:
      unpacked = nir_vector_insert_imm(b, unpacked, input_comps[0], 0);
      break;

   default:
      printf("Unsupported unpack format %s.\n", util_format_name(format));
      UNREACHABLE("");
   }

   nir_def *output_comps[4];
   for (unsigned u = 0; u < ARRAY_SIZE(output_comps); ++u) {
      enum pipe_swizzle s = desc->swizzle[u];
      if (s <= PIPE_SWIZZLE_W) {
         output_comps[u] = nir_channel(b, unpacked, s);
      } else if (s == PIPE_SWIZZLE_0) {
         output_comps[u] = nir_imm_int(b, 0);
      } else if (s == PIPE_SWIZZLE_1) {
         output_comps[u] = dest_type == nir_type_float ? nir_imm_float(b, 1.0f)
                                                       : nir_imm_int(b, 1);
      } else {
         UNREACHABLE("");
      }
   }

   return nir_vec(b, output_comps, ARRAY_SIZE(output_comps));
}

static inline bool is_processed(nir_intrinsic_instr *intr)
{
   nir_alu_type type;

   if (nir_intrinsic_has_src_type(intr))
      type = nir_intrinsic_src_type(intr);
   else if (nir_intrinsic_has_dest_type(intr))
      type = nir_intrinsic_dest_type(intr);
   else
      return true;

   return nir_alu_type_get_base_type(type) == nir_type_invalid;
}

static nir_def *lower_pfo_store(nir_builder *b,
                                nir_intrinsic_instr *intr,
                                struct pfo_state *state)
{
   /* Skip stores we've already processed. */
   if (is_processed(intr)) {
      util_dynarray_append(&state->stores, nir_intrinsic_instr *, intr);
      return NULL;
   }

   nir_def *input = intr->src[0].ssa;
   nir_src *offset = &intr->src[1];
   assert(nir_src_as_uint(*offset) == 0);

   ASSERTED unsigned bit_size = input->bit_size;
   assert(bit_size == 32);

   unsigned component = nir_intrinsic_component(intr);
   assert(!component);

   struct nir_io_semantics io_semantics = nir_intrinsic_io_semantics(intr);
   gl_frag_result location = io_semantics.location;

   b->cursor = nir_before_instr(&intr->instr);

   enum pipe_format format = state->fs->output_formats[location];
   if (format == PIPE_FORMAT_NONE)
      return NIR_LOWER_INSTR_PROGRESS_REPLACE;

   format = to_pbe_format(b, format, &input);

   nir_alu_type src_type = nir_intrinsic_src_type(intr);
   nir_def *output = pack_to_format(b, input, src_type, format);

   /* Emit and track the new store. */
   nir_intrinsic_instr *store =
      nir_store_output(b,
                       output,
                       offset->ssa,
                       .base = nir_intrinsic_base(intr),
                       .write_mask = BITFIELD_MASK(output->num_components),
                       .src_type = nir_type_invalid | 32,
                       .component = component,
                       .io_semantics = io_semantics,
                       .io_xfb = nir_intrinsic_io_xfb(intr),
                       .io_xfb2 = nir_intrinsic_io_xfb2(intr));

   util_dynarray_append(&state->stores, nir_intrinsic_instr *, store);

   /* Update the type of the stored variable. */
   nir_variable *var =
      nir_find_variable_with_location(b->shader, nir_var_shader_out, location);
   assert(var);
   var->type = glsl_uvec_type(output->num_components);

   return NIR_LOWER_INSTR_PROGRESS_REPLACE;
}

static nir_def *lower_pfo_load(nir_builder *b,
                               nir_intrinsic_instr *intr,
                               struct pfo_state *state)
{
   /* Skip loads we've already processed. */
   if (is_processed(intr)) {
      util_dynarray_append(&state->loads, nir_intrinsic_instr *, intr);
      return NULL;
   }

   unsigned base = nir_intrinsic_base(intr);

   nir_src *offset = &intr->src[0];
   assert(nir_src_as_uint(*offset) == 0);

   struct nir_io_semantics io_semantics = nir_intrinsic_io_semantics(intr);
   gl_frag_result location = io_semantics.location;

   enum pipe_format format;
   /* Special case for input attachments. */
   if (location == FRAG_RESULT_COLOR) {
      format = state->fs->ia_formats[base];
   } else {
      assert(location >= FRAG_RESULT_DATA0);
      assert(!base);
      format = state->fs->output_formats[location];
   }

   if (format == PIPE_FORMAT_NONE)
      return nir_undef(b, intr->def.num_components, intr->def.bit_size);

   format = to_pbe_format(b, format, NULL);

   nir_def *input_comps[4];
   for (unsigned c = 0; c < ARRAY_SIZE(input_comps); ++c) {
      input_comps[c] = nir_load_output(b,
                                       1,
                                       32,
                                       offset->ssa,
                                       .base = base,
                                       .component = c,
                                       .dest_type = nir_type_invalid | 32,
                                       .io_semantics = io_semantics);

      nir_intrinsic_instr *load =
         nir_instr_as_intrinsic(input_comps[c]->parent_instr);

      util_dynarray_append(&state->loads, nir_intrinsic_instr *, load);
   }

   nir_def *input = nir_vec(b, input_comps, ARRAY_SIZE(input_comps));
   nir_alu_type dest_type = nir_intrinsic_dest_type(intr);
   nir_def *output = unpack_from_format(b, input, dest_type, format);
   if (output->num_components > intr->def.num_components)
      output = nir_trim_vector(b, output, intr->def.num_components);

   return output;
}

/**
 * \brief Filters PFO-related instructions.
 *
 * \param[in] instr NIR instruction.
 * \param[in] cb_data User callback data.
 * \return True if the instruction matches the filter.
 */
static bool is_pfo(const nir_instr *instr, UNUSED const void *cb_data)
{
   if (instr->type != nir_instr_type_intrinsic)
      return false;

   nir_intrinsic_instr *intr = nir_instr_as_intrinsic(instr);
   switch (intr->intrinsic) {
   case nir_intrinsic_store_output:
   case nir_intrinsic_load_output:
   case nir_intrinsic_terminate:
   case nir_intrinsic_terminate_if:
      return true;

   default:
      break;
   }

   return false;
}

/**
 * \brief Lowers a PFO-related instruction.
 *
 * \param[in] b NIR builder.
 * \param[in] instr NIR instruction.
 * \param[in] cb_data User callback data.
 * \return The replacement/lowered def.
 */
static nir_def *lower_pfo(nir_builder *b, nir_instr *instr, void *cb_data)
{
   struct pfo_state *state = cb_data;
   nir_intrinsic_instr *intr = nir_instr_as_intrinsic(instr);

   switch (intr->intrinsic) {
   case nir_intrinsic_store_output: {
      nir_io_semantics sem = nir_intrinsic_io_semantics(intr);
      if (sem.location >= FRAG_RESULT_DATA0)
         return lower_pfo_store(b, intr, state);

      if (sem.location == FRAG_RESULT_DEPTH) {
         assert(!state->depth_feedback_src);
         state->depth_feedback_src = nir_fsat(b, intr->src[0].ssa);

         return NIR_LOWER_INSTR_PROGRESS_REPLACE;
      }

      if (sem.location == FRAG_RESULT_SAMPLE_MASK) {
         nir_def *smp_msk =
            nir_ishl(b, nir_imm_int(b, 1), nir_load_sample_id(b));

         smp_msk = nir_iand(b, smp_msk, nir_load_sample_mask_in(b));
         smp_msk = nir_iand(b, smp_msk, intr->src[0].ssa);
         nir_def *cond = nir_ieq_imm(b, smp_msk, 0);

         state->has_discards = true;
         state->has_sample_check = true;
         nir_def *val = nir_load_reg(b, state->discard_cond_reg);
         val = nir_ior(b, val, cond);
         state->last_discard_store =
            nir_build_store_reg(b, val, state->discard_cond_reg);
         return NIR_LOWER_INSTR_PROGRESS_REPLACE;
      }

      UNREACHABLE("");
   }

   case nir_intrinsic_load_output:
      return lower_pfo_load(b, intr, state);

   case nir_intrinsic_terminate:
      state->has_discards = true;
      state->last_discard_store =
         nir_build_store_reg(b, nir_imm_true(b), state->discard_cond_reg);
      return NIR_LOWER_INSTR_PROGRESS_REPLACE;

   case nir_intrinsic_terminate_if: {
      state->has_discards = true;
      nir_def *val = nir_load_reg(b, state->discard_cond_reg);
      val = nir_ior(b, val, intr->src[0].ssa);
      state->last_discard_store =
         nir_build_store_reg(b, val, state->discard_cond_reg);
      return NIR_LOWER_INSTR_PROGRESS_REPLACE;
   }

   default:
      break;
   }

   return false;
}

static bool lower_isp_fb(nir_builder *b, struct pfo_state *state)
{
   bool has_depth_feedback = !!state->depth_feedback_src;
   if (b->shader->info.writes_memory && !has_depth_feedback) {
      nir_variable *var_pos = nir_get_variable_with_location(b->shader,
                                                             nir_var_shader_in,
                                                             VARYING_SLOT_POS,
                                                             glsl_vec4_type());
      var_pos->data.interpolation = INTERP_MODE_NOPERSPECTIVE;

      b->cursor = nir_before_block(
         nir_start_block(nir_shader_get_entrypoint(b->shader)));

      state->depth_feedback_src =
         nir_load_input(b,
                        1,
                        32,
                        nir_imm_int(b, 0),
                        .component = 2,
                        .dest_type = nir_type_float32,
                        .io_semantics = (nir_io_semantics){
                           .location = VARYING_SLOT_POS,
                           .num_slots = 1,
                        });

      has_depth_feedback = true;
   }

   if (!has_depth_feedback && !state->has_discards)
      return false;

   /* Insert isp feedback instruction before the first store,
    * or if there are no stores, at the end.
    */
   if (state->stores.size > 0)
      b->cursor = nir_before_instr(
         &(*(nir_intrinsic_instr **)util_dynarray_begin(&state->stores))->instr);
   else
      b->cursor = nir_after_block(
         nir_impl_last_block(nir_shader_get_entrypoint(b->shader)));

   nir_def *undef = nir_undef(b, 1, 32);

   nir_isp_feedback_pco(
      b,
      state->has_discards ? nir_i2b(b, nir_load_reg(b, state->discard_cond_reg))
                          : undef,
      has_depth_feedback ? state->depth_feedback_src : undef);

   state->fs->uses.discard = state->has_discards;
   state->fs->uses.depth_feedback = has_depth_feedback;

   return true;
}

static bool sink_outputs(nir_shader *shader, struct pfo_state *state)
{
   bool progress = false;

   nir_instr *after_instr = nir_block_last_instr(
      nir_impl_last_block(nir_shader_get_entrypoint(shader)));

   util_dynarray_foreach (&state->stores, nir_intrinsic_instr *, store) {
      nir_instr *instr = &(*store)->instr;

      progress |= nir_instr_move(nir_after_instr(after_instr), instr);
      after_instr = instr;
   }

   return progress;
}

static bool z_replicate(nir_shader *shader, struct pfo_state *state)
{
   if (shader->info.internal || state->fs->z_replicate == ~0u)
      return false;

   assert(!nir_find_variable_with_location(shader,
                                           nir_var_shader_out,
                                           state->fs->z_replicate));

   nir_create_variable_with_location(shader,
                                     nir_var_shader_out,
                                     state->fs->z_replicate,
                                     glsl_float_type());

   if (!state->depth_feedback_src) {
      nir_variable *var_pos = nir_get_variable_with_location(shader,
                                                             nir_var_shader_in,
                                                             VARYING_SLOT_POS,
                                                             glsl_vec4_type());
      var_pos->data.interpolation = INTERP_MODE_NOPERSPECTIVE;

      nir_builder b = nir_builder_at(
         nir_before_block(nir_start_block(nir_shader_get_entrypoint(shader))));

      state->depth_feedback_src =
         nir_load_input(&b,
                        1,
                        32,
                        nir_imm_int(&b, 0),
                        .component = 2,
                        .dest_type = nir_type_float32,
                        .io_semantics = (nir_io_semantics){
                           .location = VARYING_SLOT_POS,
                           .num_slots = 1,
                        });
   }

   nir_builder b = nir_builder_at(
      nir_after_block(nir_impl_last_block(nir_shader_get_entrypoint(shader))));
   nir_store_output(&b,
                    state->depth_feedback_src,
                    nir_imm_int(&b, 0),
                    .write_mask = 1,
                    .src_type = nir_type_invalid | 32,
                    .io_semantics = (nir_io_semantics){
                       .location = state->fs->z_replicate,
                       .num_slots = 1,
                    });

   return true;
}

static bool is_frag_color_out(const nir_instr *instr,
                              UNUSED const void *cb_data)
{
   if (instr->type != nir_instr_type_intrinsic)
      return false;

   nir_intrinsic_instr *intr = nir_instr_as_intrinsic(instr);
   if (intr->intrinsic != nir_intrinsic_store_output)
      return false;

   gl_frag_result location = nir_intrinsic_io_semantics(intr).location;
   return location >= FRAG_RESULT_DATA0 && location < FRAG_RESULT_MAX;
}

static nir_def *
lower_alpha_to_coverage(nir_builder *b, nir_instr *instr, UNUSED void *cb_data)
{
   nir_intrinsic_instr *intr = nir_instr_as_intrinsic(instr);

   /* "If multiple output colors, alpha for a2c is output color 0." */
   nir_io_semantics io_sem = nir_intrinsic_io_semantics(intr);
   if (io_sem.location != FRAG_RESULT_DATA0)
      return NULL;

   nir_def *input = intr->src[0].ssa;
   b->cursor = nir_before_instr(&intr->instr);
   nir_def *alpha = input->num_components < 4 ? nir_imm_float(b, 1.0f)
                                              : nir_channel(b, input, 3);

   nir_def *a2c_mask = nir_alpha_to_coverage_pco(b, alpha);

   a2c_mask = nir_iand(b, a2c_mask, nir_load_savmsk_vm_pco(b));

   a2c_mask = nir_iand(b,
                       a2c_mask,
                       nir_ishl(b, nir_imm_int(b, 1), nir_load_sample_id(b)));

   nir_terminate_if(b, nir_ieq_imm(b, a2c_mask, 0));

   return NULL;
}

bool pco_nir_lower_alpha_to_coverage(nir_shader *shader)
{
   assert(shader->info.stage == MESA_SHADER_FRAGMENT);
   return nir_shader_lower_instructions(shader,
                                        is_frag_color_out,
                                        lower_alpha_to_coverage,
                                        NULL);
}

static nir_def *
lower_alpha_to_one(nir_builder *b, nir_instr *instr, UNUSED void *cb_data)
{
   nir_intrinsic_instr *intr = nir_instr_as_intrinsic(instr);

   nir_src *input_src = &intr->src[0];
   nir_def *input = input_src->ssa;
   nir_src *offset = &intr->src[1];
   assert(nir_src_as_uint(*offset) == 0);

   /* Skip color write that don't include alpha. */
   if (input->num_components != 4)
      return NULL;

   b->cursor = nir_before_instr(&intr->instr);

   /* TODO: define or other way of representing bit 0 of metadata... */
   nir_def *alpha_to_one_enabled =
      nir_ine_imm(b,
                  nir_ubitfield_extract_imm(b, nir_load_fs_meta_pco(b), 0, 1),
                  0);

   nir_def *alpha = nir_bcsel(b,
                              alpha_to_one_enabled,
                              nir_imm_float(b, 1.0f),
                              nir_channel(b, input, 3));

   nir_src_rewrite(input_src, nir_vector_insert_imm(b, input, alpha, 3));

   return NIR_LOWER_INSTR_PROGRESS;
}

static bool is_load_sample_mask(const nir_instr *instr,
                                UNUSED const void *cb_data)
{
   if (instr->type != nir_instr_type_intrinsic)
      return false;

   nir_intrinsic_instr *intr = nir_instr_as_intrinsic(instr);
   return intr->intrinsic == nir_intrinsic_load_sample_mask_in;
}

static nir_def *
lower_load_sample_mask(nir_builder *b, nir_instr *instr, void *cb_data)
{
   struct pfo_state *state = cb_data;

   b->cursor = nir_before_instr(instr);

   if (!state->fs->meta_present.sample_mask)
      return nir_imm_int(b, 0xffff);

   nir_def *smp_msk =
      nir_ubitfield_extract_imm(b, nir_load_fs_meta_pco(b), 9, 16);

   smp_msk = nir_iand(b, smp_msk, nir_load_savmsk_vm_pco(b));

   return smp_msk;
}

static nir_def *
lower_color_write_enable(nir_builder *b, nir_instr *instr, UNUSED void *cb_data)
{
   nir_intrinsic_instr *intr = nir_instr_as_intrinsic(instr);

   nir_src *input_src = &intr->src[0];
   nir_def *input = input_src->ssa;
   nir_def *offset = intr->src[1].ssa;

   struct nir_io_semantics io_semantics = nir_intrinsic_io_semantics(intr);
   unsigned color_write_index = io_semantics.location - FRAG_RESULT_DATA0;
   io_semantics.fb_fetch_output = true;

   b->cursor = nir_before_instr(&intr->instr);

   /* TODO: nir op that returns bool based on whether a bit is set. */
   /* TODO: define for 1 */
   nir_def *color_write_enabled =
      nir_ine_imm(b,
                  nir_ubitfield_extract_imm(b,
                                            nir_load_fs_meta_pco(b),
                                            1 + color_write_index,
                                            1),
                  0);

   nir_def *prev_input =
      nir_load_output(b,
                      input->num_components,
                      input->bit_size,
                      offset,
                      .base = nir_intrinsic_base(intr),
                      .range = nir_intrinsic_range(intr),
                      .component = nir_intrinsic_component(intr),
                      .dest_type = nir_intrinsic_src_type(intr),
                      .io_semantics = io_semantics);

   nir_src_rewrite(input_src,
                   nir_bcsel(b, color_write_enabled, input, prev_input));

   return NIR_LOWER_INSTR_PROGRESS;
}

/**
 * \brief Per-fragment output pass.
 *
 * \param[in,out] shader NIR shader.
 * \param[in,out] fs Fragment shader-specific data.
 * \return True if the pass made progress.
 */
bool pco_nir_pfo(nir_shader *shader, pco_fs_data *fs)
{
   assert(shader->info.stage == MESA_SHADER_FRAGMENT);

   nir_builder b = nir_builder_create(nir_shader_get_entrypoint(shader));
   b.cursor =
      nir_before_block(nir_start_block(nir_shader_get_entrypoint(shader)));

   struct pfo_state state = {
      .fs = fs,
      .discard_cond_reg = nir_decl_reg(&b, 1, 1, 0),
   };
   state.last_discard_store =
      nir_build_store_reg(&b, nir_imm_false(&b), state.discard_cond_reg);

   util_dynarray_init(&state.loads, NULL);
   util_dynarray_init(&state.stores, NULL);

   bool progress = false;

   /* TODO: instead of doing multiple passes, probably better to just cache all
    * the stores
    */
   if (fs->meta_present.alpha_to_one)
      progress |= nir_shader_lower_instructions(shader,
                                                is_frag_color_out,
                                                lower_alpha_to_one,
                                                &state);

   if (fs->meta_present.color_write_enable)
      progress |= nir_shader_lower_instructions(shader,
                                                is_frag_color_out,
                                                lower_color_write_enable,
                                                NULL);

   progress |= nir_shader_lower_instructions(shader, is_pfo, lower_pfo, &state);
   progress |= lower_isp_fb(&b, &state);

   progress |= sink_outputs(shader, &state);
   progress |= z_replicate(shader, &state);

   progress |= nir_shader_lower_instructions(shader,
                                             is_load_sample_mask,
                                             lower_load_sample_mask,
                                             &state);

   util_dynarray_fini(&state.stores);
   util_dynarray_fini(&state.loads);

   return progress;
}

static nir_def *lower_pvi(nir_builder *b, nir_instr *instr, void *cb_data)
{
   nir_intrinsic_instr *intr = nir_instr_as_intrinsic(instr);
   struct pvi_state *state = cb_data;

   unsigned start_comp = nir_intrinsic_component(intr);
   unsigned num_comps = intr->def.num_components;

   ASSERTED nir_src *offset = &intr->src[0];
   assert(nir_src_as_uint(*offset) == 0);

   struct nir_io_semantics io_semantics = nir_intrinsic_io_semantics(intr);
   gl_vert_attrib location = io_semantics.location;
   nir_def *attrib = state->attribs[location - VERT_ATTRIB_GENERIC0];
   assert(attrib);

   b->cursor = nir_before_instr(&intr->instr);
   return nir_channels(b, attrib, BITFIELD_RANGE(start_comp, num_comps));
}

static bool is_pvi(const nir_instr *instr, const void *cb_data)
{
   if (instr->type != nir_instr_type_intrinsic)
      return false;

   nir_intrinsic_instr *intr = nir_instr_as_intrinsic(instr);
   if (intr->intrinsic != nir_intrinsic_load_input)
      return false;

   if (is_processed(intr))
      return false;

   ASSERTED gl_vert_attrib location = nir_intrinsic_io_semantics(intr).location;
   assert(location >= VERT_ATTRIB_GENERIC0 &&
          location <= VERT_ATTRIB_GENERIC15);

   return true;
}

/**
 * \brief Per-vertex input pass.
 *
 * \param[in,out] shader NIR shader.
 * \param[in,out] vs Vertex shader-specific data.
 * \return True if the pass made progress.
 */
bool pco_nir_pvi(nir_shader *shader, pco_vs_data *vs)
{
   assert(shader->info.stage == MESA_SHADER_VERTEX);

   struct pvi_state state = { .vs = vs };

   nir_builder b = nir_builder_at(
      nir_before_block(nir_start_block(nir_shader_get_entrypoint(shader))));
   for (unsigned u = 0; u < ARRAY_SIZE(state.attribs); ++u) {
      gl_vert_attrib location = u + VERT_ATTRIB_GENERIC0;
      enum pipe_format format = vs->attrib_formats[location];
      if (format == PIPE_FORMAT_NONE)
         continue;

      /* Update the type of the stored variable, remove any fractional vars. */
      nir_variable *var = NULL;
      nir_alu_type base_type = 0;
      nir_foreach_variable_with_modes_safe (iter_var,
                                            shader,
                                            nir_var_shader_in) {
         if (iter_var->data.location != location)
            continue;

         if (!base_type)
            base_type = nir_get_nir_type_for_glsl_type(iter_var->type);
#ifndef NDEBUG
         else
            assert(base_type == nir_get_nir_type_for_glsl_type(iter_var->type));
#endif /* NDEBUG */

         if (!iter_var->data.location_frac) {
            assert(!var);
            var = iter_var;
            continue;
         }

         exec_node_remove(&iter_var->node);
      }

      if (!var) {
         if (!base_type)
            continue;

         /* An attrib var was found but was fractional so we dropped it. */
         var = nir_variable_create(shader, nir_var_shader_in, NULL, NULL);
         var->data.location = location;
      }

      unsigned format_dwords =
         DIV_ROUND_UP(util_format_get_blocksize(format), sizeof(uint32_t));
      var->type = glsl_uvec_type(format_dwords);

      nir_def *input_comps[4];
      for (unsigned c = 0; c < ARRAY_SIZE(input_comps); ++c) {
         input_comps[c] = nir_load_input(&b,
                                         1,
                                         32,
                                         nir_imm_int(&b, 0),
                                         .range = 1,
                                         .component = c,
                                         .dest_type = nir_type_invalid | 32,
                                         .io_semantics = (nir_io_semantics){
                                            .location = location,
                                            .num_slots = 1,
                                         });
      }

      nir_def *input = nir_vec(&b, input_comps, ARRAY_SIZE(input_comps));
      state.attribs[u] = unpack_from_format(&b, input, base_type, format);
   }

   nir_shader_lower_instructions(shader, is_pvi, lower_pvi, &state);

   return true;
}

/**
 * \brief Checks if the point size is written.
 *
 * \param[in] b NIR builder.
 * \param[in] intr NIR intrinsic instruction.
 * \param[in] cb_data User callback data.
 * \return True if the instruction was lowered.
 */
static bool
check_psiz_write(nir_builder *b, nir_intrinsic_instr *intr, void *cb_data)
{
   if (intr->intrinsic != nir_intrinsic_store_output)
      return false;

   bool *writes_psiz = cb_data;

   struct nir_io_semantics io_semantics = nir_intrinsic_io_semantics(intr);
   *writes_psiz |= (io_semantics.location == VARYING_SLOT_PSIZ);

   return false;
}

/**
 * \brief Vertex shader point size pass.
 *
 * \param[in,out] shader NIR shader.
 * \return True if the pass made progress.
 */
bool pco_nir_point_size(nir_shader *shader)
{
   assert(shader->info.stage == MESA_SHADER_VERTEX);
   if (shader->info.internal)
      return false;

   bool writes_psiz = false;
   nir_shader_intrinsics_pass(shader,
                              check_psiz_write,
                              nir_metadata_all,
                              &writes_psiz);

   /* Nothing to do if the shader already writes the point size. */
   if (writes_psiz)
      return false;

   /* Create a point size variable if there isn't one. */
   nir_get_variable_with_location(shader,
                                  nir_var_shader_out,
                                  VARYING_SLOT_PSIZ,
                                  glsl_float_type());

   /* Add a point size write. */
   nir_builder b = nir_builder_at(
      nir_after_block(nir_impl_last_block(nir_shader_get_entrypoint(shader))));

   nir_store_output(&b,
                    nir_imm_float(&b, PVR_POINT_SIZE_RANGE_MIN),
                    nir_imm_int(&b, 0),
                    .base = 0,
                    .range = 1,
                    .write_mask = 1,
                    .component = 0,
                    .src_type = nir_type_float32,
                    .io_semantics = (nir_io_semantics){
                       .location = VARYING_SLOT_PSIZ,
                       .num_slots = 1,
                    });

   return true;
}

static bool is_fs_intr(const nir_instr *instr, UNUSED const void *cb_data)
{
   if (instr->type != nir_instr_type_intrinsic)
      return false;

   nir_intrinsic_instr *intr = nir_instr_as_intrinsic(instr);
   switch (intr->intrinsic) {
   case nir_intrinsic_load_front_face:
      return true;

   default:
      break;
   }

   return false;
}

static nir_def *lower_front_face(nir_builder *b, nir_intrinsic_instr *intr)
{
   nir_def *face_ccw = nir_load_face_ccw_pco(b);
   nir_def *front_face = nir_ieq_imm(b, face_ccw, 0);

   nir_def *ff_op = nir_load_front_face_op_pco(b);
   nir_def *ff_elems[] = {
      [PCO_FRONT_FACE_OP_NOP] = front_face,
      [PCO_FRONT_FACE_OP_SWAP] = nir_inot(b, front_face),
      [PCO_FRONT_FACE_OP_TRUE] = nir_imm_true(b),
   };

   return nir_select_from_ssa_def_array(b,
                                        ff_elems,
                                        ARRAY_SIZE(ff_elems),
                                        ff_op);
}

static nir_def *
lower_fs_intr(nir_builder *b, nir_instr *instr, UNUSED void *cb_data)
{
   nir_intrinsic_instr *intr = nir_instr_as_intrinsic(instr);
   switch (intr->intrinsic) {
   case nir_intrinsic_load_front_face:
      return lower_front_face(b, intr);

   default:
      break;
   }

   UNREACHABLE("");
}

bool pco_nir_lower_fs_intrinsics(nir_shader *shader)
{
   assert(shader->info.stage == MESA_SHADER_FRAGMENT);

   return nir_shader_lower_instructions(shader, is_fs_intr, lower_fs_intr, NULL);
}

bool pco_nir_lower_clip_cull_vars(nir_shader *shader)
{
   if (shader->info.internal)
      return false;

   unsigned clip_cull_comps = shader->info.clip_distance_array_size +
                              shader->info.cull_distance_array_size;
   if (!clip_cull_comps)
      return false;

   /* Remove the old variables. */
   const gl_varying_slot clip_cull_locations[] = {
      VARYING_SLOT_CLIP_DIST0,
      VARYING_SLOT_CLIP_DIST1,
   };

   nir_variable *var;
   for (unsigned u = 0; u < ARRAY_SIZE(clip_cull_locations); ++u) {
      gl_varying_slot location = clip_cull_locations[u];
      while ((var = nir_find_variable_with_location(shader,
                                                    nir_var_shader_out,
                                                    location))) {
         exec_node_remove(&var->node);
      }
   }

   /* Create new variables. */
   nir_create_variable_with_location(shader,
                                     nir_var_shader_out,
                                     VARYING_SLOT_CLIP_DIST0,
                                     glsl_vec_type(MIN2(clip_cull_comps, 4)));

   if (clip_cull_comps > 4) {
      nir_create_variable_with_location(shader,
                                        nir_var_shader_out,
                                        VARYING_SLOT_CLIP_DIST1,
                                        glsl_vec_type(clip_cull_comps - 4));
   }

   nir_metadata_invalidate(shader);

   return true;
}

static bool
clone_clip_cull_stores(nir_builder *b, nir_intrinsic_instr *intr, void *data)
{
   if (intr->intrinsic != nir_intrinsic_store_deref)
      return false;

   nir_deref_instr *deref = nir_src_as_deref(intr->src[0]);
   if (deref->deref_type != nir_deref_type_array)
      return false;

   nir_variable *var = nir_deref_instr_get_variable(deref);
   if (var->data.location != VARYING_SLOT_CLIP_DIST0 &&
       var->data.location != VARYING_SLOT_CLIP_DIST1)
      return false;

   b->cursor = nir_after_instr(&intr->instr);

   unsigned var_index = var->data.location - VARYING_SLOT_CLIP_DIST0;
   nir_def *index =
      nir_iadd_imm(b, deref->arr.index.ssa, var->data.location_frac);
   index = nir_iadd_imm(b, index, var_index * 4);

   nir_variable *clone_var = data;
   nir_store_array_var(b, clone_var, index, intr->src[1].ssa, 1);

   return true;
}

static bool is_clip_cull_load(const nir_instr *instr,
                              UNUSED const void *cb_data)
{
   if (instr->type != nir_instr_type_intrinsic)
      return false;

   nir_intrinsic_instr *intr = nir_instr_as_intrinsic(instr);
   if (intr->intrinsic != nir_intrinsic_load_deref)
      return false;

   nir_deref_instr *deref = nir_src_as_deref(intr->src[0]);
   if (deref->deref_type != nir_deref_type_array)
      return false;

   nir_variable *var = nir_deref_instr_get_variable(deref);

   return var->data.location == VARYING_SLOT_CLIP_DIST0 ||
          var->data.location == VARYING_SLOT_CLIP_DIST1;
}

static nir_def *
swap_clip_cull_load(nir_builder *b, nir_instr *instr, void *cb_data)
{
   nir_intrinsic_instr *intr = nir_instr_as_intrinsic(instr);
   nir_deref_instr *deref = nir_src_as_deref(intr->src[0]);
   nir_variable *var = nir_deref_instr_get_variable(deref);

   unsigned var_index = var->data.location - VARYING_SLOT_CLIP_DIST0;
   nir_def *index =
      nir_iadd_imm(b, deref->arr.index.ssa, var->data.location_frac);
   index = nir_iadd_imm(b, index, var_index * 4);

   nir_variable *clone_var = cb_data;
   return nir_load_array_var(b, clone_var, index);
}

bool pco_nir_link_clip_cull_vars(nir_shader *producer, nir_shader *consumer)
{
   if (producer->info.stage != MESA_SHADER_VERTEX ||
       consumer->info.stage != MESA_SHADER_FRAGMENT) {
      return false;
   }

   unsigned clip_cull_comps = consumer->info.clip_distance_array_size +
                              consumer->info.cull_distance_array_size;
   /* Skip if clip/cull comps aren't actually consumed. */
   if (!clip_cull_comps)
      return false;

   const glsl_type *clone_var_type =
      glsl_array_type(glsl_float_type(), clip_cull_comps, 0);

   /* Find unused varying slot to use and create the variables. */
   gl_varying_slot clone_slot = VARYING_SLOT_VAR0;
   nir_foreach_shader_out_variable (var, producer) {
      clone_slot = MAX2(clone_slot, var->data.location + 1);
   }
   assert(clone_slot < VARYING_SLOT_MAX);

   nir_variable *clone_var =
      nir_variable_create(producer, nir_var_shader_out, clone_var_type, NULL);
   clone_var->data.location = clone_slot;

   nir_shader_intrinsics_pass(producer,
                              clone_clip_cull_stores,
                              nir_metadata_block_index | nir_metadata_dominance,
                              clone_var);

   clone_var =
      nir_variable_create(consumer, nir_var_shader_in, clone_var_type, NULL);
   clone_var->data.location = clone_slot;

   nir_shader_lower_instructions(consumer,
                                 is_clip_cull_load,
                                 swap_clip_cull_load,
                                 clone_var);

   return true;
}
