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
   if (util_format_is_compressed(format))
      return (struct pan_image_block_size){4, 4};
   else
      return (struct pan_image_block_size){16, 16};
}

/*
 * Determine the block size used for interleaving. For u-interleaving, this is
 * the tile size. For AFBC, this is the superblock size. For AFRC, this is the
 * paging tile size. For linear textures, this is trivially 1x1.
 */
struct pan_image_block_size
pan_image_block_size_el(uint64_t modifier, enum pipe_format format)
{
   if (modifier == DRM_FORMAT_MOD_ARM_16X16_BLOCK_U_INTERLEAVED)
      return pan_u_interleaved_tile_size_el(format);
   else if (drm_is_afbc(modifier))
      return pan_afbc_superblock_size(modifier);
   else if (drm_is_afrc(modifier))
      return pan_afrc_tile_size(format, modifier);
   else
      return (struct pan_image_block_size){1, 1};
}

/* For non-AFBC and non-wide AFBC, the render block size matches
 * the block size, but for wide AFBC, the GPU wants the block height
 * to be 16 pixels high.
 */
struct pan_image_block_size
pan_image_renderblock_size_el(uint64_t modifier, enum pipe_format format)
{
   if (!drm_is_afbc(modifier))
      return pan_image_block_size_el(modifier, format);

   return pan_afbc_renderblock_size(modifier);
}

static unsigned
linear_or_tiled_row_align_req(unsigned arch, enum pipe_format format,
                              uint64_t modifier)
{
   assert(modifier == DRM_FORMAT_MOD_ARM_16X16_BLOCK_U_INTERLEAVED ||
          modifier == DRM_FORMAT_MOD_LINEAR);

   /* Prior to v7 we assume a cacheline alignment, though this could be relaxed
    * on some formats if we have to, like we do on v7+. */
   if (arch < 7)
      return 64;

   switch (format) {
   /* For v7+, NV12/NV21/I420 have a looser alignment requirement of 16 bytes */
   case PIPE_FORMAT_R8_G8B8_420_UNORM:
   case PIPE_FORMAT_G8_B8R8_420_UNORM:
   case PIPE_FORMAT_R8_G8_B8_420_UNORM:
   case PIPE_FORMAT_R8_B8_G8_420_UNORM:
   case PIPE_FORMAT_R8_G8B8_422_UNORM:
   case PIPE_FORMAT_R8_B8G8_422_UNORM:
      return 16;
   /* the 10 bit formats have even looser alignment */
   case PIPE_FORMAT_R10_G10B10_420_UNORM:
   case PIPE_FORMAT_R10_G10B10_422_UNORM:
      return 1;
   default:
      return 64;
   }
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
                    unsigned width_px, unsigned height_px, unsigned offset_B)
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

unsigned
pan_image_surface_stride(const struct pan_image_props *props,
                         const struct pan_image_layout *layout, unsigned level)
{
   if (props->dim != MALI_TEXTURE_DIMENSION_3D)
      return layout->array_stride_B;
   else if (drm_is_afbc(props->modifier))
      return layout->slices[level].afbc.surface_stride_B;
   else
      return layout->slices[level].surface_stride_B;
}

struct pan_image_wsi_layout
pan_image_layout_get_wsi_layout(const struct pan_image_props *props,
                                const struct pan_image_layout *layout,
                                unsigned level)
{
   unsigned row_stride_B = layout->slices[level].row_stride_B;
   struct pan_image_block_size block_size_el =
      pan_image_renderblock_size_el(props->modifier, props->format);

   if (drm_is_afbc(props->modifier)) {
      unsigned width_px = u_minify(props->extent_px.width, level);
      unsigned alignment_B =
         block_size_el.width * pan_afbc_tile_size(props->modifier);

      width_px = ALIGN_POT(width_px, alignment_B);
      return (struct pan_image_wsi_layout){
         .offset_B = layout->slices[level].offset_B,
         .row_pitch_B = width_px * util_format_get_blocksize(props->format),
      };
   } else if (drm_is_afrc(props->modifier)) {
      struct pan_image_block_size tile_size_px =
         pan_afrc_tile_size(props->format, props->modifier);

      return (struct pan_image_wsi_layout){
         .offset_B = layout->slices[level].offset_B,
         .row_pitch_B = row_stride_B / tile_size_px.height,
      };
   } else {
      return (struct pan_image_wsi_layout){
         .offset_B = layout->slices[level].offset_B,
         .row_pitch_B = row_stride_B / block_size_el.height,
      };
   }
}

