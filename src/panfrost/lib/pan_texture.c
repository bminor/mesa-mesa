/*
 * Copyright (C) 2008 VMware, Inc.
 * Copyright (C) 2014 Broadcom
 * Copyright (C) 2018-2019 Alyssa Rosenzweig
 * Copyright (C) 2019-2020 Collabora, Ltd.
 * Copyright (C) 2024 Arm Ltd.
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
 *
 */

#include "pan_texture.h"
#include "util/macros.h"
#include "util/u_math.h"
#include "pan_afbc.h"
#include "pan_afrc.h"
#include "pan_desc.h"
#include "pan_format.h"
#include "pan_image.h"
#include "pan_pool.h"
#include "pan_util.h"

#if PAN_ARCH >= 5
/*
 * Arm Scalable Texture Compression (ASTC) corresponds to just a few formats.
 * The block dimension is not part of the format. Instead, it is encoded as a
 * 6-bit tag on the payload pointer. Map the block size for a single dimension.
 */
static inline enum mali_astc_2d_dimension
pan_astc_dim_2d(unsigned dim)
{
   switch (dim) {
   case 4:
      return MALI_ASTC_2D_DIMENSION_4;
   case 5:
      return MALI_ASTC_2D_DIMENSION_5;
   case 6:
      return MALI_ASTC_2D_DIMENSION_6;
   case 8:
      return MALI_ASTC_2D_DIMENSION_8;
   case 10:
      return MALI_ASTC_2D_DIMENSION_10;
   case 12:
      return MALI_ASTC_2D_DIMENSION_12;
   default:
      unreachable("Invalid ASTC dimension");
   }
}

static inline enum mali_astc_3d_dimension
pan_astc_dim_3d(unsigned dim)
{
   switch (dim) {
   case 3:
      return MALI_ASTC_3D_DIMENSION_3;
   case 4:
      return MALI_ASTC_3D_DIMENSION_4;
   case 5:
      return MALI_ASTC_3D_DIMENSION_5;
   case 6:
      return MALI_ASTC_3D_DIMENSION_6;
   default:
      unreachable("Invalid ASTC dimension");
   }
}
#endif

/* Texture addresses are tagged with information about compressed formats.
 * AFBC uses a bit for whether the colorspace transform is enabled (RGB and
 * RGBA only).
 * For ASTC, this is a "stretch factor" encoding the block size. */

static unsigned
pan_compression_tag(const struct util_format_description *desc,
                    enum mali_texture_dimension dim, uint64_t modifier)
{
#if PAN_ARCH >= 5 && PAN_ARCH <= 8
   if (drm_is_afbc(modifier)) {
      unsigned flags =
         (modifier & AFBC_FORMAT_MOD_YTR) ? MALI_AFBC_SURFACE_FLAG_YTR : 0;

#if PAN_ARCH >= 6
      /* Prefetch enable */
      flags |= MALI_AFBC_SURFACE_FLAG_PREFETCH;

      if (pan_afbc_is_wide(modifier))
         flags |= MALI_AFBC_SURFACE_FLAG_WIDE_BLOCK;

      if (modifier & AFBC_FORMAT_MOD_SPLIT)
         flags |= MALI_AFBC_SURFACE_FLAG_SPLIT_BLOCK;
#endif

#if PAN_ARCH >= 7
      /* Tiled headers */
      if (modifier & AFBC_FORMAT_MOD_TILED)
         flags |= MALI_AFBC_SURFACE_FLAG_TILED_HEADER;

      /* Used to make sure AFBC headers don't point outside the AFBC
       * body. HW is using the AFBC surface stride to do this check,
       * which doesn't work for 3D textures because the surface
       * stride does not cover the body. Only supported on v7+.
       */
      if (dim != MALI_TEXTURE_DIMENSION_3D)
         flags |= MALI_AFBC_SURFACE_FLAG_CHECK_PAYLOAD_RANGE;
#endif

      return flags;
   } else if (desc->layout == UTIL_FORMAT_LAYOUT_ASTC) {
      if (desc->block.depth > 1) {
         return (pan_astc_dim_3d(desc->block.depth) << 4) |
                (pan_astc_dim_3d(desc->block.height) << 2) |
                pan_astc_dim_3d(desc->block.width);
      } else {
         return (pan_astc_dim_2d(desc->block.height) << 3) |
                pan_astc_dim_2d(desc->block.width);
      }
   }
#endif

   /* Tags are not otherwise used */
   return 0;
}

/* Following the texture descriptor is a number of descriptors. How many? */

static unsigned
pan_texture_num_elements(const struct pan_image_view *iview)
{
   unsigned levels = 1 + iview->last_level - iview->first_level;
   unsigned layers = 1 + iview->last_layer - iview->first_layer;
   unsigned nr_samples = pan_image_view_get_nr_samples(iview);

   return levels * layers * MAX2(nr_samples, 1);
}

