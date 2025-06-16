/*
 * Copyright (C) 2019-2022 Collabora, Ltd.
 * Copyright (C) 2018-2019 Alyssa Rosenzweig
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

#include "util/log.h"
#include "util/macros.h"
#include "util/u_math.h"
#include "pan_afbc.h"
#include "pan_afrc.h"
#include "pan_layout.h"
#include "pan_props.h"

/*
 * Computes sizes for checksumming, which is 8 bytes per 16x16 tile.
 * Checksumming is believed to be a CRC variant (CRC64 based on the size?).
 * This feature is also known as "transaction elimination".
 * CRC values are prefetched by 32x32 (64x64 on v12+) regions so size needs to
 * be aligned.
 */

#define CHECKSUM_TILE_WIDTH     16
#define CHECKSUM_TILE_HEIGHT    16
#define CHECKSUM_BYTES_PER_TILE 8

static void
init_slice_crc_info(unsigned arch, struct pan_image_slice_layout *slice,
                    unsigned width_px, unsigned height_px, uint64_t offset_B)
{
   unsigned checksum_region_size_px = pan_meta_tile_size(arch);
   unsigned checksum_x_tile_per_region =
      (checksum_region_size_px / CHECKSUM_TILE_WIDTH);
   unsigned checksum_y_tile_per_region =
      (checksum_region_size_px / CHECKSUM_TILE_HEIGHT);

   unsigned tile_count_x = checksum_x_tile_per_region *
                           DIV_ROUND_UP(width_px, checksum_region_size_px);
   unsigned tile_count_y = checksum_y_tile_per_region *
                           DIV_ROUND_UP(height_px, checksum_region_size_px);

   slice->crc.offset_B = offset_B;
   slice->crc.stride_B = tile_count_x * CHECKSUM_BYTES_PER_TILE;
   slice->crc.size_B = slice->crc.stride_B * tile_count_y;
}

static struct pan_image_extent
get_mip_level_extent(const struct pan_image_props *props, unsigned plane_idx,
                     unsigned mip_level)
{
   return (struct pan_image_extent){
      .width = u_minify(util_format_get_plane_width(props->format, plane_idx,
                                                    props->extent_px.width),
                        mip_level),
      .height = u_minify(util_format_get_plane_height(props->format, plane_idx,
                                                      props->extent_px.height),
                         mip_level),
      .depth = u_minify(props->extent_px.depth, mip_level),
   };
}

static unsigned
get_afbc_wsi_row_pitch(const struct pan_image_props *props, unsigned plane_idx,
                       const struct pan_image_slice_layout *slayout)
{
   const unsigned header_row_stride_B = slayout->row_stride_B;
   const struct pan_image_block_size tile_extent_el =
      pan_afbc_superblock_size_el(props->format, props->modifier);
   const unsigned tile_payload_size_B =
      tile_extent_el.width * tile_extent_el.height *
      pan_format_get_plane_blocksize(props->format, plane_idx);
   const unsigned tile_row_payload_size_B =
      pan_afbc_stride_blocks(props->modifier, header_row_stride_B) *
      tile_payload_size_B;

   return tile_row_payload_size_B / pan_afbc_superblock_height(props->modifier);
}

static bool
init_afbc_slice_layout(
   unsigned arch, const struct pan_image_props *props, unsigned plane_idx,
   struct pan_image_extent mip_extent_px,
   const struct pan_image_layout_constraints *layout_constraints,
   struct pan_image_slice_layout *slayout)
{
   /* Use explicit layout only when wsi_row_pitch_B is non-zero */
   const bool use_explicit_layout =
      layout_constraints && layout_constraints->wsi_row_pitch_B;
   struct pan_image_block_size afbc_tile_extent_px =
      pan_afbc_superblock_size(props->modifier);
   unsigned offset_align_mask =
      pan_afbc_header_align(arch, props->modifier) - 1;
   unsigned row_align_mask =
      pan_afbc_header_row_stride_align(arch, props->format, props->modifier) -
      1;
   struct pan_image_block_size afbc_tile_extent_el =
      pan_afbc_superblock_size_el(props->format, props->modifier);
   unsigned afbc_tile_payload_size_B =
      afbc_tile_extent_el.width * afbc_tile_extent_el.height *
      pan_format_get_plane_blocksize(props->format, plane_idx);

