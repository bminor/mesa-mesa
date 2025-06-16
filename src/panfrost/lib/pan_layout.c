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
 * Given a format, determine the tile size used for u-interleaving. For formats
 * that are already block compressed, this is 4x4. For all other formats, this
 * is 16x16, hence the modifier name.
 */
static inline struct pan_image_block_size
pan_u_interleaved_tile_size_el(enum pipe_format format)
{
   if (util_format_is_compressed(format)) {
      return (struct pan_image_block_size){4, 4};
   } else {
      assert(16 % util_format_get_blockwidth(format) == 0);
      assert(16 % util_format_get_blockheight(format) == 0);
      return (struct pan_image_block_size){
         .width = 16 / util_format_get_blockwidth(format),
         .height = 16 / util_format_get_blockheight(format),
      };
   }
}

/*
 * Determine the block size used for interleaving. For u-interleaving, this is
 * the tile size. For AFBC, this is the superblock size. For AFRC, this is the
 * paging tile size. For linear textures, this is trivially 1x1.
 */
struct pan_image_block_size
pan_image_block_size_el(uint64_t modifier, enum pipe_format format,
                        unsigned plane_idx)
{
   if (modifier == DRM_FORMAT_MOD_ARM_16X16_BLOCK_U_INTERLEAVED) {
      return pan_u_interleaved_tile_size_el(format);
   } else if (drm_is_afbc(modifier)) {
      struct pan_image_block_size sb_size_px =
         pan_afbc_superblock_size(modifier);

      assert(sb_size_px.width % util_format_get_blockwidth(format) == 0);
      assert(sb_size_px.height % util_format_get_blockheight(format) == 0);

      return (struct pan_image_block_size){
         .width = sb_size_px.width / util_format_get_blockwidth(format),
         .height = sb_size_px.height / util_format_get_blockheight(format),
      };
   } else if (drm_is_afrc(modifier)) {
      assert(util_format_get_blockwidth(format) == 1 &&
             util_format_get_blockheight(format) == 1);
      return pan_afrc_tile_size(format, modifier);
   } else {
      assert(util_format_is_compressed(format) ||
             util_format_get_blockheight(format) == 1);
      return (struct pan_image_block_size){1, 1};
   }
}

/* For non-AFBC and non-wide AFBC, the render block size matches
 * the block size, but for wide AFBC, the GPU wants the block height
 * to be 16 pixels high.
 */
struct pan_image_block_size
pan_image_renderblock_size_el(uint64_t modifier, enum pipe_format format,
                              unsigned plane_idx)
{
   if (!drm_is_afbc(modifier))
      return pan_image_block_size_el(modifier, format, plane_idx);

   struct pan_image_block_size rb_size_px = pan_afbc_renderblock_size(modifier);

   assert(rb_size_px.width % util_format_get_blockwidth(format) == 0);
   assert(rb_size_px.height % util_format_get_blockheight(format) == 0);

   return (struct pan_image_block_size){
      .width = rb_size_px.width / util_format_get_blockwidth(format),
      .height = rb_size_px.height / util_format_get_blockheight(format),
   };
}

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

static unsigned
get_plane_blocksize(enum pipe_format format, unsigned plane_idx)
{
   switch (format) {
   case PIPE_FORMAT_R8_G8B8_420_UNORM:
   case PIPE_FORMAT_R8_B8G8_420_UNORM:
   case PIPE_FORMAT_R8_G8B8_422_UNORM:
   case PIPE_FORMAT_R8_B8G8_422_UNORM:
      return plane_idx ? 2 : 1;
   case PIPE_FORMAT_R10_G10B10_420_UNORM:
   case PIPE_FORMAT_R10_G10B10_422_UNORM:
      return plane_idx ? 10 : 5;
   case PIPE_FORMAT_R8_G8_B8_420_UNORM:
   case PIPE_FORMAT_R8_B8_G8_420_UNORM:
      return 1;
   default:
      assert(util_format_get_num_planes(format) == 1);
      return util_format_get_blocksize(format);
   }
}