/* Conservative estimate of the size of the texture payload a priori.
 * Average case, size equal to the actual size. Worst case, off by 2x (if
 * a manual stride is not needed on a linear texture). Returned value
 * must be greater than or equal to the actual size, so it's safe to use
 * as an allocation amount */

unsigned
GENX(pan_texture_estimate_payload_size)(const struct pan_image_view *iview)
{
   size_t element_size;

#if PAN_ARCH >= 9
   element_size = pan_size(PLANE);

   /* 2-plane and 3-plane YUV use two plane descriptors. */
   if (pan_format_is_yuv(iview->format) && iview->planes[1].image != NULL)
      element_size *= 2;
#elif PAN_ARCH == 7
   if (pan_format_is_yuv(iview->format))
      element_size = pan_size(MULTIPLANAR_SURFACE);
   else
      element_size = pan_size(SURFACE_WITH_STRIDE);
#else
   /* Assume worst case. Overestimates on Midgard, but that's ok. */
   element_size = pan_size(SURFACE_WITH_STRIDE);
#endif

   unsigned elements = pan_texture_num_elements(iview);

   return element_size * elements;
}

static void
pan_get_surface_strides(const struct pan_image_props *props,
                        const struct pan_image_layout *layout, unsigned l,
                        int32_t *row_stride_B, int32_t *surf_stride_B)
{
   const struct pan_image_slice_layout *slice = &layout->slices[l];

   if (drm_is_afbc(props->modifier)) {
      /* Pre v7 don't have a row stride field. This field is
       * repurposed as a Y offset which we don't use */
      *row_stride_B = PAN_ARCH < 7 ? 0 : slice->row_stride_B;
      *surf_stride_B = slice->afbc.surface_stride_B;
   } else {
      *row_stride_B = slice->row_stride_B;
      *surf_stride_B = slice->surface_stride_B;
   }
}

static uint64_t
pan_get_surface_pointer(const struct pan_image_props *props,
                        const struct pan_image_layout *layout,
                        enum mali_texture_dimension dim, uint64_t base,
                        unsigned l, unsigned i, unsigned s)
{
   unsigned offset;

   if (props->dim == MALI_TEXTURE_DIMENSION_3D) {
      assert(!s);
      offset = layout->slices[l].offset_B +
               i * pan_image_surface_stride(props, layout, l);
   } else {
      offset = pan_image_surface_offset(layout, l, i, s);
   }

   return base + offset;
}

struct pan_image_section_info {
   uint64_t pointer;
   int32_t row_stride;
   int32_t surface_stride;
};

static struct pan_image_section_info
get_image_section_info(const struct pan_image_view *iview,
                       const struct pan_image_plane_ref pref, unsigned level,
                       unsigned index, unsigned sample)
{
   const struct util_format_description *desc =
      util_format_description(iview->format);
   const struct pan_image *image = pref.image;
   const struct pan_image_plane *plane = image->planes[pref.plane_idx];
   uint64_t base = plane->base;
   struct pan_image_section_info info = {0};

   /* v4 does not support compression */
   assert(PAN_ARCH >= 5 || !drm_is_afbc(image->props.modifier));
   assert(PAN_ARCH >= 5 || desc->layout != UTIL_FORMAT_LAYOUT_ASTC);

   /* pan_compression_tag() wants the dimension of the resource, not the
    * one of the image view (those might differ).
    */
   unsigned tag =
      pan_compression_tag(desc, image->props.dim, image->props.modifier);

   info.pointer =
      pan_get_surface_pointer(&image->props, &plane->layout, iview->dim,
                              base | tag, level, index, sample);
   pan_get_surface_strides(&image->props, &plane->layout, level,
                           &info.row_stride, &info.surface_stride);

   return info;
}

#if PAN_ARCH < 9
static void
pan_emit_bview_surface_with_stride(const struct pan_buffer_view *bview,
                                   void *payload)
{
   const struct util_format_description *desc =
      util_format_description(bview->format);
   unsigned tag = pan_compression_tag(desc, MALI_TEXTURE_DIMENSION_1D,
                                      DRM_FORMAT_MOD_LINEAR);

   pan_cast_and_pack(payload, SURFACE_WITH_STRIDE, cfg) {
      cfg.pointer = bview->base | tag;
      cfg.row_stride = 0;
      cfg.surface_stride = 0;
   }
}

static void
pan_emit_iview_surface_with_stride(const struct pan_image_section_info *section,
                                   void **payload)
{
   pan_cast_and_pack(*payload, SURFACE_WITH_STRIDE, cfg) {
      cfg.pointer = section->pointer;
      cfg.row_stride = section->row_stride;
      cfg.surface_stride = section->surface_stride;
   }
   *payload += pan_size(SURFACE_WITH_STRIDE);
}
#endif