   struct pan_image_block_size align_px =
      pan_afbc_renderblock_size(props->modifier);

   /* If superblock tiling is used, align on a superblock tile. */
   if (props->modifier & AFBC_FORMAT_MOD_TILED) {
      align_px.width =
         ALIGN_POT(align_px.width, afbc_tile_extent_px.width *
                                      pan_afbc_tile_size(props->modifier));
      align_px.height =
         ALIGN_POT(align_px.height, afbc_tile_extent_px.height *
                                       pan_afbc_tile_size(props->modifier));
   }

   struct pan_image_extent aligned_extent_px = {
      .width = ALIGN_POT(mip_extent_px.width, align_px.width),
      .height = ALIGN_POT(mip_extent_px.height, align_px.height),
      .depth = mip_extent_px.depth,
   };

   if (use_explicit_layout) {
      unsigned afbc_tile_payload_row_stride_B =
         layout_constraints->wsi_row_pitch_B *
         pan_afbc_superblock_height(props->modifier);

      /* For quite some time, we've been accepting WSI row pitch that
       * didn't match exactly the image size and have been assuming tightly
       * packed tile rows instead of using the explicit stride in that case.
       * This is something we can't change without risking breaking existing
       * users, so we enforce this explicit tile alignment only if we were
       * asked to. */
      if (layout_constraints->strict &&
          (afbc_tile_payload_row_stride_B % afbc_tile_payload_size_B)) {
         mesa_loge("WSI pitch is not aligned on an AFBC tile");
         return false;
      }

      unsigned width_from_wsi_row_stride =
         (afbc_tile_payload_row_stride_B / afbc_tile_payload_size_B) *
         pan_afbc_superblock_width(props->modifier);

      if (width_from_wsi_row_stride < mip_extent_px.width) {
         mesa_loge("WSI pitch too small");
         return false;
      }

      slayout->row_stride_B =
         pan_afbc_row_stride(props->modifier, width_from_wsi_row_stride);
      if (slayout->row_stride_B & row_align_mask) {
         mesa_loge("WSI pitch not properly aligned");
         return false;
      }

      slayout->offset_B = layout_constraints->offset_B;
      if (slayout->offset_B & offset_align_mask) {
         mesa_loge("WSI offset not properly aligned");
         return false;
      }

      /* If this is not a strict import, ignore the WSI row pitch and use
       * the resource width to get the size. */
      if (!layout_constraints->strict) {
         slayout->row_stride_B = ALIGN_POT(
            pan_afbc_row_stride(props->modifier, aligned_extent_px.width),
            row_align_mask + 1);
      }
   } else {
      slayout->offset_B =
         ALIGN_POT(layout_constraints ? layout_constraints->offset_B : 0,
                   offset_align_mask + 1);
      slayout->row_stride_B = ALIGN_POT(
         pan_afbc_row_stride(props->modifier, aligned_extent_px.width),
         row_align_mask + 1);
   }

   const unsigned row_stride_sb =
      pan_afbc_stride_blocks(props->modifier, slayout->row_stride_B);
   const unsigned surface_stride_sb =
      row_stride_sb * (aligned_extent_px.height / afbc_tile_extent_px.height);

   slayout->afbc.surface_stride_B =
      surface_stride_sb * AFBC_HEADER_BYTES_PER_TILE;
   slayout->afbc.surface_stride_B =
      ALIGN_POT(slayout->afbc.surface_stride_B, offset_align_mask + 1);

   slayout->afbc.header_size_B =
      ALIGN_POT(slayout->afbc.surface_stride_B * aligned_extent_px.depth,
                pan_afbc_body_align(arch, props->modifier));

   slayout->surface_stride_B = surface_stride_sb * afbc_tile_payload_size_B;
   slayout->afbc.body_size_B =
      surface_stride_sb * afbc_tile_payload_size_B * aligned_extent_px.depth;
   slayout->size_B =
      (uint64_t)slayout->afbc.header_size_B + slayout->afbc.body_size_B;

