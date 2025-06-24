/*
 * Copyright (C) 2022 Collabora, Ltd.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the next
 * paragraph) shall be included in all copies or substantial portions of the
 * Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */

#include "pan_afbc.h"
#include "pan_afrc.h"
#include "pan_format.h"
#include "pan_image.h"
#include "pan_layout.h"
#include "pan_mod.h"

#include <gtest/gtest.h>

TEST(Align, UTiledLinear)
{
   struct {
      unsigned arch;
      enum pipe_format format;
      unsigned plane_idx;
      unsigned alignment;
   } cases[] = {
      { 6, PIPE_FORMAT_ETC2_RGB8, 0, 8 },
      { 6, PIPE_FORMAT_R32G32B32_FLOAT, 0, 4 },
      { 6, PIPE_FORMAT_R8G8B8A8_UNORM, 0, 1 },
      { 6, PIPE_FORMAT_R5G6B5_UNORM, 0, 2 },
      { 6, PIPE_FORMAT_R8_G8B8_420_UNORM, 0, 1 },
      { 6, PIPE_FORMAT_R8_G8B8_420_UNORM, 1, 2 },
      { 7, PIPE_FORMAT_ETC2_RGB8, 0, 64 },
      { 7, PIPE_FORMAT_R32G32B32_FLOAT, 0, 64 },
      { 7, PIPE_FORMAT_R8G8B8A8_UNORM, 0, 64 },
      { 7, PIPE_FORMAT_R5G6B5_UNORM, 0, 64 },
      { 7, PIPE_FORMAT_R8_G8B8_420_UNORM, 0, 16 },
      { 7, PIPE_FORMAT_R8_G8B8_420_UNORM, 1, 16 },
      { 7, PIPE_FORMAT_R10_G10B10_420_UNORM, 0, 1 },
      { 7, PIPE_FORMAT_R10_G10B10_420_UNORM, 1, 1 },
   };
   for (unsigned i = 0; i < ARRAY_SIZE(cases); ++i) {
      unsigned align = pan_linear_or_tiled_row_align_req(
         cases[i].arch, cases[i].format, cases[i].plane_idx);

      EXPECT_EQ(align, cases[i].alignment);
   }
}

TEST(BlockSize, UInterleavedRegular)
{
   enum pipe_format format[] = {
      PIPE_FORMAT_R32G32B32_FLOAT,
      PIPE_FORMAT_R8G8B8_UNORM,
   };

   for (unsigned i = 0; i < ARRAY_SIZE(format); ++i) {
      struct pan_image_block_size blk =
         pan_u_interleaved_tile_size_el(format[i]);

      EXPECT_EQ(blk.width, 16);
      EXPECT_EQ(blk.height, 16);
   }
}

TEST(BlockSize, UInterleavedBlockCompressed)
{
   enum pipe_format format[] = {PIPE_FORMAT_ETC2_RGB8, PIPE_FORMAT_ASTC_5x5};

   for (unsigned i = 0; i < ARRAY_SIZE(format); ++i) {
      struct pan_image_block_size blk =
         pan_u_interleaved_tile_size_el(format[i]);

      EXPECT_EQ(blk.width, 4);
      EXPECT_EQ(blk.height, 4);
   }
}

TEST(BlockSize, AFBCFormatInvariant16x16)
{
   enum pipe_format format[] = {PIPE_FORMAT_R32G32B32_FLOAT,
                                PIPE_FORMAT_R8G8B8_UNORM};

   uint64_t modifier =
      DRM_FORMAT_MOD_ARM_AFBC(AFBC_FORMAT_MOD_BLOCK_SIZE_16x16 |
                              AFBC_FORMAT_MOD_SPARSE | AFBC_FORMAT_MOD_YTR);

   for (unsigned i = 0; i < ARRAY_SIZE(format); ++i) {
      struct pan_image_block_size blk =
         pan_afbc_superblock_size_el(format[i], modifier);

      EXPECT_EQ(blk.width, 16);
      EXPECT_EQ(blk.height, 16);
   }
}

TEST(BlockSize, AFBCFormatInvariant32x8)
{
   enum pipe_format format[] = {PIPE_FORMAT_R32G32B32_FLOAT,
                                PIPE_FORMAT_R8G8B8_UNORM};

   uint64_t modifier =
      DRM_FORMAT_MOD_ARM_AFBC(AFBC_FORMAT_MOD_BLOCK_SIZE_32x8 |
                              AFBC_FORMAT_MOD_SPARSE | AFBC_FORMAT_MOD_YTR);

   for (unsigned i = 0; i < ARRAY_SIZE(format); ++i) {
      struct pan_image_block_size blk =
         pan_afbc_superblock_size_el(format[i], modifier);

      EXPECT_EQ(blk.width, 32);
      EXPECT_EQ(blk.height, 8);
   }
}

