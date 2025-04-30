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
pan_u_interleaved_tile_size(enum pipe_format format)
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
pan_image_block_size(uint64_t modifier, enum pipe_format format)
{
   if (modifier == DRM_FORMAT_MOD_ARM_16X16_BLOCK_U_INTERLEAVED)
      return pan_u_interleaved_tile_size(format);
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
pan_image_renderblock_size(uint64_t modifier, enum pipe_format format)
{
   if (!drm_is_afbc(modifier))
      return pan_image_block_size(modifier, format);

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
                    unsigned width, unsigned height, unsigned offset)
{
   unsigned checksum_region_size = pan_meta_tile_size(arch);
   unsigned checksum_x_tile_per_region =
      (checksum_region_size / CHECKSUM_TILE_WIDTH);
   unsigned checksum_y_tile_per_region =
      (checksum_region_size / CHECKSUM_TILE_HEIGHT);

   unsigned tile_count_x =
      checksum_x_tile_per_region * DIV_ROUND_UP(width, checksum_region_size);
   unsigned tile_count_y =
      checksum_y_tile_per_region * DIV_ROUND_UP(height, checksum_region_size);

   slice->crc.offset = offset;
   slice->crc.stride = tile_count_x * CHECKSUM_BYTES_PER_TILE;
   slice->crc.size = slice->crc.stride * tile_count_y;
}

unsigned
pan_image_surface_stride(const struct pan_image_layout *layout, unsigned level)
{
   if (layout->dim != MALI_TEXTURE_DIMENSION_3D)
      return layout->array_stride;
   else if (drm_is_afbc(layout->modifier))
      return layout->slices[level].afbc.surface_stride;
   else
      return layout->slices[level].surface_stride;
}

struct pan_image_wsi_layout
pan_image_layout_get_wsi_layout(const struct pan_image_layout *layout,
                                unsigned level)
{
   unsigned row_stride = layout->slices[level].row_stride;
   struct pan_image_block_size block_size =
      pan_image_renderblock_size(layout->modifier, layout->format);

   if (drm_is_afbc(layout->modifier)) {
      unsigned width = u_minify(layout->width, level);
      unsigned alignment =
         block_size.width * pan_afbc_tile_size(layout->modifier);

      width = ALIGN_POT(width, alignment);
      return (struct pan_image_wsi_layout){
         .offset_B = layout->slices[level].offset,
         .row_pitch_B = width * util_format_get_blocksize(layout->format),
      };
   } else if (drm_is_afrc(layout->modifier)) {
      struct pan_image_block_size tile_size =
         pan_afrc_tile_size(layout->format, layout->modifier);

      return (struct pan_image_wsi_layout){
         .offset_B = layout->slices[level].offset,
         .row_pitch_B = row_stride / tile_size.height,
      };
   } else {
      return (struct pan_image_wsi_layout){
         .offset_B = layout->slices[level].offset,
         .row_pitch_B = row_stride / block_size.height,
      };
   }
}

static unsigned
wsi_row_pitch_to_row_stride(unsigned wsi_row_pitch, enum pipe_format format,
                            uint64_t modifier)
{
   struct pan_image_block_size block_size =
      pan_image_renderblock_size(modifier, format);

   if (drm_is_afbc(modifier)) {
      unsigned width = wsi_row_pitch / util_format_get_blocksize(format);

      return pan_afbc_row_stride(modifier, width);
   } else if (drm_is_afrc(modifier)) {
      struct pan_image_block_size tile_size = pan_afrc_tile_size(format, modifier);

      return wsi_row_pitch * tile_size.height;
   } else {
      return wsi_row_pitch * block_size.height;
   }
}

/* Computes the offset of an image surface at a particular level/face. Add to
 * the base address of a texture to get the address to that level/face */

unsigned
pan_image_surface_offset(const struct pan_image_layout *layout, unsigned level,
                         unsigned array_idx, unsigned surface_idx)
{
   return layout->slices[level].offset + (array_idx * layout->array_stride) +
          (surface_idx * layout->slices[level].surface_stride);
}

bool
pan_image_layout_init(unsigned arch, struct pan_image_layout *layout,
                      const struct pan_image_wsi_layout *wsi_layout)
{
   /* Explicit stride only work with non-mipmap, non-array, single-sample
    * 2D image without CRC.
    */
   if (wsi_layout &&
       (layout->depth > 1 || layout->nr_samples > 1 || layout->array_size > 1 ||
        layout->dim != MALI_TEXTURE_DIMENSION_2D || layout->nr_slices > 1 ||
        layout->crc))
      return false;

   bool afbc = drm_is_afbc(layout->modifier);
   bool afrc = drm_is_afrc(layout->modifier);
   int align_req =
      format_minimum_alignment(arch, layout->format, layout->modifier);
   uint64_t offset = 0, wsi_row_stride = 0;

   /* Mandate alignment */
   if (wsi_layout) {
      bool rejected = false;

      int align_mask = align_req - 1;

      offset = wsi_layout->offset_B;
      wsi_row_stride = wsi_row_pitch_to_row_stride(
         wsi_layout->row_pitch_B, layout->format, layout->modifier);

      if (arch >= 7) {
         rejected = ((offset & align_mask) ||
                     (wsi_row_stride & align_mask));
      } else {
         rejected = (offset & align_mask);
      }

      if (rejected) {
         mesa_loge(
            "panfrost: rejecting image due to unsupported offset or stride "
            "alignment.\n");
         return false;
      }
   }

   unsigned fmt_blocksize = util_format_get_blocksize(layout->format);

   /* MSAA is implemented as a 3D texture with z corresponding to the
    * sample #, horrifyingly enough */

   assert(layout->depth == 1 || layout->nr_samples == 1);

   bool linear = layout->modifier == DRM_FORMAT_MOD_LINEAR;
   bool is_3d = layout->dim == MALI_TEXTURE_DIMENSION_3D;

   struct pan_image_block_size renderblk_size =
      pan_image_renderblock_size(layout->modifier, layout->format);
   struct pan_image_block_size block_size =
      pan_image_block_size(layout->modifier, layout->format);

   unsigned width = layout->width;
   unsigned height = layout->height;
   unsigned depth = layout->depth;

   unsigned align_w = renderblk_size.width;
   unsigned align_h = renderblk_size.height;

   /* For tiled AFBC, align to tiles of superblocks (this can be large) */
   if (afbc) {
      align_w *= pan_afbc_tile_size(layout->modifier);
      align_h *= pan_afbc_tile_size(layout->modifier);
   }

   for (unsigned l = 0; l < layout->nr_slices; ++l) {
      struct pan_image_slice_layout *slice = &layout->slices[l];

      unsigned effective_width =
         ALIGN_POT(util_format_get_nblocksx(layout->format, width), align_w);
      unsigned effective_height =
         ALIGN_POT(util_format_get_nblocksy(layout->format, height), align_h);
      unsigned row_stride;

      /* Align levels to cache-line as a performance improvement for
       * linear/tiled and as a requirement for AFBC */

      offset = ALIGN_POT(offset, pan_image_slice_align(layout->modifier));

      slice->offset = offset;

      if (afrc) {
         row_stride = pan_afrc_row_stride(layout->format, layout->modifier,
                                          effective_width);
      } else {
         row_stride = fmt_blocksize * effective_width * block_size.height;
      }

      /* On v7+ row_stride and offset alignment requirement are equal */
      if (arch >= 7) {
         row_stride = ALIGN_POT(row_stride, align_req);
      }

      if (wsi_layout && !afbc && !afrc) {
         /* Make sure the explicit stride is valid */
         if (wsi_row_stride < row_stride) {
            mesa_loge("panfrost: rejecting image due to invalid row stride.\n");
            return false;
         }

         row_stride = wsi_row_stride;
      } else if (linear) {
         /* Keep lines alignment on 64 byte for performance */
         row_stride = ALIGN_POT(row_stride, 64);
      }

      uint64_t slice_one_size =
         (uint64_t)row_stride * (effective_height / block_size.height);

      /* Compute AFBC sizes if necessary */
      if (afbc) {
         slice->row_stride =
            pan_afbc_row_stride(layout->modifier, effective_width);
         slice->afbc.stride = effective_width / block_size.width;
         slice->afbc.nr_blocks =
            slice->afbc.stride * (effective_height / block_size.height);
         slice->afbc.header_size =
            ALIGN_POT(slice->afbc.nr_blocks * AFBC_HEADER_BYTES_PER_TILE,
                      pan_afbc_body_align(arch, layout->modifier));

         if (wsi_layout && wsi_row_stride < slice->row_stride) {
            mesa_loge("panfrost: rejecting image due to invalid row stride.\n");
            return false;
         }

         /* AFBC body size */
         slice->afbc.body_size = slice_one_size;

         /* 3D AFBC resources have all headers placed at the
          * beginning instead of having them split per depth
          * level
          */
         if (is_3d) {
            slice->afbc.surface_stride = slice->afbc.header_size;
            slice->afbc.header_size *= depth;
            slice->afbc.body_size *= depth;
            offset += slice->afbc.header_size;
         } else {
            slice_one_size += slice->afbc.header_size;
            slice->afbc.surface_stride = slice_one_size;
         }
      } else {
         slice->row_stride = row_stride;
      }

      uint64_t slice_full_size = slice_one_size * depth * layout->nr_samples;

      slice->surface_stride = slice_one_size;

      /* Compute AFBC sizes if necessary */

      offset += slice_full_size;
      slice->size = slice_full_size;

      /* Add a checksum region if necessary */
      if (layout->crc) {
         init_slice_crc_info(arch, slice, width, height, offset);
         offset += slice->crc.size;
         slice->size += slice->crc.size;
      }

      width = u_minify(width, 1);
      height = u_minify(height, 1);
      depth = u_minify(depth, 1);
   }

   /* Arrays and cubemaps have the entire miptree duplicated */
   layout->array_stride = ALIGN_POT(offset, 64);
   if (wsi_layout)
      layout->data_size = offset;
   else
      layout->data_size = ALIGN_POT(
         (uint64_t)layout->array_stride * (uint64_t)layout->array_size, 4096);

   return true;
}