#if PAN_ARCH == 7
static void
pan_emit_iview_multiplanar_surface(
   const struct pan_image_section_info *sections, void **payload)
{
   assert(sections[2].row_stride == 0 ||
          sections[1].row_stride == sections[2].row_stride);

   pan_cast_and_pack(*payload, MULTIPLANAR_SURFACE, cfg) {
      cfg.plane_0_pointer = sections[0].pointer;
      cfg.plane_0_row_stride = sections[0].row_stride;
      cfg.plane_1_2_row_stride = sections[1].row_stride;
      cfg.plane_1_pointer = sections[1].pointer;
      cfg.plane_2_pointer = sections[2].pointer;
   }
   *payload += pan_size(MULTIPLANAR_SURFACE);
}
#endif

#if PAN_ARCH >= 9

/* clang-format off */
#define CLUMP_FMT(pipe, mali) [PIPE_FORMAT_ ## pipe] = MALI_CLUMP_FORMAT_ ## mali
static enum mali_clump_format special_clump_formats[PIPE_FORMAT_COUNT] = {
   CLUMP_FMT(X32_S8X24_UINT,  X32S8X24),
   CLUMP_FMT(X24S8_UINT,      X24S8),
   CLUMP_FMT(S8X24_UINT,      S8X24),
   CLUMP_FMT(S8_UINT,         S8),
   CLUMP_FMT(L4A4_UNORM,      L4A4),
   CLUMP_FMT(L8A8_UNORM,      L8A8),
   CLUMP_FMT(L8A8_UINT,       L8A8),
   CLUMP_FMT(L8A8_SINT,       L8A8),
   CLUMP_FMT(A8_UNORM,        A8),
   CLUMP_FMT(A8_UINT,         A8),
   CLUMP_FMT(A8_SINT,         A8),
   CLUMP_FMT(ETC1_RGB8,       ETC2_RGB8),
   CLUMP_FMT(ETC2_RGB8,       ETC2_RGB8),
   CLUMP_FMT(ETC2_SRGB8,      ETC2_RGB8),
   CLUMP_FMT(ETC2_RGB8A1,     ETC2_RGB8A1),
   CLUMP_FMT(ETC2_SRGB8A1,    ETC2_RGB8A1),
   CLUMP_FMT(ETC2_RGBA8,      ETC2_RGBA8),
   CLUMP_FMT(ETC2_SRGBA8,     ETC2_RGBA8),
   CLUMP_FMT(ETC2_R11_UNORM,  ETC2_R11_UNORM),
   CLUMP_FMT(ETC2_R11_SNORM,  ETC2_R11_SNORM),
   CLUMP_FMT(ETC2_RG11_UNORM, ETC2_RG11_UNORM),
   CLUMP_FMT(ETC2_RG11_SNORM, ETC2_RG11_SNORM),
   CLUMP_FMT(DXT1_RGB,        BC1_UNORM),
   CLUMP_FMT(DXT1_RGBA,       BC1_UNORM),
   CLUMP_FMT(DXT1_SRGB,       BC1_UNORM),
   CLUMP_FMT(DXT1_SRGBA,      BC1_UNORM),
   CLUMP_FMT(DXT3_RGBA,       BC2_UNORM),
   CLUMP_FMT(DXT3_SRGBA,      BC2_UNORM),
   CLUMP_FMT(DXT5_RGBA,       BC3_UNORM),
   CLUMP_FMT(DXT5_SRGBA,      BC3_UNORM),
   CLUMP_FMT(RGTC1_UNORM,     BC4_UNORM),
   CLUMP_FMT(RGTC1_SNORM,     BC4_SNORM),
   CLUMP_FMT(RGTC2_UNORM,     BC5_UNORM),
   CLUMP_FMT(RGTC2_SNORM,     BC5_SNORM),
   CLUMP_FMT(BPTC_RGB_FLOAT,  BC6H_SF16),
   CLUMP_FMT(BPTC_RGB_UFLOAT, BC6H_UF16),
   CLUMP_FMT(BPTC_RGBA_UNORM, BC7_UNORM),
   CLUMP_FMT(BPTC_SRGBA,      BC7_UNORM),
};
#undef CLUMP_FMT
/* clang-format on */