TEST(BlockSize, AFBCSuperblock16x16)
{
   uint64_t modifier =
      DRM_FORMAT_MOD_ARM_AFBC(AFBC_FORMAT_MOD_BLOCK_SIZE_16x16 |
                              AFBC_FORMAT_MOD_SPARSE | AFBC_FORMAT_MOD_YTR);

   EXPECT_EQ(pan_afbc_superblock_size(modifier).width, 16);
   EXPECT_EQ(pan_afbc_superblock_width(modifier), 16);

   EXPECT_EQ(pan_afbc_superblock_size(modifier).height, 16);
   EXPECT_EQ(pan_afbc_superblock_height(modifier), 16);

   EXPECT_FALSE(pan_afbc_is_wide(modifier));
}

TEST(BlockSize, AFBCSuperblock32x8)
{
   uint64_t modifier = DRM_FORMAT_MOD_ARM_AFBC(AFBC_FORMAT_MOD_BLOCK_SIZE_32x8 |
                                               AFBC_FORMAT_MOD_SPARSE);

   EXPECT_EQ(pan_afbc_superblock_size(modifier).width, 32);
   EXPECT_EQ(pan_afbc_superblock_width(modifier), 32);

   EXPECT_EQ(pan_afbc_superblock_size(modifier).height, 8);
   EXPECT_EQ(pan_afbc_superblock_height(modifier), 8);

   EXPECT_TRUE(pan_afbc_is_wide(modifier));
}

TEST(BlockSize, AFBCSuperblock64x4)
{
   uint64_t modifier = DRM_FORMAT_MOD_ARM_AFBC(AFBC_FORMAT_MOD_BLOCK_SIZE_64x4 |
                                               AFBC_FORMAT_MOD_SPARSE);

   EXPECT_EQ(pan_afbc_superblock_size(modifier).width, 64);
   EXPECT_EQ(pan_afbc_superblock_width(modifier), 64);

   EXPECT_EQ(pan_afbc_superblock_size(modifier).height, 4);
   EXPECT_EQ(pan_afbc_superblock_height(modifier), 4);

   EXPECT_TRUE(pan_afbc_is_wide(modifier));
}

/* Calculate Bifrost line stride, since we have reference formulas for Bifrost
 * stride calculations.
 */
static uint32_t
pan_afbc_line_stride(uint64_t modifier, uint32_t width)
{
   return pan_afbc_stride_blocks(modifier,
                                 pan_afbc_row_stride(modifier, width));
}

/* Which form of the stride we specify is hardware specific (row stride for
 * Valhall, line stride for Bifrost). However, the layout code is hardware
 * independent, so we test both row stride and line stride calculations.
 */
TEST(AFBCStride, Linear)
{
   uint64_t modifiers[] = {
      DRM_FORMAT_MOD_ARM_AFBC(AFBC_FORMAT_MOD_BLOCK_SIZE_16x16 |
                              AFBC_FORMAT_MOD_SPARSE),
      DRM_FORMAT_MOD_ARM_AFBC(AFBC_FORMAT_MOD_BLOCK_SIZE_32x8 |
                              AFBC_FORMAT_MOD_SPARSE),
      DRM_FORMAT_MOD_ARM_AFBC(AFBC_FORMAT_MOD_BLOCK_SIZE_64x4 |
                              AFBC_FORMAT_MOD_SPARSE),
   };

   for (unsigned m = 0; m < ARRAY_SIZE(modifiers); ++m) {
      uint64_t modifier = modifiers[m];

      uint32_t sw = pan_afbc_superblock_width(modifier);
      uint32_t cases[] = {1, 4, 17, 39};

      for (unsigned i = 0; i < ARRAY_SIZE(cases); ++i) {
         uint32_t width = sw * cases[i];

         EXPECT_EQ(pan_afbc_row_stride(modifier, width),
                   16 * DIV_ROUND_UP(width, sw));

         EXPECT_EQ(pan_afbc_line_stride(modifier, width),
                   DIV_ROUND_UP(width, sw));
      }
   }
}