unsigned
pan_image_get_wsi_row_pitch(const struct pan_image_props *props,
                            unsigned plane_idx,
                            const struct pan_image_layout *layout,
                            unsigned level)
{
   const unsigned row_stride_B = layout->slices[level].row_stride_B;
   const struct pan_image_block_size block_size_el =
      pan_image_renderblock_size_el(props->modifier, props->format, plane_idx);

   if (drm_is_afbc(props->modifier)) {
      const struct pan_image_block_size afbc_tile_extent_el =
         pan_image_block_size_el(props->modifier, props->format, plane_idx);
      const unsigned afbc_tile_payload_size_B =
         afbc_tile_extent_el.width * afbc_tile_extent_el.height *
         get_plane_blocksize(props->format, plane_idx);
      const unsigned afbc_tile_row_payload_size_B =
         pan_afbc_stride_blocks(props->modifier, row_stride_B) *
         afbc_tile_payload_size_B;

      return afbc_tile_row_payload_size_B /
             pan_afbc_superblock_height(props->modifier);
   }

   if (drm_is_afrc(props->modifier)) {
      const struct pan_image_block_size tile_size_px =
         pan_afrc_tile_size(props->format, props->modifier);

      return row_stride_B / tile_size_px.height;
   }

   return row_stride_B / block_size_el.height;
}

static bool
wsi_row_pitch_to_row_stride(
   unsigned arch, const struct pan_image_props *props, unsigned plane_idx,
   const struct pan_image_layout_constraints *layout_constraints,
   unsigned *row_stride_B)
{
   unsigned row_align_mask, offset_align_mask, width_px;
   unsigned wsi_row_pitch_B = layout_constraints->wsi_row_pitch_B;
   enum pipe_format format = props->format;
   uint64_t modifier = props->modifier;

   if (drm_is_afbc(modifier)) {
      struct pan_image_block_size afbc_tile_extent_px =
         pan_afbc_superblock_size(modifier);
      /* YUV packed formats can have a block extent bigger than one pixel,
       * but the block extent must be a multiple of the tile extent. */
      assert(
         !(afbc_tile_extent_px.width % util_format_get_blockwidth(format)) &&
         !(afbc_tile_extent_px.height % util_format_get_blockheight(format)));
      unsigned pixels_per_blk = util_format_get_blockwidth(format) *
                                util_format_get_blockheight(format);
      unsigned pixels_per_tile = afbc_tile_extent_px.width *
                                 afbc_tile_extent_px.height;
      unsigned blks_per_tile = pixels_per_tile / pixels_per_blk;
      unsigned afbc_tile_payload_size_B =
         blks_per_tile * get_plane_blocksize(format, plane_idx);
      unsigned afbc_tile_payload_row_stride_B =
         wsi_row_pitch_B * afbc_tile_extent_px.height;

      offset_align_mask = pan_afbc_header_align(arch, modifier) - 1;
      row_align_mask =
         pan_afbc_header_row_stride_align(arch, format, modifier) - 1;
      width_px =
         (afbc_tile_payload_row_stride_B / afbc_tile_payload_size_B) *
         afbc_tile_extent_px.width;
      *row_stride_B = pan_afbc_row_stride(modifier, width_px);

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
   } else if (drm_is_afrc(modifier)) {
      struct pan_image_block_size tile_size_px =
         pan_afrc_tile_size(format, modifier);
      unsigned afrc_blk_size_B =
         pan_afrc_block_size_from_modifier(props->modifier) *
         AFRC_CLUMPS_PER_TILE;

      row_align_mask = pan_afrc_buffer_alignment_from_modifier(modifier) - 1;
      offset_align_mask = row_align_mask;
      *row_stride_B = wsi_row_pitch_B * tile_size_px.height;
      width_px = (*row_stride_B / afrc_blk_size_B) * tile_size_px.width;
   } else {
      struct pan_image_block_size block_size_el =
         pan_image_renderblock_size_el(modifier, format, plane_idx);

      if (!util_format_is_compressed(format)) {
         /* Block-based YUV needs special care, because the U-tile extent
          * is in pixels, not blocks in that case. */
         if (block_size_el.width * block_size_el.height > 1) {
            assert(block_size_el.width % util_format_get_blockwidth(format) ==
                   0);
            block_size_el.width /= util_format_get_blockwidth(format);
            assert(block_size_el.height % util_format_get_blockheight(format) ==
                   0);
            block_size_el.height /= util_format_get_blockheight(format);
         } else {
            block_size_el.width = util_format_get_blockwidth(format);
            assert(util_format_get_blockheight(format) == 1);
         }
      }

      unsigned tile_size_B = block_size_el.width * block_size_el.height *
                             get_plane_blocksize(format, plane_idx);

      row_align_mask =
         pan_linear_or_tiled_row_align_req(arch, format, plane_idx) - 1;
      offset_align_mask = row_align_mask;
      *row_stride_B = wsi_row_pitch_B * block_size_el.height;
      width_px = (*row_stride_B / tile_size_B) *
                 (block_size_el.width * util_format_get_blockwidth(format));
   }

   if (*row_stride_B & row_align_mask) {
      mesa_loge("WSI pitch not properly aligned");
      return false;
   }

   if (layout_constraints->offset_B & offset_align_mask) {
      mesa_loge("WSI offset not properly aligned");
      return false;
   }

   unsigned min_width_px =
      util_format_get_plane_width(format, plane_idx, props->extent_px.width);
   if (width_px < min_width_px) {
      mesa_loge("WSI pitch too small");
      return false;
   }

   return true;
}