   if (props->dim != MALI_TEXTURE_DIMENSION_3D) {
      /* FIXME: it doesn't make sense to have the header size counted in the
       * surface stride. Will be fixed once we revisit the
       * pan_image_slice_layout fields to make them mod-specific. */
      slayout->surface_stride_B += slayout->afbc.header_size_B;
      slayout->afbc.surface_stride_B = slayout->surface_stride_B;
   }

   return true;
}

static unsigned
get_afrc_wsi_row_pitch(const struct pan_image_props *props, unsigned plane_idx,
                       const struct pan_image_slice_layout *slayout)
{
   const struct pan_image_block_size tile_extent_px =
      pan_afrc_tile_size(props->format, props->modifier);

   return slayout->row_stride_B / tile_extent_px.height;
}

static bool
init_afrc_slice_layout(
   unsigned arch, const struct pan_image_props *props, unsigned plane_idx,
   struct pan_image_extent mip_extent_px,
   const struct pan_image_layout_constraints *layout_constraints,
   struct pan_image_slice_layout *slayout)
{
   /* Use explicit layout only when wsi_row_pitch_B is non-zero */
   const bool use_explicit_layout =
      layout_constraints && layout_constraints->wsi_row_pitch_B;
   const unsigned align_mask =
      pan_afrc_buffer_alignment_from_modifier(props->modifier) - 1;
   struct pan_image_block_size tile_extent_px =
      pan_afrc_tile_size(props->format, props->modifier);
   struct pan_image_extent aligned_extent_px = {
      .width = ALIGN_POT(mip_extent_px.width, tile_extent_px.width),
      .height = ALIGN_POT(mip_extent_px.height, tile_extent_px.height),
      .depth = mip_extent_px.depth,
   };

   if (use_explicit_layout) {
      slayout->row_stride_B =
         layout_constraints->wsi_row_pitch_B * tile_extent_px.height;
      if (slayout->row_stride_B & align_mask) {
         mesa_loge("WSI pitch not properly aligned");
         return false;
      }

      slayout->offset_B = layout_constraints->offset_B;
      if (slayout->offset_B & align_mask) {
         mesa_loge("WSI offset not properly aligned");
         return false;
      }

      unsigned afrc_blk_size_B =
         pan_afrc_block_size_from_modifier(props->modifier) *
         AFRC_CLUMPS_PER_TILE;
      unsigned width_from_wsi_row_stride =
         (slayout->row_stride_B / afrc_blk_size_B) * tile_extent_px.width;

      if (width_from_wsi_row_stride < mip_extent_px.width) {
         mesa_loge("WSI pitch too small");
         return false;
      }

      /* If this is not a strict import, ignore the WSI row pitch and use
       * the resource width to get the size. */
      if (!layout_constraints->strict) {
         slayout->row_stride_B = pan_afrc_row_stride(
            props->format, props->modifier, mip_extent_px.width);
      }
   } else {
      /* Align levels to cache-line as a performance improvement for
       * linear/tiled and as a requirement for AFBC */
      slayout->offset_B = ALIGN_POT(
         layout_constraints ? layout_constraints->offset_B : 0, align_mask + 1);
      slayout->row_stride_B =
         ALIGN_POT(pan_afrc_row_stride(props->format, props->modifier,
                                       mip_extent_px.width),
                   align_mask + 1);
   }

   slayout->surface_stride_B =
      slayout->row_stride_B *
      DIV_ROUND_UP(aligned_extent_px.height, aligned_extent_px.height);
   slayout->size_B = (uint64_t)slayout->surface_stride_B *
                     aligned_extent_px.depth * props->nr_samples;
   return true;
}

static unsigned
get_u_tiled_wsi_row_pitch(const struct pan_image_props *props,
                          unsigned plane_idx,
                          const struct pan_image_slice_layout *slayout)
{
   return slayout->row_stride_B /
          pan_u_interleaved_tile_size_el(props->format).height;
}