TEST(AFBCStride, Tiled)
{
   uint64_t modifiers[] = {
      DRM_FORMAT_MOD_ARM_AFBC(AFBC_FORMAT_MOD_BLOCK_SIZE_16x16 |
                              AFBC_FORMAT_MOD_TILED | AFBC_FORMAT_MOD_SPARSE),
      DRM_FORMAT_MOD_ARM_AFBC(AFBC_FORMAT_MOD_BLOCK_SIZE_32x8 |
                              AFBC_FORMAT_MOD_TILED | AFBC_FORMAT_MOD_SPARSE),
      DRM_FORMAT_MOD_ARM_AFBC(AFBC_FORMAT_MOD_BLOCK_SIZE_64x4 |
                              AFBC_FORMAT_MOD_TILED | AFBC_FORMAT_MOD_SPARSE),
   };

   for (unsigned m = 0; m < ARRAY_SIZE(modifiers); ++m) {
      uint64_t modifier = modifiers[m];

      uint32_t sw = pan_afbc_superblock_width(modifier);
      uint32_t cases[] = {1, 4, 17, 39};

      for (unsigned i = 0; i < ARRAY_SIZE(cases); ++i) {
         uint32_t width = sw * 8 * cases[i];

         EXPECT_EQ(pan_afbc_row_stride(modifier, width),
                   16 * DIV_ROUND_UP(width, (sw * 8)) * 8 * 8);

         EXPECT_EQ(pan_afbc_line_stride(modifier, width),
                   DIV_ROUND_UP(width, sw * 8) * 8);
      }
   }
}

static bool
layout_init(unsigned arch, const struct pan_image_props *props,
            unsigned plane_idx,
            const struct pan_image_layout_constraints *layout_constraints,
            struct pan_image_layout *layout)
{
   /* Pick the first supported arch if it's zero. */
   if (!arch)
      arch = 4;

   struct pan_image_plane plane = {
   };
   struct pan_image img = {
      .props = *props,
      .mod_handler = pan_mod_get_handler(arch, props->modifier),
   };

   img.planes[plane_idx] = &plane;

   if (!pan_image_layout_init(arch, &img, plane_idx, layout_constraints))
      return false;

   *layout = plane.layout;
   return true;
}

/* dEQP-GLES3.functional.texture.format.compressed.etc1_2d_pot */
TEST(Layout, ImplicitLayoutInterleavedETC2)
{
   struct pan_image_props p = {
      .modifier = DRM_FORMAT_MOD_ARM_16X16_BLOCK_U_INTERLEAVED,
      .format = PIPE_FORMAT_ETC2_RGB8,
      .extent_px = {
         .width = 128,
         .height = 128,
         .depth = 1,
      },
      .nr_samples = 1,
      .dim = MALI_TEXTURE_DIMENSION_2D,
      .nr_slices = 8,
   };
   struct pan_image_layout l = {};

   unsigned offsets[9] = {0,     8192,  10240, 10752, 10880,
                          11008, 11136, 11264, 11392};

   ASSERT_TRUE(layout_init(0, &p, 0, NULL, &l));

   for (unsigned i = 0; i < 8; ++i) {
      unsigned size = (offsets[i + 1] - offsets[i]);
      EXPECT_EQ(l.slices[i].offset_B, offsets[i]);

      if (size == 64)
         EXPECT_TRUE(l.slices[i].size_B < 64);
      else
         EXPECT_EQ(l.slices[i].size_B, size);
   }
}

TEST(Layout, ImplicitLayoutInterleavedASTC5x5)
{
   struct pan_image_props p = {
      .modifier = DRM_FORMAT_MOD_ARM_16X16_BLOCK_U_INTERLEAVED,
      .format = PIPE_FORMAT_ASTC_5x5,
      .extent_px = {
         .width = 50,
         .height = 50,
         .depth = 1,
      },
      .nr_samples = 1,
      .dim = MALI_TEXTURE_DIMENSION_2D,
      .nr_slices = 1,
   };
   struct pan_image_layout l = {};

   ASSERT_TRUE(layout_init(0, &p, 0, NULL, &l));

   /* The image is 50x50 pixels, with 5x5 blocks. So it is a 10x10 grid of ASTC
    * blocks. 4x4 tiles of ASTC blocks are u-interleaved, so we have to round up
    * to a 12x12 grid. So we need space for 144 ASTC blocks. Each ASTC block is
    * 16 bytes (128-bits), so we require 2304 bytes, with a row stride of 12 *
    * 16 * 4 = 192 bytes.
    */
   EXPECT_EQ(l.slices[0].offset_B, 0);
   EXPECT_EQ(l.slices[0].tiled_or_linear.row_stride_B, 768);
   EXPECT_EQ(l.slices[0].tiled_or_linear.surface_stride_B, 2304);
   EXPECT_EQ(l.slices[0].size_B, 2304);
}