static enum mali_clump_format
pan_clump_format(enum pipe_format format)
{
   /* First, try a special clump format. Note that the 0 encoding is for a
    * raw clump format, which will never be in the special table.
    */
   if (special_clump_formats[format])
      return special_clump_formats[format];

   /* Else, it's a raw format. Raw formats must not be compressed. */
   assert(!util_format_is_compressed(format));

   /* YUV-sampling has special cases */
   if (pan_format_is_yuv(format)) {
      switch (format) {
      case PIPE_FORMAT_R8G8_R8B8_UNORM:
      case PIPE_FORMAT_G8R8_B8R8_UNORM:
      case PIPE_FORMAT_R8B8_R8G8_UNORM:
      case PIPE_FORMAT_B8R8_G8R8_UNORM:
      case PIPE_FORMAT_R8_G8B8_422_UNORM:
      case PIPE_FORMAT_R8_B8G8_422_UNORM:
         return MALI_CLUMP_FORMAT_Y8_UV8_422;
      case PIPE_FORMAT_R8_G8B8_420_UNORM:
      case PIPE_FORMAT_R8_B8G8_420_UNORM:
      case PIPE_FORMAT_R8_G8_B8_420_UNORM:
      case PIPE_FORMAT_R8_B8_G8_420_UNORM:
         return MALI_CLUMP_FORMAT_Y8_UV8_420;
      case PIPE_FORMAT_R10_G10B10_420_UNORM:
         return MALI_CLUMP_FORMAT_Y10_UV10_420;
      case PIPE_FORMAT_R10_G10B10_422_UNORM:
         return MALI_CLUMP_FORMAT_Y10_UV10_422;
      default:
         unreachable("unhandled clump format");
      }
   }

   /* Select the appropriate raw format. */
   switch (util_format_get_blocksize(format)) {
   case 1:
      return MALI_CLUMP_FORMAT_RAW8;
   case 2:
      return MALI_CLUMP_FORMAT_RAW16;
   case 3:
      return MALI_CLUMP_FORMAT_RAW24;
   case 4:
      return MALI_CLUMP_FORMAT_RAW32;
   case 6:
      return MALI_CLUMP_FORMAT_RAW48;
   case 8:
      return MALI_CLUMP_FORMAT_RAW64;
   case 12:
      return MALI_CLUMP_FORMAT_RAW96;
   case 16:
      return MALI_CLUMP_FORMAT_RAW128;
   default:
      unreachable("Invalid bpp");
   }
}

static enum mali_afbc_superblock_size
translate_superblock_size(uint64_t modifier)
{
   assert(drm_is_afbc(modifier));

   switch (modifier & AFBC_FORMAT_MOD_BLOCK_SIZE_MASK) {
   case AFBC_FORMAT_MOD_BLOCK_SIZE_16x16:
      return MALI_AFBC_SUPERBLOCK_SIZE_16X16;
   case AFBC_FORMAT_MOD_BLOCK_SIZE_32x8:
      return MALI_AFBC_SUPERBLOCK_SIZE_32X8;
   case AFBC_FORMAT_MOD_BLOCK_SIZE_64x4:
      return MALI_AFBC_SUPERBLOCK_SIZE_64X4;
   default:
      unreachable("Invalid superblock size");
   }
}

static void
pan_emit_bview_plane(const struct pan_buffer_view *bview, void *payload)
{
   const struct util_format_description *desc =
      util_format_description(bview->format);

   pan_cast_and_pack(payload, PLANE, cfg) {
      cfg.pointer = bview->base;
      cfg.size = util_format_get_blocksize(bview->format) * bview->width_el;
      cfg.clump_ordering = MALI_CLUMP_ORDERING_LINEAR;
#if PAN_ARCH >= 10
      cfg.width = bview->width_el;
      cfg.height = 1;
#endif

      if (desc->layout == UTIL_FORMAT_LAYOUT_ASTC) {
         if (desc->block.depth > 1) {
            cfg.plane_type = MALI_PLANE_TYPE_ASTC_3D;
            cfg.astc._3d.block_width = pan_astc_dim_3d(desc->block.width);
            cfg.astc._3d.block_height = pan_astc_dim_3d(desc->block.height);
            cfg.astc._3d.block_depth = pan_astc_dim_3d(desc->block.depth);
         } else {
            cfg.plane_type = MALI_PLANE_TYPE_ASTC_2D;
            cfg.astc._2d.block_width = pan_astc_dim_2d(desc->block.width);
            cfg.astc._2d.block_height = pan_astc_dim_2d(desc->block.height);
         }

         bool srgb = (desc->colorspace == UTIL_FORMAT_COLORSPACE_SRGB);

         cfg.astc.decode_hdr = bview->astc.hdr;

         /* sRGB formats decode to RGBA8 sRGB, which is narrow.
          *
          * Non-sRGB formats decode to RGBA16F which is wide except if decode
          * precision is set to GL_RGBA8 for that texture.
          */
         cfg.astc.decode_wide = !srgb && !bview->astc.narrow;
      } else {
         cfg.plane_type = MALI_PLANE_TYPE_GENERIC;
         cfg.clump_format = pan_clump_format(bview->format);
      }
   }
}