static bool
init_u_tiled_slice_layout(
   unsigned arch, const struct pan_image_props *props, unsigned plane_idx,
   struct pan_image_extent mip_extent_px,
   const struct pan_image_layout_constraints *layout_constraints,
   struct pan_image_slice_layout *slayout)
{
   /* Use explicit layout only when wsi_row_pitch_B is non-zero */
   const bool use_explicit_layout =
      layout_constraints && layout_constraints->wsi_row_pitch_B;
   unsigned align_mask =
      pan_linear_or_tiled_row_align_req(arch, props->format, plane_idx) - 1;
   struct pan_image_block_size tile_extent_el =
      pan_u_interleaved_tile_size_el(props->format);
   struct pan_image_extent mip_extent_el;
   unsigned tile_size_B;

   if (util_format_is_compressed(props->format)) {
      assert(util_format_get_num_planes(props->format) == 1);
      mip_extent_el.width = DIV_ROUND_UP(
         mip_extent_px.width, util_format_get_blockwidth(props->format));
      mip_extent_el.height = DIV_ROUND_UP(
         mip_extent_px.height, util_format_get_blockheight(props->format));
      mip_extent_el.depth = DIV_ROUND_UP(
         mip_extent_px.depth, util_format_get_blockdepth(props->format));
      tile_size_B = tile_extent_el.width * tile_extent_el.height *
                    pan_format_get_plane_blocksize(props->format, plane_idx);
   } else {
      /* Block-based YUV needs special care, because the U-tile extent
       * is in pixels, not blocks in that case. */
      assert(tile_extent_el.width % util_format_get_blockwidth(props->format) ==
             0);
      assert(tile_extent_el.height %
                util_format_get_blockheight(props->format) ==
             0);
      mip_extent_el = mip_extent_px;
      tile_size_B =
         (tile_extent_el.width / util_format_get_blockwidth(props->format)) *
         (tile_extent_el.height / util_format_get_blockheight(props->format)) *
         pan_format_get_plane_blocksize(props->format, plane_idx);
   }

   if (use_explicit_layout) {
      slayout->row_stride_B =
         layout_constraints->wsi_row_pitch_B * tile_extent_el.height;
      if (slayout->row_stride_B & align_mask) {
         mesa_loge("WSI pitch not properly aligned");
         return false;
      }

      const unsigned width_from_wsi_row_stride =
         (slayout->row_stride_B / tile_size_B) * tile_extent_el.width;

      if (width_from_wsi_row_stride < mip_extent_el.width) {
         mesa_loge("WSI pitch too small");
         return false;
      }

      slayout->offset_B = layout_constraints->offset_B;
      if (slayout->offset_B & align_mask) {
         mesa_loge("WSI offset not properly aligned");
         return false;
      }
   } else {
      /* When we can decide of the layout, we want things aligned on at least a
       * cacheline for performance reasons. */
      align_mask = MAX2(align_mask, 63);
      slayout->offset_B = ALIGN_POT(
         layout_constraints ? layout_constraints->offset_B : 0,
         MAX2(align_mask + 1, pan_image_slice_align(props->modifier)));
      slayout->row_stride_B = ALIGN_POT(
         tile_size_B * DIV_ROUND_UP(mip_extent_el.width, tile_extent_el.width),
         align_mask + 1);
   }

   uint64_t surf_stride_B =
      (uint64_t)slayout->row_stride_B *
      DIV_ROUND_UP(mip_extent_el.height, tile_extent_el.height);
   surf_stride_B = ALIGN_POT(surf_stride_B, (uint64_t)align_mask + 1);

   slayout->surface_stride_B = surf_stride_B;
   slayout->size_B = surf_stride_B * mip_extent_el.depth * props->nr_samples;
   return true;
}

static unsigned
get_linear_wsi_row_pitch(const struct pan_image_props *props,
                         unsigned plane_idx,
                         const struct pan_image_slice_layout *slayout)
{
   return slayout->row_stride_B;
}