TEST(Layout, ImplicitLayoutLinearASTC5x5)
{
   struct pan_image_props p = {
      .modifier = DRM_FORMAT_MOD_LINEAR,
      .format = PIPE_FORMAT_ASTC_5x5,
      .extent_px = {
         .width = 50,
         .height = 50,
         .depth = 1,
      },
      .nr_samples = 1,
      .dim = MALI_TEXTURE_DIMENSION_2D,
      .nr_slices = 1,
   };
   struct pan_image_layout l = {};

   ASSERT_TRUE(layout_init(0, &p, 0, NULL, &l));

   /* The image is 50x50 pixels, with 5x5 blocks. So it is a 10x10 grid of ASTC
    * blocks. Each ASTC block is 16 bytes, so the row stride is 160 bytes,
    * rounded up to the cache line (192 bytes).  There are 10 rows, so we have
    * 1920 bytes total.
    */
   EXPECT_EQ(l.slices[0].offset_B, 0);
   EXPECT_EQ(l.slices[0].tiled_or_linear.row_stride_B, 192);
   EXPECT_EQ(l.slices[0].tiled_or_linear.surface_stride_B, 1920);
   EXPECT_EQ(l.slices[0].size_B, 1920);
}

/* dEQP-GLES3.functional.texture.format.unsized.rgba_unsigned_byte_3d_pot */
TEST(AFBCLayout, Linear3D)
{
   uint64_t modifier = DRM_FORMAT_MOD_ARM_AFBC(
      AFBC_FORMAT_MOD_BLOCK_SIZE_16x16 | AFBC_FORMAT_MOD_SPARSE);

   struct pan_image_props p = {
      .modifier = modifier,
      .format = PIPE_FORMAT_R8G8B8A8_UNORM,
      .extent_px = {
         .width = 8,
         .height = 32,
         .depth = 16,
      },
      .nr_samples = 1,
      .dim = MALI_TEXTURE_DIMENSION_3D,
      .nr_slices = 1,
   };
   struct pan_image_layout l = {};

   ASSERT_TRUE(layout_init(0, &p, 0, NULL, &l));

   /* AFBC Surface size is the size of headers for a single surface. At superblock
    * size 16x16, the 8x32 layer has 1x2 superblocks, so the header size is 2 *
    * 16 = 32 bytes. Body offset needs to be aligned on 64 bytes on v6-.
    * Header/body sections of a 3D image are interleaved, so the surface stride is
    * is the header size, aligned to meet body offset alignment constraints, plus
    * the body of a single surface.
    *
    * There is only 1 superblock per row, so the row stride is the bytes per 1
    * header block = 16.
    *
    * There are 16 layers of size 64 so afbc.header_size = 16 * 64 = 1024.
    *
    * Each 16x16 superblock consumes 16 * 16 * 4 = 1024 bytes. There are 2 * 1 *
    * 16 superblocks in the image, so body size is 32768.
    */
   EXPECT_EQ(l.slices[0].offset_B, 0);
   EXPECT_EQ(l.slices[0].afbc.header.row_stride_B, 16);
   EXPECT_EQ(l.slices[0].afbc.header.surface_size_B, 32);
   EXPECT_EQ(l.slices[0].afbc.surface_stride_B, 64 + 2048);
   EXPECT_EQ(l.slices[0].size_B, (64 + 2048) * 16);
}