bool
pan_image_layout_init(
   unsigned arch, const struct pan_image_props *props, unsigned plane_idx,
   const struct pan_image_layout_constraints *layout_constraints,
   struct pan_image_layout *layout)
{
   /* Use explicit layout only when wsi_row_pitch_B is non-zero */
   const bool use_explicit_layout =
      layout_constraints && layout_constraints->wsi_row_pitch_B;

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

   const bool afbc = drm_is_afbc(props->modifier);
   const bool afrc = drm_is_afrc(props->modifier);
   int align_req_B;

   if (afbc) {
      align_req_B =
         pan_afbc_header_row_stride_align(arch, props->format, props->modifier);
   } else if (afrc) {
      align_req_B = pan_afrc_buffer_alignment_from_modifier(props->modifier);
   } else {
      /* This is the alignment for non-explicit layout, and we want things
       * aligned on at least a cacheline for performance reasons in that case.
       */
      align_req_B =
         pan_linear_or_tiled_row_align_req(arch, props->format, plane_idx);
      align_req_B = MAX2(align_req_B, 64);
   }

   /* Mandate alignment */
   unsigned wsi_row_stride_B = 0;
   if (use_explicit_layout) {
      if (!wsi_row_pitch_to_row_stride(arch, props, plane_idx,
                                       layout_constraints, &wsi_row_stride_B))
         return false;
   }

   const unsigned fmt_blocksize_B =
      get_plane_blocksize(props->format, plane_idx);

   /* MSAA is implemented as a 3D texture with z corresponding to the
    * sample #, horrifyingly enough */

   assert(props->extent_px.depth == 1 || props->nr_samples == 1);

   const bool linear = props->modifier == DRM_FORMAT_MOD_LINEAR;
   const bool is_3d = props->dim == MALI_TEXTURE_DIMENSION_3D;

   const struct pan_image_block_size renderblk_size_el =
      pan_image_renderblock_size_el(props->modifier, props->format, plane_idx);
   const struct pan_image_block_size block_size_el =
      pan_image_block_size_el(props->modifier, props->format, plane_idx);

   unsigned width_px = util_format_get_plane_width(props->format, plane_idx,
                                                   props->extent_px.width);
   unsigned height_px = util_format_get_plane_height(props->format, plane_idx,
                                                     props->extent_px.height);
   unsigned blk_width_px = util_format_get_blockwidth(props->format);
   unsigned blk_height_px = util_format_get_blockheight(props->format);
   unsigned depth_px = props->extent_px.depth;

   unsigned align_w_el = renderblk_size_el.width;
   unsigned align_h_el = renderblk_size_el.height;

   /* For tiled AFBC, align to tiles of superblocks (this can be large) */
   if (afbc) {
      align_w_el *= pan_afbc_tile_size(props->modifier);
      align_h_el *= pan_afbc_tile_size(props->modifier);
   }