static bool
init_linear_slice_layout(
   unsigned arch, const struct pan_image_props *props, unsigned plane_idx,
   struct pan_image_extent mip_extent_px,
   const struct pan_image_layout_constraints *layout_constraints,
   struct pan_image_slice_layout *slayout)
{
   /* Use explicit layout only when wsi_row_pitch_B is non-zero */
   const bool use_explicit_layout =
      layout_constraints && layout_constraints->wsi_row_pitch_B;
   unsigned align_mask =
      pan_linear_or_tiled_row_align_req(arch, props->format, plane_idx) - 1;
   const unsigned fmt_blksize_B =
      pan_format_get_plane_blocksize(props->format, plane_idx);
   struct pan_image_extent mip_extent_el;

   if (util_format_is_compressed(props->format)) {
      assert(util_format_get_num_planes(props->format) == 1);
      mip_extent_el.width = DIV_ROUND_UP(
         mip_extent_px.width, util_format_get_blockwidth(props->format));
      mip_extent_el.height = DIV_ROUND_UP(
         mip_extent_px.height, util_format_get_blockheight(props->format));
      mip_extent_el.depth = DIV_ROUND_UP(
         mip_extent_px.depth, util_format_get_blockdepth(props->format));
   } else {
      mip_extent_el = mip_extent_px;
   }

   if (use_explicit_layout) {
      unsigned width_from_wsi_row_stride =
         layout_constraints->wsi_row_pitch_B / fmt_blksize_B;

      if (!util_format_is_compressed(props->format))
         width_from_wsi_row_stride *= util_format_get_blockwidth(props->format);

      if (width_from_wsi_row_stride < mip_extent_el.width) {
         mesa_loge("WSI pitch too small");
         return false;
      }

      slayout->row_stride_B = layout_constraints->wsi_row_pitch_B;
      if (slayout->row_stride_B & align_mask) {
         mesa_loge("WSI pitch not properly aligned");
         return false;
      }

      slayout->offset_B = layout_constraints->offset_B;
      if (slayout->offset_B & align_mask) {
         mesa_loge("WSI offset not properly aligned");
         return false;
      }
   } else {
      /* When we can decide of the layout, we want things aligned on at least a
       * cacheline for performance reasons. */
      align_mask = MAX2(align_mask, 63);
      slayout->offset_B = ALIGN_POT(
         layout_constraints ? layout_constraints->offset_B : 0,
         MAX2(align_mask + 1, pan_image_slice_align(props->modifier)));
      slayout->row_stride_B =
         ALIGN_POT(mip_extent_el.width * fmt_blksize_B, align_mask + 1);
   }

   uint64_t surf_stride_B = (uint64_t)mip_extent_el.height * slayout->row_stride_B;
   surf_stride_B = ALIGN_POT(surf_stride_B, (uint64_t)align_mask + 1);

   slayout->surface_stride_B = surf_stride_B;
   slayout->size_B = surf_stride_B * mip_extent_el.depth * props->nr_samples;
   return true;
}

static bool
init_slice_layout(
   unsigned arch, const struct pan_image_props *props, unsigned plane_idx,
   struct pan_image_extent mip_extent_px,
   const struct pan_image_layout_constraints *layout_constraints,
   struct pan_image_slice_layout *slayout)
{
   if (drm_is_afbc(props->modifier)) {
      return init_afbc_slice_layout(arch, props, plane_idx, mip_extent_px,
                                    layout_constraints, slayout);
   } else if (drm_is_afrc(props->modifier)) {
      return init_afrc_slice_layout(arch, props, plane_idx, mip_extent_px,
                                    layout_constraints, slayout);
   } else if (props->modifier == DRM_FORMAT_MOD_ARM_16X16_BLOCK_U_INTERLEAVED) {
      return init_u_tiled_slice_layout(arch, props, plane_idx, mip_extent_px,
                                       layout_constraints, slayout);
   } else {
      assert(props->modifier == DRM_FORMAT_MOD_LINEAR);
      return init_linear_slice_layout(arch, props, plane_idx, mip_extent_px,
                                      layout_constraints, slayout);
   }
}