TEST(AFBCLayout, Tiled16x16)
{
   uint64_t modifier =
      DRM_FORMAT_MOD_ARM_AFBC(AFBC_FORMAT_MOD_BLOCK_SIZE_16x16 |
                              AFBC_FORMAT_MOD_TILED | AFBC_FORMAT_MOD_SPARSE);

   struct pan_image_props p = {
      .modifier = modifier,
      .format = PIPE_FORMAT_R8G8B8A8_UNORM,
      .extent_px = {
         .width = 917,
         .height = 417,
         .depth = 1,
      },
      .nr_samples = 1,
      .dim = MALI_TEXTURE_DIMENSION_2D,
      .nr_slices = 1,
   };
   struct pan_image_layout l = {};

   ASSERT_TRUE(layout_init(0, &p, 0, NULL, &l));

   /* The image is 917x417. Superblocks are 16x16, so there are 58x27
    * superblocks. Superblocks are grouped into 8x8 tiles, so there are 8x4
    * tiles of superblocks. So the row stride is 16 * 8 * 8 * 8 = 8192 bytes.
    * There are 4 tiles vertically, so the header is 8192 * 4 = 32768 bytes.
    * This is already 4096-byte aligned.
    *
    * Each tile of superblock contains 128x128 pixels and each pixel is 4 bytes,
    * so tiles are 65536 bytes, meaning the payload is 8 * 4 * 65536 = 2097152
    * bytes.
    *
    * In total, the AFBC surface is 32768 + 2097152 = 2129920 bytes.
    */
   EXPECT_EQ(l.slices[0].offset_B, 0);
   EXPECT_EQ(l.slices[0].afbc.header.row_stride_B, 8192);
   EXPECT_EQ(l.slices[0].afbc.header.surface_size_B, 32768);
   EXPECT_EQ(l.slices[0].afbc.surface_stride_B, 2129920);
   EXPECT_EQ(l.slices[0].size_B, 2129920);
}

TEST(AFBCLayout, Linear16x16Minimal)
{
   uint64_t modifier = DRM_FORMAT_MOD_ARM_AFBC(
      AFBC_FORMAT_MOD_BLOCK_SIZE_16x16 | AFBC_FORMAT_MOD_SPARSE);

   struct pan_image_props p = {
      .modifier = modifier,
      .format = PIPE_FORMAT_R8_UNORM,
      .extent_px = {
         .width = 1,
         .height = 1,
         .depth = 1,
      },
      .nr_samples = 1,
      .dim = MALI_TEXTURE_DIMENSION_2D,
      .nr_slices = 1,
   };
   struct pan_image_layout l = {};

   ASSERT_TRUE(layout_init(0, &p, 0, NULL, &l));

   /* Image is 1x1 to test for correct alignment everywhere. */
   EXPECT_EQ(l.slices[0].offset_B, 0);
   EXPECT_EQ(l.slices[0].afbc.header.row_stride_B, 16);
   EXPECT_EQ(l.slices[0].afbc.header.surface_size_B, 16);
   EXPECT_EQ(l.slices[0].afbc.surface_stride_B, 64 + (32 * 8));
   EXPECT_EQ(l.slices[0].size_B, 64 + (32 * 8));
}

TEST(AFBCLayout, Linear16x16Minimalv6)
{
   uint64_t modifier = DRM_FORMAT_MOD_ARM_AFBC(
      AFBC_FORMAT_MOD_BLOCK_SIZE_16x16 | AFBC_FORMAT_MOD_SPARSE);

   struct pan_image_props p = {
      .modifier = modifier,
      .format = PIPE_FORMAT_R8_UNORM,
      .extent_px = {
         .width = 1,
         .height = 1,
         .depth = 1,
      },
      .nr_samples = 1,
      .dim = MALI_TEXTURE_DIMENSION_2D,
      .nr_slices = 1,
   };
   struct pan_image_layout l = {};

   ASSERT_TRUE(layout_init(6, &p, 0, NULL, &l));

   /* Image is 1x1 to test for correct alignment everywhere. */
   EXPECT_EQ(l.slices[0].offset_B, 0);
   EXPECT_EQ(l.slices[0].afbc.header.row_stride_B, 16);
   EXPECT_EQ(l.slices[0].afbc.header.surface_size_B, 16);
   EXPECT_EQ(l.slices[0].afbc.surface_stride_B, 128 + (32 * 8));
   EXPECT_EQ(l.slices[0].size_B, 128 + (32 * 8));
}

TEST(AFBCLayout, Tiled16x16Minimal)
{
   uint64_t modifier =
      DRM_FORMAT_MOD_ARM_AFBC(AFBC_FORMAT_MOD_BLOCK_SIZE_16x16 |
                              AFBC_FORMAT_MOD_TILED | AFBC_FORMAT_MOD_SPARSE);

   struct pan_image_props p = {
      .modifier = modifier,
      .format = PIPE_FORMAT_R8_UNORM,
      .extent_px = {
         .width = 1,
         .height = 1,
         .depth = 1,
      },
      .nr_samples = 1,
      .dim = MALI_TEXTURE_DIMENSION_2D,
      .nr_slices = 1,
   };
   struct pan_image_layout l = {};

   ASSERT_TRUE(layout_init(0, &p, 0, NULL, &l));

   /* Image is 1x1 to test for correct alignment everywhere. */
   EXPECT_EQ(l.slices[0].offset_B, 0);
   EXPECT_EQ(l.slices[0].afbc.header.row_stride_B, 16 * 8 * 8);
   EXPECT_EQ(l.slices[0].afbc.header.surface_size_B, 16 * 8 * 8);
   EXPECT_EQ(l.slices[0].afbc.surface_stride_B, 4096 + (32 * 8 * 8 * 8));
   EXPECT_EQ(l.slices[0].size_B, 4096 + (32 * 8 * 8 * 8));
}