   uint64_t offset_B = layout_constraints ? layout_constraints->offset_B : 0;
   for (unsigned l = 0; l < props->nr_slices; ++l) {
      struct pan_image_slice_layout *slice = &layout->slices[l];

      const unsigned effective_width_el =
         ALIGN_POT(DIV_ROUND_UP(width_px, blk_width_px), align_w_el);
      const unsigned effective_height_el =
         ALIGN_POT(DIV_ROUND_UP(height_px, blk_height_px), align_h_el);
      const unsigned effective_width_px = effective_width_el * blk_width_px;
      const unsigned effective_height_px = effective_height_el * blk_height_px;
      unsigned row_stride_B;

      /* Align levels to cache-line as a performance improvement for
       * linear/tiled and as a requirement for AFBC */

      if (!use_explicit_layout)
         offset_B = ALIGN_POT(offset_B, pan_image_slice_align(props->modifier));

      slice->offset_B = offset_B;

      if (afrc) {
         row_stride_B = pan_afrc_row_stride(props->format, props->modifier,
                                            effective_width_px);
      } else {
         row_stride_B =
            fmt_blocksize_B * effective_width_el * block_size_el.height;
      }

      /* On v7+ row_stride and offset alignment requirement are equal */
      if (arch >= 7) {
         row_stride_B = ALIGN_POT(row_stride_B, align_req_B);
      }

      if (use_explicit_layout && !afbc) {
         /* Explicit stride should be rejected by wsi_row_pitch_to_row_stride()
          * if it's too small. */
         assert(wsi_row_stride_B >= row_stride_B);

         if (!afrc || layout_constraints->strict)
            row_stride_B = wsi_row_stride_B;
      } else if (linear) {
         /* Keep lines alignment on 64 byte for performance */
         row_stride_B = ALIGN_POT(row_stride_B, 64);
      }

      uint64_t slice_one_size_B =
         (uint64_t)row_stride_B * (effective_height_el / block_size_el.height);

      /* Compute AFBC sizes if necessary */
      if (afbc) {
         slice->row_stride_B =
            pan_afbc_row_stride(props->modifier, effective_width_px);

         /* Explicit stride should be rejected by wsi_row_pitch_to_row_stride()
          * if it's too small. */
         assert(!use_explicit_layout ||
                wsi_row_stride_B >= slice->row_stride_B);

         if (use_explicit_layout && layout_constraints->strict) {
            slice->row_stride_B = wsi_row_stride_B;
            slice_one_size_B = (uint64_t)layout_constraints->wsi_row_pitch_B *
                               effective_height_el;
         }

         const unsigned stride_sb =
            pan_afbc_stride_blocks(props->modifier, slice->row_stride_B);
         const unsigned nr_sblocks =
            stride_sb * (effective_height_px / block_size_el.height);

         slice->afbc.header_size_B =
            ALIGN_POT(nr_sblocks * AFBC_HEADER_BYTES_PER_TILE,
                      pan_afbc_body_align(arch, props->modifier));

         /* AFBC body size */
         slice->afbc.body_size_B = slice_one_size_B;

         /* 3D AFBC resources have all headers placed at the
          * beginning instead of having them split per depth
          * level
          */
         if (is_3d) {
            slice->afbc.surface_stride_B = slice->afbc.header_size_B;
            slice->afbc.header_size_B *= depth_px;
            slice->afbc.body_size_B *= depth_px;
            offset_B += slice->afbc.header_size_B;
         } else {
            slice_one_size_B += slice->afbc.header_size_B;
            slice->afbc.surface_stride_B = slice_one_size_B;
         }
      } else {
         slice->row_stride_B = row_stride_B;
      }

      const uint64_t slice_full_size_B =
         slice_one_size_B * depth_px * props->nr_samples;

      slice->surface_stride_B = slice_one_size_B;

      /* Compute AFBC sizes if necessary */

      offset_B += slice_full_size_B;

      /* We can't use slice_full_size_B for AFBC(3D), otherwise the headers are
       * not counted. */
      if (afbc)
         slice->size_B = slice->afbc.body_size_B + slice->afbc.header_size_B;
      else
         slice->size_B = slice_full_size_B;

      /* Add a checksum region if necessary */
      if (props->crc) {
         init_slice_crc_info(arch, slice, width_px, height_px, offset_B);
         offset_B += slice->crc.size_B;
         slice->size_B += slice->crc.size_B;
      }

      width_px = u_minify(width_px, 1);
      height_px = u_minify(height_px, 1);
      depth_px = u_minify(depth_px, 1);
   }

   /* Arrays and cubemaps have the entire miptree duplicated */
   layout->array_stride_B =
      ALIGN_POT(offset_B - layout->slices[0].offset_B, 64);
   if (use_explicit_layout) {
      layout->data_size_B = offset_B - layout_constraints->offset_B;
   } else {
      /* Native images start from offset 0, and the planar plane offset has
       * been at least 4K page aligned below. So the base level slice offset
       * should always be the same with the plane offset.
       */
      assert(!layout_constraints ||
             layout_constraints->offset_B == layout->slices[0].offset_B);
      layout->data_size_B = ALIGN_POT(
         (uint64_t)layout->array_stride_B * (uint64_t)props->array_size, 4096);
   }

   return true;
}
