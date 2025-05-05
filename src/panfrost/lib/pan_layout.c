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

static inline unsigned
format_minimum_alignment(unsigned arch, enum pipe_format format, uint64_t mod)
{
   if (drm_is_afbc(mod))
      return 16;

   if (drm_is_afrc(mod))
      return pan_afrc_buffer_alignment_from_modifier(mod);

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
pan_image_surface_stride(const struct pan_image_layout *layout, unsigned level)
{
   if (layout->dim != MALI_TEXTURE_DIMENSION_3D)
      return layout->array_stride_B;
   else if (drm_is_afbc(layout->modifier))
      return layout->slices[level].afbc.surface_stride_B;
   else
      return layout->slices[level].surface_stride_B;
}

struct pan_image_wsi_layout
pan_image_layout_get_wsi_layout(const struct pan_image_layout *layout,
                                unsigned level)
{
   unsigned row_stride_B = layout->slices[level].row_stride_B;
   struct pan_image_block_size block_size_el =
      pan_image_renderblock_size_el(layout->modifier, layout->format);

   if (drm_is_afbc(layout->modifier)) {
      unsigned width_px = u_minify(layout->extent_px.width, level);
      unsigned alignment_B =
         block_size_el.width * pan_afbc_tile_size(layout->modifier);

      width_px = ALIGN_POT(width_px, alignment_B);
      return (struct pan_image_wsi_layout){
         .offset_B = layout->slices[level].offset_B,
         .row_pitch_B = width_px * util_format_get_blocksize(layout->format),
      };
   } else if (drm_is_afrc(layout->modifier)) {
      struct pan_image_block_size tile_size_px =
         pan_afrc_tile_size(layout->format, layout->modifier);

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

static unsigned
wsi_row_pitch_to_row_stride(unsigned wsi_row_pitch_B, enum pipe_format format,
                            uint64_t modifier)
{
   if (drm_is_afbc(modifier)) {
      unsigned width_px = wsi_row_pitch_B / util_format_get_blocksize(format);

      return pan_afbc_row_stride(modifier, width_px);
   } else if (drm_is_afrc(modifier)) {
      struct pan_image_block_size tile_size_px =
         pan_afrc_tile_size(format, modifier);

      return wsi_row_pitch_B * tile_size_px.height;
   } else {
      struct pan_image_block_size block_size_el =
         pan_image_renderblock_size_el(modifier, format);

      return wsi_row_pitch_B * block_size_el.height;
   }
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
pan_image_layout_init(unsigned arch, struct pan_image_layout *layout,
                      const struct pan_image_wsi_layout *wsi_layout)
{
   /* Explicit stride only work with non-mipmap, non-array, single-sample
    * 2D image without CRC.
    */
   if (wsi_layout &&
       (layout->extent_px.depth > 1 || layout->nr_samples > 1 ||
        layout->array_size > 1 || layout->dim != MALI_TEXTURE_DIMENSION_2D ||
        layout->nr_slices > 1 || layout->crc))
      return false;

   bool afbc = drm_is_afbc(layout->modifier);
   bool afrc = drm_is_afrc(layout->modifier);
   int align_req_B =
      format_minimum_alignment(arch, layout->format, layout->modifier);
   uint64_t offset_B = 0, wsi_row_stride_B = 0;

   /* Mandate alignment */
   if (wsi_layout) {
      bool rejected = false;

      int align_mask = align_req_B - 1;

      offset_B = wsi_layout->offset_B;
      wsi_row_stride_B = wsi_row_pitch_to_row_stride(
         wsi_layout->row_pitch_B, layout->format, layout->modifier);

      if (arch >= 7) {
         rejected =
            ((offset_B & align_mask) || (wsi_row_stride_B & align_mask));
      } else {
         rejected = (offset_B & align_mask);
      }

      if (rejected) {
         mesa_loge(
            "panfrost: rejecting image due to unsupported offset or stride "
            "alignment.\n");
         return false;
      }
   }

   unsigned fmt_blocksize_B = util_format_get_blocksize(layout->format);

   /* MSAA is implemented as a 3D texture with z corresponding to the
    * sample #, horrifyingly enough */

   assert(layout->extent_px.depth == 1 || layout->nr_samples == 1);

   bool linear = layout->modifier == DRM_FORMAT_MOD_LINEAR;
   bool is_3d = layout->dim == MALI_TEXTURE_DIMENSION_3D;

   struct pan_image_block_size renderblk_size_el =
      pan_image_renderblock_size_el(layout->modifier, layout->format);
   struct pan_image_block_size block_size_el =
      pan_image_block_size_el(layout->modifier, layout->format);

   unsigned width_px = layout->extent_px.width;
   unsigned height_px = layout->extent_px.height;
   unsigned depth_px = layout->extent_px.depth;

   unsigned align_w_el = renderblk_size_el.width;
   unsigned align_h_el = renderblk_size_el.height;

   /* For tiled AFBC, align to tiles of superblocks (this can be large) */
   if (afbc) {
      align_w_el *= pan_afbc_tile_size(layout->modifier);
      align_h_el *= pan_afbc_tile_size(layout->modifier);
   }

   for (unsigned l = 0; l < layout->nr_slices; ++l) {
      struct pan_image_slice_layout *slice = &layout->slices[l];

      unsigned effective_width_el = ALIGN_POT(
         util_format_get_nblocksx(layout->format, width_px), align_w_el);
      unsigned effective_height_el = ALIGN_POT(
         util_format_get_nblocksy(layout->format, height_px), align_h_el);
      unsigned row_stride_B;

      /* Align levels to cache-line as a performance improvement for
       * linear/tiled and as a requirement for AFBC */

      offset_B = ALIGN_POT(offset_B, pan_image_slice_align(layout->modifier));

      slice->offset_B = offset_B;

      if (afrc) {
         row_stride_B = pan_afrc_row_stride(layout->format, layout->modifier,
                                            effective_width_el);
      } else {
         row_stride_B =
            fmt_blocksize_B * effective_width_el * block_size_el.height;
      }

      /* On v7+ row_stride and offset alignment requirement are equal */
      if (arch >= 7) {
         row_stride_B = ALIGN_POT(row_stride_B, align_req_B);
      }

      if (wsi_layout && !afbc && !afrc) {
         /* Make sure the explicit stride is valid */
         if (wsi_row_stride_B < row_stride_B) {
            mesa_loge("panfrost: rejecting image due to invalid row stride.\n");
            return false;
         }

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
            pan_afbc_row_stride(layout->modifier, effective_width_el);
         slice->afbc.stride_sb = effective_width_el / block_size_el.width;
         slice->afbc.nr_sblocks = slice->afbc.stride_sb *
                                  (effective_height_el / block_size_el.height);
         slice->afbc.header_size_B =
            ALIGN_POT(slice->afbc.nr_sblocks * AFBC_HEADER_BYTES_PER_TILE,
                      pan_afbc_body_align(arch, layout->modifier));

         if (wsi_layout && wsi_row_stride_B < slice->row_stride_B) {
            mesa_loge("panfrost: rejecting image due to invalid row stride.\n");
            return false;
         }

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
         slice_one_size_B * depth_px * layout->nr_samples;

      slice->surface_stride_B = slice_one_size_B;

      /* Compute AFBC sizes if necessary */

      offset_B += slice_full_size_B;
      slice->size_B = slice_full_size_B;

      /* Add a checksum region if necessary */
      if (layout->crc) {
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
         (uint64_t)layout->array_stride_B * (uint64_t)layout->array_size, 4096);

   return true;
}