static void
pan_emit_iview_plane(const struct pan_image_view *iview,
                     const struct pan_image_section_info *sections,
                     int plane_index, unsigned level, void **payload)
{
   const struct util_format_description *desc =
      util_format_description(iview->format);
   const struct pan_image_plane_ref pref =
      util_format_has_stencil(desc)
         ? pan_image_view_get_s_plane(iview)
         : pan_image_view_get_plane(iview, plane_index);
   const struct pan_image *image = pref.image;
   const struct pan_image_plane *plane = image->planes[pref.plane_idx];
   const struct pan_image_layout *layout = &plane->layout;
   const struct pan_image_props *props = &image->props;
   int32_t row_stride = sections[plane_index].row_stride;
   int32_t surface_stride = sections[plane_index].surface_stride;
   uint64_t pointer = sections[plane_index].pointer;

   assert(row_stride >= 0 && surface_stride >= 0 && "negative stride");

   bool afbc = drm_is_afbc(props->modifier);
   bool afrc = drm_is_afrc(props->modifier);
   // TODO: this isn't technically guaranteed to be YUV, but it is in practice.
   bool is_chroma_2p =
      desc->layout == UTIL_FORMAT_LAYOUT_PLANAR3 && plane_index > 0;

   pan_cast_and_pack(*payload, PLANE, cfg) {
      cfg.pointer = pointer;
      cfg.row_stride = row_stride;
      cfg.size = layout->slices[level].size_B;
#if PAN_ARCH >= 10
      struct pan_image_extent extent = {
         .width = u_minify(props->extent_px.width, level),
         .height = u_minify(props->extent_px.height, level),
      };

      if (is_chroma_2p) {
         cfg.two_plane_yuv_chroma.width = extent.width;
         cfg.two_plane_yuv_chroma.height = extent.height;
      } else {
         cfg.width = extent.width;
         cfg.height = extent.height;
      }
#endif

      if (is_chroma_2p) {
         cfg.two_plane_yuv_chroma.secondary_pointer =
            sections[plane_index + 1].pointer;
      } else if (!pan_format_is_yuv(props->format)) {
         cfg.slice_stride = props->nr_samples > 1
                               ? surface_stride
                               : pan_image_surface_stride(props, layout, level);
      }

      if (desc->layout == UTIL_FORMAT_LAYOUT_ASTC) {
         assert(!afbc);
         assert(!afrc);

         if (desc->block.depth > 1) {
            cfg.plane_type = MALI_PLANE_TYPE_ASTC_3D;
            cfg.astc._3d.block_width = pan_astc_dim_3d(desc->block.width);
            cfg.astc._3d.block_height = pan_astc_dim_3d(desc->block.height);
            cfg.astc._3d.block_depth = pan_astc_dim_3d(desc->block.depth);
         } else {
            cfg.plane_type = MALI_PLANE_TYPE_ASTC_2D;
            cfg.astc._2d.block_width = pan_astc_dim_2d(desc->block.width);
            cfg.astc._2d.block_height = pan_astc_dim_2d(desc->block.height);
         }

         bool srgb = (desc->colorspace == UTIL_FORMAT_COLORSPACE_SRGB);

         cfg.astc.decode_hdr = iview->astc.hdr;

         /* sRGB formats decode to RGBA8 sRGB, which is narrow.
          *
          * Non-sRGB formats decode to RGBA16F which is wide except if decode
          * precision is set to GL_RGBA8 for that texture.
          */
         cfg.astc.decode_wide = !srgb && !iview->astc.narrow;
      } else if (afbc) {
         cfg.plane_type = MALI_PLANE_TYPE_AFBC;
         cfg.afbc.superblock_size = translate_superblock_size(props->modifier);
         cfg.afbc.ytr = (props->modifier & AFBC_FORMAT_MOD_YTR);
         cfg.afbc.split_block = (props->modifier & AFBC_FORMAT_MOD_SPLIT);
         cfg.afbc.tiled_header = (props->modifier & AFBC_FORMAT_MOD_TILED);
         cfg.afbc.prefetch = true;
         cfg.afbc.compression_mode =
            pan_afbc_compression_mode(iview->format, plane_index);
         cfg.afbc.header_stride = layout->slices[level].afbc.header_size_B;
      } else if (afrc) {
#if PAN_ARCH >= 10
         struct pan_afrc_format_info finfo =
            pan_afrc_get_format_info(iview->format);

         cfg.plane_type = MALI_PLANE_TYPE_AFRC;
         cfg.afrc.block_size =
            pan_afrc_block_size(props->modifier, plane_index);
         cfg.afrc.format =
            pan_afrc_format(finfo, props->modifier, plane_index);
#endif
      } else {
         cfg.plane_type =
            is_chroma_2p ? MALI_PLANE_TYPE_CHROMA_2P : MALI_PLANE_TYPE_GENERIC;
         cfg.clump_format = pan_clump_format(iview->format);
      }

      if (!afbc && !afrc) {
         if (props->modifier == DRM_FORMAT_MOD_ARM_16X16_BLOCK_U_INTERLEAVED)
            cfg.clump_ordering = MALI_CLUMP_ORDERING_TILED_U_INTERLEAVED;
         else
            cfg.clump_ordering = MALI_CLUMP_ORDERING_LINEAR;
      }
   }
   *payload += pan_size(PLANE);
}
#endif