static unsigned archs[] = {4, 5, 6, 7, 9, 12, 13};

#define IMAGE_WIDTH  4096
#define IMAGE_HEIGHT 512
#define IMAGE_FORMAT                                                           \
   (PAN_BIND_DEPTH_STENCIL | PAN_BIND_RENDER_TARGET | PAN_BIND_SAMPLER_VIEW |  \
    PAN_BIND_STORAGE_IMAGE)

#define EXPECT_IMPORT_SUCCESS(__arch, __iprops, __plane, __wsi_layout,         \
                              __out_layout, __test_desc)                       \
   do {                                                                        \
      bool __result =                                                          \
         layout_init(__arch, __iprops, __plane, __wsi_layout, __out_layout);   \
      EXPECT_TRUE(__result)                                                    \
         << __test_desc                                                        \
         << " for <format=" << util_format_name((__iprops)->format)            \
         << ",plane=" << __plane << ",mod=" << std::hex                        \
         << (__iprops)->modifier << std::dec << "> rejected (arch=" << __arch  \
         << ")";                                                               \
                                                                               \
      if (!__result)                                                           \
         break;                                                                \
                                                                               \
      struct pan_image_plane img_plane = {                                     \
         .layout = *(__out_layout),                                            \
      };                                                                       \
      struct pan_image img = {                                                 \
         .props = *(__iprops),                                                 \
         .mod_handler = pan_mod_get_handler(__arch, (__iprops)->modifier),     \
      };                                                                       \
      img.planes[__plane] = &img_plane;                                        \
      unsigned __export_row_pitch_B =                                          \
         pan_image_get_wsi_row_pitch(&img, __plane, 0);                        \
      unsigned __export_offset_B = pan_image_get_wsi_offset(&img, __plane, 0); \
      EXPECT_TRUE(__export_row_pitch_B == (__wsi_layout)->wsi_row_pitch_B &&   \
                  __export_offset_B == (__wsi_layout)->offset_B)               \
         << " mismatch between import and export for <format="                 \
         << util_format_name(iprops.format) << ",plane=" << __plane            \
         << ",mod=" << std::hex << (__iprops)->modifier << std::dec            \
         << "> (arch=" << __arch << ")";                                       \
   } while (0)

#define EXPECT_IMPORT_FAIL(__arch, __iprops, __plane, __wsi_layout,            \
                           __out_layout, __test_desc)                          \
   EXPECT_FALSE(                                                               \
      layout_init(__arch, __iprops, __plane, __wsi_layout, __out_layout))      \
      << __test_desc                                                           \
      << " for <format=" << util_format_name((__iprops)->format)               \
      << ",plane=" << __plane << ",mod=" << std::hex << (__iprops)->modifier   \
      << std::dec << "> not rejected (arch=" << __arch << ")"

static bool
format_can_do_mod(unsigned arch, enum pipe_format format, unsigned plane_idx,
                  uint64_t modifier)
{
   if (drm_is_afbc(modifier)) {
      return pan_afbc_format(arch, format, plane_idx) != PAN_AFBC_MODE_INVALID;
   } else if (drm_is_afrc(modifier)) {
      return arch >= 10 && pan_afrc_supports_format(format);
   } else {
      assert(modifier == DRM_FORMAT_MOD_ARM_16X16_BLOCK_U_INTERLEAVED ||
             modifier == DRM_FORMAT_MOD_LINEAR);

      switch (format) {
      case PIPE_FORMAT_R8G8B8_420_UNORM_PACKED:
      case PIPE_FORMAT_R10G10B10_420_UNORM_PACKED:
         /* Those are only supported with AFBC. */
         return false;

      default:
         return true;
      }
   }
}