bool
pan_image_layout_init(
   unsigned arch, const struct pan_image_props *props, unsigned plane_idx,
   const struct pan_image_layout_constraints *explicit_layout_constraints,
   struct pan_image_layout *layout)
{
   /* Use explicit layout only when wsi_row_pitch_B is non-zero */
   struct pan_image_layout_constraints layout_constraints = {0};
   if (explicit_layout_constraints)
      layout_constraints = *explicit_layout_constraints;

   const bool use_explicit_layout = layout_constraints.wsi_row_pitch_B != 0;

   /* Explicit stride only work with non-mipmap, non-array, single-sample
    * 2D image without CRC.
    */
   if (use_explicit_layout &&
       (props->extent_px.depth > 1 || props->nr_samples > 1 ||
        props->array_size > 1 || props->dim != MALI_TEXTURE_DIMENSION_2D ||
        props->nr_slices > 1 || props->crc))
      return false;

   if (plane_idx >= util_format_get_num_planes(props->format))
      return false;

   /* MSAA is implemented as a 3D texture with z corresponding to the
    * sample #, horrifyingly enough */

   assert(props->extent_px.depth == 1 || props->nr_samples == 1);

   struct pan_image_extent mip_extent_px = {
      .width = util_format_get_plane_width(props->format, plane_idx,
                                           props->extent_px.width),
      .height = util_format_get_plane_height(props->format, plane_idx,
                                             props->extent_px.height),
      .depth = props->extent_px.depth,
   };

   for (unsigned l = 0; l < props->nr_slices; ++l) {
      struct pan_image_slice_layout *slayout = &layout->slices[l];

      if (!init_slice_layout(arch, props, plane_idx, mip_extent_px,
                             &layout_constraints, slayout))
         return false;

      layout_constraints.offset_B += slayout->size_B;

      /* Add a checksum region if necessary */
      if (props->crc) {
         init_slice_crc_info(arch, slayout, mip_extent_px.width,
                             mip_extent_px.height, layout_constraints.offset_B);
         layout_constraints.offset_B += slayout->crc.size_B;
         slayout->size_B += slayout->crc.size_B;
      }

      mip_extent_px.width = u_minify(mip_extent_px.width, 1);
      mip_extent_px.height = u_minify(mip_extent_px.height, 1);
      mip_extent_px.depth = u_minify(mip_extent_px.depth, 1);
   }

   /* Arrays and cubemaps have the entire miptree duplicated */
   layout->array_stride_B =
      ALIGN_POT(layout_constraints.offset_B - layout->slices[0].offset_B, 64);
   if (use_explicit_layout) {
      layout->data_size_B =
         layout_constraints.offset_B - explicit_layout_constraints->offset_B;
   } else {
      /* Native images start from offset 0, and the planar plane offset has
       * been at least 4K page aligned below. So the base level slice offset
       * should always be the same with the plane offset.
       */
      assert(!explicit_layout_constraints ||
             explicit_layout_constraints->offset_B ==
                layout->slices[0].offset_B);
      layout->data_size_B = ALIGN_POT(
         (uint64_t)layout->array_stride_B * (uint64_t)props->array_size, 4096);
   }

   return true;
}

unsigned
pan_image_get_wsi_row_pitch(const struct pan_image_props *props,
                            unsigned plane_idx,
                            const struct pan_image_layout *layout,
                            unsigned level)
{
   const struct pan_image_slice_layout *slayout = &layout->slices[level];

   if (drm_is_afbc(props->modifier)) {
      return get_afbc_wsi_row_pitch(props, plane_idx, slayout);
   } else if (drm_is_afrc(props->modifier)) {
      return get_afrc_wsi_row_pitch(props, plane_idx, slayout);
   } else if (props->modifier == DRM_FORMAT_MOD_ARM_16X16_BLOCK_U_INTERLEAVED) {
      return get_u_tiled_wsi_row_pitch(props, plane_idx, slayout);
   } else {
      assert(props->modifier == DRM_FORMAT_MOD_LINEAR);
      return get_linear_wsi_row_pitch(props, plane_idx, slayout);
   }
}