static void
pan_emit_iview_surface(const struct pan_image_view *iview, unsigned level,
                       unsigned index, unsigned sample, void **payload)
{
#if PAN_ARCH == 7 || PAN_ARCH >= 9
   if (pan_format_is_yuv(iview->format)) {
      struct pan_image_section_info sections[MAX_IMAGE_PLANES] = {0};
      unsigned plane_count = 0;

      for (int i = 0; i < MAX_IMAGE_PLANES; i++) {
         const struct pan_image_plane_ref pref =
            pan_image_view_get_plane(iview, i);

         if (!pref.image)
            break;

         sections[i] =
            get_image_section_info(iview, pref, level, index, sample);
         plane_count++;
      }

#if PAN_ARCH >= 9
      /* 3-plane YUV is submitted using two PLANE descriptors, where the
       * second one is of type CHROMA_2P */
      pan_emit_iview_plane(iview, sections, 0, level, payload);

      if (plane_count > 1) {
         /* 3-plane YUV requires equal stride for both chroma planes */
         assert(plane_count == 2 ||
                sections[1].row_stride == sections[2].row_stride);
         pan_emit_iview_plane(iview, sections, 1, level, payload);
      }
#else
      if (plane_count > 1)
         pan_emit_iview_multiplanar_surface(sections, payload);
      else
         pan_emit_iview_surface_with_stride(sections, payload);
#endif
      return;
   }
#endif

   const struct util_format_description *fdesc =
      util_format_description(iview->format);

   /* In case of multiplanar depth/stencil, the stencil is always on
    * plane 1. Combined depth/stencil only has one plane, so depth
    * will be on plane 0 in either case.
    */
   const struct pan_image_plane_ref pref =
      util_format_has_stencil(fdesc) ? pan_image_view_get_s_plane(iview)
                                     : pan_image_view_get_plane(iview, 0);
   assert(pref.image != NULL);

   struct pan_image_section_info section[1] = {
      get_image_section_info(iview, pref, level, index, sample),
   };

#if PAN_ARCH >= 9
   pan_emit_iview_plane(iview, section, 0, level, payload);
#else
   pan_emit_iview_surface_with_stride(section, payload);
#endif
}

static void
pan_emit_iview_texture_payload(const struct pan_image_view *iview,
                               void *payload)
{
   unsigned nr_samples =
      PAN_ARCH < 9 ? pan_image_view_get_nr_samples(iview) : 1;

   /* Inject the addresses in, interleaving array indices, mip levels,
    * cube faces, and strides in that order. On Bifrost and older, each
    * sample had its own surface descriptor; on Valhall, they are fused
    * into a single plane descriptor.
    */

#if PAN_ARCH >= 7
   /* V7 and later treats faces as extra layers */
   for (int layer = iview->first_layer; layer <= iview->last_layer; ++layer) {
      for (int sample = 0; sample < nr_samples; ++sample) {
         for (int level = iview->first_level; level <= iview->last_level;
              ++level) {
            pan_emit_iview_surface(iview, level, layer, sample, &payload);
         }
      }
   }
#else
   unsigned first_layer = iview->first_layer, last_layer = iview->last_layer;
   unsigned face_count = 1;

   if (iview->dim == MALI_TEXTURE_DIMENSION_CUBE) {
      first_layer /= 6;
      last_layer /= 6;
      face_count = 6;
   }

   /* V6 and earlier has a different memory-layout */
   for (int layer = first_layer; layer <= last_layer; ++layer) {
      for (int level = iview->first_level; level <= iview->last_level;
           ++level) {
         /* order of face and sample doesn't matter; we can only have multiple
          * of one or the other (no support for multisampled cubemaps)
          */
         for (int face = 0; face < face_count; ++face) {
            for (int sample = 0; sample < nr_samples; ++sample) {
               pan_emit_iview_surface(iview, level, (face_count * layer) + face,
                                      sample, &payload);
            }
         }
      }
   }
#endif
}

#if PAN_ARCH < 9
/* Map modifiers to mali_texture_layout for packing in a texture descriptor */

static enum mali_texture_layout
pan_modifier_to_layout(uint64_t modifier)
{
   if (drm_is_afbc(modifier))
      return MALI_TEXTURE_LAYOUT_AFBC;
   else if (modifier == DRM_FORMAT_MOD_ARM_16X16_BLOCK_U_INTERLEAVED)
      return MALI_TEXTURE_LAYOUT_TILED;
   else if (modifier == DRM_FORMAT_MOD_LINEAR)
      return MALI_TEXTURE_LAYOUT_LINEAR;
   else
      unreachable("Invalid modifer");
}
#endif

#if PAN_ARCH >= 7
void
GENX(pan_texture_swizzle_replicate_x)(struct pan_image_view *iview)
{
   /* v7+ doesn't have an _RRRR component order, combine the
    * user swizzle with a .XXXX swizzle to emulate that. */
   assert(util_format_is_depth_or_stencil(iview->format));

   static const unsigned char replicate_x[4] = {
      PIPE_SWIZZLE_X,
      PIPE_SWIZZLE_X,
      PIPE_SWIZZLE_X,
      PIPE_SWIZZLE_X,
   };

   util_format_compose_swizzles(replicate_x, iview->swizzle, iview->swizzle);
}
#endif