static unsigned
offset_align_for_mod(unsigned arch, const struct pan_image_props *iprops,
                     unsigned plane_idx)
{
   uint64_t modifier = iprops->modifier;
   enum pipe_format format = iprops->format;

   if (drm_is_afbc(modifier)) {
      return pan_afbc_header_align(arch, modifier);
   } else if (drm_is_afrc(modifier)) {
      return pan_afrc_buffer_alignment_from_modifier(modifier);
   } else {
      assert(modifier == DRM_FORMAT_MOD_ARM_16X16_BLOCK_U_INTERLEAVED ||
             modifier == DRM_FORMAT_MOD_LINEAR);

      return pan_linear_or_tiled_row_align_req(arch, format, plane_idx);
   }
}

static unsigned
row_align_for_mod(unsigned arch, const struct pan_image_props *iprops,
                  unsigned plane_idx)
{
   uint64_t modifier = iprops->modifier;
   enum pipe_format format = iprops->format;

   if (drm_is_afbc(modifier)) {
      unsigned hdr_row_align =
         pan_afbc_header_row_stride_align(arch, format, modifier);
      unsigned ntiles = hdr_row_align / AFBC_HEADER_BYTES_PER_TILE;
      unsigned sb_width_el = pan_afbc_superblock_width(modifier) /
                             util_format_get_blockwidth(format);

      assert(pan_afbc_superblock_width(modifier) %
                util_format_get_blockwidth(format) ==
             0);
      return ntiles * sb_width_el *
             pan_format_get_plane_blocksize(format, plane_idx);
   } else if (drm_is_afrc(modifier)) {
      unsigned row_align = pan_afrc_buffer_alignment_from_modifier(modifier);
      struct pan_image_block_size tile_size_px =
         pan_afrc_tile_size(format, modifier);

      assert(row_align % tile_size_px.height == 0);
      return row_align / tile_size_px.height;
   } else {
      assert(modifier == DRM_FORMAT_MOD_ARM_16X16_BLOCK_U_INTERLEAVED ||
             modifier == DRM_FORMAT_MOD_LINEAR);

      unsigned tile_height_el = modifier == DRM_FORMAT_MOD_LINEAR   ? 1
                                : util_format_is_compressed(format) ? 4
                                                                    : 16;

      return DIV_ROUND_UP(offset_align_for_mod(arch, iprops, plane_idx),
                          tile_height_el);
   }
}

static unsigned
default_wsi_row_pitch(unsigned arch, const struct pan_image_props *iprops,
                      unsigned plane_idx)
{
   uint64_t modifier = iprops->modifier;
   enum pipe_format format = iprops->format;
   unsigned fmt_blksz_B = pan_format_get_plane_blocksize(format, plane_idx);
   unsigned width_px =
      util_format_get_plane_width(format, plane_idx, iprops->extent_px.width);

   assert(width_px % util_format_get_blockwidth(format) == 0);

   if (drm_is_afbc(modifier)) {
      unsigned sb_width_el = pan_afbc_superblock_width(modifier) /
                             util_format_get_blockwidth(format);
      unsigned sb_height_el = pan_afbc_superblock_height(modifier) /
                              util_format_get_blockheight(format);
      unsigned ntiles =
         DIV_ROUND_UP(width_px, pan_afbc_superblock_width(modifier));
      unsigned tile_row_size_B =
         sb_width_el * sb_height_el * fmt_blksz_B * ntiles;

      assert(pan_afbc_superblock_width(modifier) %
                util_format_get_blockwidth(format) ==
             0);
      assert(pan_afbc_superblock_height(modifier) %
                util_format_get_blockheight(format) ==
             0);
      assert(tile_row_size_B % pan_afbc_superblock_height(modifier) == 0);
      return tile_row_size_B / pan_afbc_superblock_height(modifier);
   } else if (drm_is_afrc(modifier)) {
      struct pan_image_block_size tile_size =
         pan_afrc_tile_size(format, modifier);
      unsigned afrc_row_stride_B =
         pan_afrc_row_stride(format, modifier, width_px);

      assert(afrc_row_stride_B % tile_size.height == 0);

      return afrc_row_stride_B / tile_size.height;
   } else {
      assert(modifier == DRM_FORMAT_MOD_LINEAR ||
             modifier == DRM_FORMAT_MOD_ARM_16X16_BLOCK_U_INTERLEAVED);

      unsigned row_pitch_B =
         (width_px / util_format_get_blockwidth(format)) * fmt_blksz_B;
      struct pan_image_block_size tile_size_el = {1, 1};

      if (modifier == DRM_FORMAT_MOD_ARM_16X16_BLOCK_U_INTERLEAVED) {
         if (util_format_is_compressed(format)) {
            tile_size_el.width = 4;
            tile_size_el.height = 4;
         } else {
            tile_size_el.width = 16;
            tile_size_el.height = 16;
         }
      }

      assert(width_px %
                (util_format_get_blockwidth(format) * tile_size_el.width) ==
             0);

      return row_pitch_B;
   }
}