static bool
wsi_row_pitch_to_row_stride(unsigned arch, const struct pan_image_props *props,
                            const struct pan_image_wsi_layout *wsi_layout,
                            unsigned *row_stride_B)
{
   unsigned row_align_mask, offset_align_mask, width_px;
   unsigned wsi_row_pitch_B = wsi_layout->row_pitch_B;
   enum pipe_format format = props->format;
   uint64_t modifier = props->modifier;

   if (drm_is_afbc(modifier)) {
      /* We assume single pixel blocks in AFBC. */
      assert(util_format_get_blockwidth(format) == 1 &&
             util_format_get_blockheight(format) == 1);

      struct pan_image_block_size afbc_tile_extent_px =
         pan_afbc_renderblock_size(modifier);
      unsigned afbc_tile_payload_size_B = afbc_tile_extent_px.width *
                                          afbc_tile_extent_px.height *
                                          util_format_get_blocksize(format);
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
      if (wsi_layout->strict &&
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
         pan_image_renderblock_size_el(modifier, format);
      unsigned tile_size_B = block_size_el.width * block_size_el.height *
                             util_format_get_blocksize(format);

      /* The row_stride_B -> width_px conversion is assuming a 1x1 pixel
       * block size for non-compressed formats. Revisit when adding support
       * for block-based YUV. */
      assert(util_format_is_compressed(format) ||
             (util_format_get_blockwidth(format) == 1 &&
              util_format_get_blockheight(format) == 1));

      row_align_mask =
         linear_or_tiled_row_align_req(arch, format, modifier) - 1;
      offset_align_mask = row_align_mask;
      *row_stride_B = wsi_row_pitch_B * block_size_el.height;
      width_px = (*row_stride_B / tile_size_B) *
                 (block_size_el.width * util_format_get_blockwidth(format));
   }

   if (*row_stride_B & row_align_mask) {
      mesa_loge("WSI pitch not properly aligned");
      return false;
   }

   if (wsi_layout->offset_B & offset_align_mask) {
      mesa_loge("WSI offset not properly aligned");
      return false;
   }

   if (width_px < props->extent_px.width) {
      mesa_loge("WSI pitch too small");
      return false;
   }

   return true;
}

/* Computes the offset of an image surface at a particular level/face. Add to
 * the base address of a texture to get the address to that level/face */

unsigned
pan_image_surface_offset(const struct pan_image_layout *layout, unsigned level,
                         unsigned array_idx, unsigned surface_idx)
{
   return layout->slices[level].offset_B +
          (array_idx * layout->array_stride_B) +
          (surface_idx * layout->slices[level].surface_stride_B);
}

bool
pan_image_layout_init(unsigned arch, const struct pan_image_props *props,
                      const struct pan_image_wsi_layout *wsi_layout,
                      struct pan_image_layout *layout)
{
   /* Explicit stride only work with non-mipmap, non-array, single-sample
    * 2D image without CRC.
    */
   if (wsi_layout &&
       (props->extent_px.depth > 1 || props->nr_samples > 1 ||
        props->array_size > 1 || props->dim != MALI_TEXTURE_DIMENSION_2D ||
        props->nr_slices > 1 || props->crc))
      return false;

   bool afbc = drm_is_afbc(props->modifier);
   bool afrc = drm_is_afrc(props->modifier);
   int align_req_B =
      afbc ? pan_afbc_header_row_stride_align(arch, props->format,
                                              props->modifier)
      : afrc
         ? pan_afrc_buffer_alignment_from_modifier(props->modifier)
         : linear_or_tiled_row_align_req(arch, props->format, props->modifier);
   unsigned wsi_row_stride_B = 0;
   uint64_t offset_B = 0;

   /* Mandate alignment */
   if (wsi_layout) {
      if (!wsi_row_pitch_to_row_stride(arch, props, wsi_layout,
                                       &wsi_row_stride_B))
         return false;

      offset_B = wsi_layout->offset_B;
   }

   unsigned fmt_blocksize_B = util_format_get_blocksize(props->format);

   /* MSAA is implemented as a 3D texture with z corresponding to the
    * sample #, horrifyingly enough */

   assert(props->extent_px.depth == 1 || props->nr_samples == 1);

   bool linear = props->modifier == DRM_FORMAT_MOD_LINEAR;
   bool is_3d = props->dim == MALI_TEXTURE_DIMENSION_3D;

   struct pan_image_block_size renderblk_size_el =
      pan_image_renderblock_size_el(props->modifier, props->format);
   struct pan_image_block_size block_size_el =
      pan_image_block_size_el(props->modifier, props->format);

   unsigned width_px = props->extent_px.width;
   unsigned height_px = props->extent_px.height;
   unsigned depth_px = props->extent_px.depth;

   unsigned align_w_el = renderblk_size_el.width;
   unsigned align_h_el = renderblk_size_el.height;

   /* For tiled AFBC, align to tiles of superblocks (this can be large) */
   if (afbc) {
      align_w_el *= pan_afbc_tile_size(props->modifier);
      align_h_el *= pan_afbc_tile_size(props->modifier);
   }

   for (unsigned l = 0; l < props->nr_slices; ++l) {
      struct pan_image_slice_layout *slice = &layout->slices[l];

      unsigned effective_width_el = ALIGN_POT(
         util_format_get_nblocksx(props->format, width_px), align_w_el);
      unsigned effective_height_el = ALIGN_POT(
         util_format_get_nblocksy(props->format, height_px), align_h_el);
      unsigned row_stride_B;

      /* Align levels to cache-line as a performance improvement for
       * linear/tiled and as a requirement for AFBC */

      if (!wsi_layout)
         offset_B = ALIGN_POT(offset_B, pan_image_slice_align(props->modifier));

      slice->offset_B = offset_B;

      if (afrc) {
         row_stride_B = pan_afrc_row_stride(props->format, props->modifier,
                                            effective_width_el);
      } else {
         row_stride_B =
            fmt_blocksize_B * effective_width_el * block_size_el.height;
      }

      /* On v7+ row_stride and offset alignment requirement are equal */
      if (arch >= 7) {
         row_stride_B = ALIGN_POT(row_stride_B, align_req_B);
      }

      if (wsi_layout && !afbc) {
         /* Explicit stride should be rejected by wsi_row_pitch_to_row_stride()
          * if it's too small. */
         assert(wsi_row_stride_B >= row_stride_B);

         if (!afrc || wsi_layout->strict)
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
            pan_afbc_row_stride(props->modifier, effective_width_el);

         /* Explicit stride should be rejected by wsi_row_pitch_to_row_stride()
          * if it's too small. */
         assert(!wsi_layout || wsi_row_stride_B >= slice->row_stride_B);

         if (wsi_layout && wsi_layout->strict) {
            slice->row_stride_B = wsi_row_stride_B;
            slice_one_size_B = (uint64_t)wsi_layout->row_pitch_B * effective_height_el;
         }

         slice->afbc.stride_sb =
            pan_afbc_stride_blocks(props->modifier, slice->row_stride_B);
         slice->afbc.nr_sblocks = slice->afbc.stride_sb *
                                  (effective_height_el / block_size_el.height);
         slice->afbc.header_size_B =
            ALIGN_POT(slice->afbc.nr_sblocks * AFBC_HEADER_BYTES_PER_TILE,
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

      uint64_t slice_full_size_B =
         slice_one_size_B * depth_px * props->nr_samples;

      slice->surface_stride_B = slice_one_size_B;

      /* Compute AFBC sizes if necessary */

      offset_B += slice_full_size_B;
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
   layout->array_stride_B = ALIGN_POT(offset_B, 64);
   if (wsi_layout)
      layout->data_size_B = offset_B;
   else
      layout->data_size_B = ALIGN_POT(
         (uint64_t)layout->array_stride_B * (uint64_t)props->array_size, 4096);

   return true;
}