#if PAN_ARCH == 7
void
GENX(pan_texture_afbc_reswizzle)(struct pan_image_view *iview)
{
   /* v7 (only) restricts component orders when AFBC is in use.
    * Rather than restrict AFBC for all non-canonical component orders, we use
    * an allowed component order with an invertible swizzle composed.
    * This allows us to support AFBC(BGR) as well as AFBC(RGB).
    */
   assert(!util_format_is_depth_or_stencil(iview->format));
   assert(!pan_format_is_yuv(iview->format));
   assert(pan_format_supports_afbc(PAN_ARCH, iview->format));

   uint32_t mali_format = GENX(pan_format_from_pipe_format)(iview->format)->hw;

   enum mali_rgb_component_order orig = mali_format & BITFIELD_MASK(12);
   struct pan_decomposed_swizzle decomposed = GENX(pan_decompose_swizzle)(orig);

   /* Apply the new component order */
   if (orig != decomposed.pre)
      iview->format = util_format_rgb_to_bgr(iview->format);
   /* Only RGB<->BGR should be allowed for AFBC */
   assert(iview->format != PIPE_FORMAT_NONE);
   assert(decomposed.pre ==
          (GENX(pan_format_from_pipe_format)(iview->format)->hw &
           BITFIELD_MASK(12)));

   /* Compose the new swizzle */
   util_format_compose_swizzles(decomposed.post, iview->swizzle,
                                iview->swizzle);
}
#endif

static unsigned
pan_texture_get_array_size(const struct pan_image_view *iview)
{
   unsigned array_size = iview->last_layer - iview->first_layer + 1;

   /* If this is a cubemap, we expect the number of layers to be a multiple
    * of 6.
    */
   if (iview->dim == MALI_TEXTURE_DIMENSION_CUBE) {
      assert(array_size % 6 == 0);
      array_size /= 6;
   }

   /* Multiplanar YUV textures require 2 surface descriptors. */
   if (pan_format_is_yuv(iview->format) && PAN_ARCH >= 9 &&
       pan_image_view_get_plane(iview, 1).image != NULL)
      array_size *= 2;

   return array_size;
}

static struct pan_image_extent
pan_texture_get_extent(const struct pan_image_view *iview,
                       const struct pan_image_props *iprops)
{
   struct pan_image_extent extent_px = {
      .width = u_minify(iprops->extent_px.width, iview->first_level),
      .height = u_minify(iprops->extent_px.height, iview->first_level),
      .depth = u_minify(iprops->extent_px.depth, iview->first_level),
   };

   if (util_format_is_compressed(iprops->format) &&
       !util_format_is_compressed(iview->format)) {
      extent_px.width = DIV_ROUND_UP(
         extent_px.width, util_format_get_blockwidth(iprops->format));
      extent_px.height = DIV_ROUND_UP(
         extent_px.height, util_format_get_blockheight(iprops->format));
      extent_px.depth = DIV_ROUND_UP(
         extent_px.depth, util_format_get_blockdepth(iprops->format));
      assert(util_format_get_blockwidth(iview->format) == 1);
      assert(util_format_get_blockheight(iview->format) == 1);
      assert(util_format_get_blockheight(iview->format) == 1);
      assert(iview->last_level == iview->first_level);
   }

   return extent_px;
}

/*
 * Generates a texture descriptor. Ideally, descriptors are immutable after the
 * texture is created, so we can keep these hanging around in GPU memory in a
 * dedicated BO and not have to worry. In practice there are some minor gotchas
 * with this (the driver sometimes will change the format of a texture on the
 * fly for compression) but it's fast enough to just regenerate the descriptor
 * in those cases, rather than monkeypatching at drawtime. A texture descriptor
 * consists of a 32-byte header followed by pointers.
 */
void
GENX(pan_sampled_texture_emit)(const struct pan_image_view *iview,
                               struct mali_texture_packed *out,
                               const struct pan_ptr *payload)
{
   pan_image_view_check(iview);

   const struct util_format_description *desc =
      util_format_description(iview->format);
   const struct pan_image_plane_ref first_plane = pan_image_view_get_first_plane(iview);
   const struct pan_image_props *props = &first_plane.image->props;
   uint32_t mali_format = GENX(pan_format_from_pipe_format)(iview->format)->hw;

   if (desc->layout == UTIL_FORMAT_LAYOUT_ASTC && iview->astc.narrow &&
       desc->colorspace != UTIL_FORMAT_COLORSPACE_SRGB) {
      mali_format = MALI_PACK_FMT(RGBA8_UNORM, RGBA, L);
   }

   pan_emit_iview_texture_payload(iview, payload->cpu);

   unsigned array_size = pan_texture_get_array_size(iview);

   struct pan_image_extent extent_px =
      pan_texture_get_extent(iview, props);

   pan_pack(out, TEXTURE, cfg) {
      cfg.dimension = iview->dim;
      cfg.format = mali_format;
      cfg.width = extent_px.width;
      cfg.height = extent_px.height;
      if (iview->dim == MALI_TEXTURE_DIMENSION_3D)
         cfg.depth = extent_px.depth;
      else
         cfg.sample_count = props->nr_samples;
      cfg.swizzle = pan_translate_swizzle_4(iview->swizzle);
#if PAN_ARCH >= 9
      cfg.texel_interleave = (props->modifier != DRM_FORMAT_MOD_LINEAR) ||
                             util_format_is_compressed(iview->format);
#else
      cfg.texel_ordering = pan_modifier_to_layout(props->modifier);
#endif
      cfg.levels = iview->last_level - iview->first_level + 1;
      cfg.array_size = array_size;

#if PAN_ARCH >= 6
      cfg.surfaces = payload->gpu;

      /* We specify API-level LOD clamps in the sampler descriptor
       * and use these clamps simply for bounds checking.
       */
      cfg.minimum_lod = 0;
      cfg.maximum_lod = cfg.levels - 1;
#endif
   }
}