TEST(WSI, Import)
{
   /* We don't want to spam stderr with failure messages caused by our
    * EXPECT_FALSE() cases. */
   setenv("MESA_LOG", "null", 0);

   struct pan_image_layout layout;
   for (unsigned i = 0; i < ARRAY_SIZE(archs); i++) {
      unsigned arch = archs[i];
      const struct pan_format *ftable = pan_format_table(arch);
      PAN_SUPPORTED_MODIFIERS(mods);

      for (unsigned m = 0; m < ARRAY_SIZE(mods); m++) {
         for (unsigned fmt = PIPE_FORMAT_NONE + 1; fmt < PIPE_FORMAT_COUNT;
              fmt++) {
            if (!(ftable[fmt].bind & IMAGE_FORMAT))
               continue;


            struct pan_image_props iprops = {
               .modifier = mods[m],
               .format = (enum pipe_format)fmt,
               .extent_px = {
                  .width = IMAGE_WIDTH,
                  .height = IMAGE_HEIGHT,
                  .depth = 1,
               },
               .nr_samples = 1,
               .dim = MALI_TEXTURE_DIMENSION_2D,
               .nr_slices = 1,
               .array_size = 1,
               .crc = false,
            };

            bool supported = true;
            for (unsigned p = 0; p < util_format_get_num_planes(iprops.format);
                 p++) {
               if (!format_can_do_mod(arch, iprops.format, p,
                                      iprops.modifier)) {
                  supported = false;
                  break;
               }
            }

            if (!supported)
               continue;

            if (util_format_is_compressed(iprops.format)) {
               /* We multiply the image extent by the block extent to make sure
                * things are always aligned on a block. */
               iprops.extent_px.width *=
                  util_format_get_blockwidth(iprops.format);
               iprops.extent_px.height *=
                  util_format_get_blockheight(iprops.format);
            }

            for (unsigned p = 0; p < util_format_get_num_planes(iprops.format);
                 p++) {
               unsigned row_align_req_B =
                  row_align_for_mod(arch, &iprops, p);
               unsigned offset_align_req_B =
                  offset_align_for_mod(arch, &iprops, p);
               unsigned default_row_pitch_B =
                  default_wsi_row_pitch(arch, &iprops, p);

               assert(default_row_pitch_B > row_align_req_B);

               if (row_align_req_B > 1) {
                  struct pan_image_layout_constraints wsi_layout = {
                     .wsi_row_pitch_B = default_row_pitch_B + 1,
                     .strict = true,
                  };

                  EXPECT_IMPORT_FAIL(arch, &iprops, p, &wsi_layout, &layout,
                                     "unaligned WSI row pitch");
               }

               if (offset_align_req_B > 1) {
                  struct pan_image_layout_constraints wsi_layout = {
                     .offset_B = 1,
                     .wsi_row_pitch_B = default_row_pitch_B,
                     .strict = true,
                  };

                  EXPECT_IMPORT_FAIL(arch, &iprops, p, &wsi_layout, &layout,
                                     "unaligned WSI offset");
               }

               /* Exact match. */
               struct pan_image_layout_constraints wsi_layout = {
                  .wsi_row_pitch_B = default_row_pitch_B,
                  .strict = true,
               };

               EXPECT_IMPORT_SUCCESS(arch, &iprops, p, &wsi_layout, &layout,
                                     "tightly packed lines");

               wsi_layout.wsi_row_pitch_B =
                  default_row_pitch_B + row_align_req_B;
               EXPECT_IMPORT_SUCCESS(arch, &iprops, p, &wsi_layout, &layout,
                                     "lines with padding");

               wsi_layout.wsi_row_pitch_B =
                  default_row_pitch_B - row_align_req_B;
               EXPECT_IMPORT_FAIL(arch, &iprops, p, &wsi_layout, &layout,
                                  "partially aliased lines");

               wsi_layout.wsi_row_pitch_B = default_row_pitch_B;
               wsi_layout.offset_B = offset_align_req_B;
               EXPECT_IMPORT_SUCCESS(arch, &iprops, p, &wsi_layout, &layout,
                                     "properly aligned offset");
            }
         }
      }
   }
}