#if PAN_ARCH >= 9
void
GENX(pan_storage_texture_emit)(const struct pan_image_view *iview,
                               struct mali_texture_packed *out,
                               const struct pan_ptr *payload)
{
   pan_image_view_check(iview);

   const struct util_format_description *desc =
      util_format_description(iview->format);
   const struct pan_image_plane_ref first_plane =
      pan_image_view_get_first_plane(iview);
   const struct pan_image_props *props = &first_plane.image->props;

   /* AFBC and AFRC cannot be used in storage operations. */
   assert(!drm_is_afbc(props->modifier));
   assert(!drm_is_afrc(props->modifier));

   uint32_t mali_format =
      GENX(pan_format_from_pipe_format)(iview->format)->hw;
   if (desc->layout == UTIL_FORMAT_LAYOUT_ASTC && iview->astc.narrow &&
       desc->colorspace != UTIL_FORMAT_COLORSPACE_SRGB) {
      mali_format = MALI_PACK_FMT(RGBA8_UNORM, RGBA, L);
   }

   pan_emit_iview_texture_payload(iview, payload->cpu);

   unsigned array_size = pan_texture_get_array_size(iview);

   struct pan_image_extent extent_px =
      pan_texture_get_extent(iview, props);

   static const unsigned char rgba_swizzle[4] = {
      PIPE_SWIZZLE_X,
      PIPE_SWIZZLE_Y,
      PIPE_SWIZZLE_Z,
      PIPE_SWIZZLE_W,
   };

   pan_pack(out, TEXTURE, cfg) {
      cfg.dimension = iview->dim;
      cfg.format = mali_format;
      cfg.width = extent_px.width;
      cfg.height = extent_px.height;
      if (iview->dim == MALI_TEXTURE_DIMENSION_3D)
         cfg.depth = extent_px.depth;
      else
         cfg.sample_count = props->nr_samples;
      cfg.texel_interleave = (props->modifier != DRM_FORMAT_MOD_LINEAR) ||
                             util_format_is_compressed(iview->format);
      cfg.levels = iview->last_level - iview->first_level + 1;
      cfg.array_size = array_size;

      cfg.surfaces = payload->gpu;

      /* Requirements for storage image use. */
      cfg.minimum_lod = 0;
      cfg.maximum_lod = 0;
      cfg.minimum_level = 0;
      cfg.swizzle = pan_translate_swizzle_4(rgba_swizzle);
   }
}
#endif

void
GENX(pan_buffer_texture_emit)(const struct pan_buffer_view *bview,
                              struct mali_texture_packed *out,
                              const struct pan_ptr *payload)
{
   uint32_t mali_format = GENX(pan_format_from_pipe_format)(bview->format)->hw;
   static const unsigned char rgba_swizzle[4] = {
      PIPE_SWIZZLE_X,
      PIPE_SWIZZLE_Y,
      PIPE_SWIZZLE_Z,
      PIPE_SWIZZLE_W,
   };

#if PAN_ARCH >= 9
   pan_emit_bview_plane(bview, payload->cpu);
#else
   pan_emit_bview_surface_with_stride(bview, payload->cpu);
#endif

   pan_pack(out, TEXTURE, cfg) {
      cfg.dimension = MALI_TEXTURE_DIMENSION_1D;
      cfg.format = mali_format;
      cfg.width = bview->width_el;
      cfg.height = 1;
      cfg.sample_count = 1;
      cfg.swizzle = pan_translate_swizzle_4(rgba_swizzle);
#if PAN_ARCH >= 9
      cfg.texel_interleave = false;
#else
      cfg.texel_ordering = MALI_TEXTURE_LAYOUT_LINEAR;
#endif
      cfg.levels = 1;
      cfg.array_size = 1;

#if PAN_ARCH >= 6
      cfg.surfaces = payload->gpu;
      cfg.minimum_lod = 0;
      cfg.maximum_lod = 0;
#endif
   }
}
