/*
 * Copyright (C) 2008 VMware, Inc.
 * Copyright (C) 2014 Broadcom
 * Copyright (C) 2018-2019 Alyssa Rosenzweig
 * Copyright (C) 2019-2020 Collabora, Ltd.
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef __PAN_LAYOUT_H
#define __PAN_LAYOUT_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdbool.h>

#include "genxml/gen_macros.h"

#define MAX_MIP_LEVELS   17
#define MAX_IMAGE_PLANES 3

struct pan_image_slice_layout {
   /* Offset in bytes relative to the base bo bound.
    *
    * Unlike gallium, vulkan has to report explicit image subres layout which
    * disallows to hide the planar plane offset into the bo mapping. So we let
    * the slice offsets to include the plane offset of the native multi-planar
    * images to be consistent with the imported ones via explicit layout info.
    * Doing so allows us to use a single code path to correctly:
    * - report image subres layout and memory requirement
    * - bind image memory
    */
   uint64_t offset_B;

   /* Size of the MIP level in bytes. */
   uint64_t size_B;

   /* For AFBC images, the number of bytes between two rows of AFBC
    * headers.
    *
    * For non-AFBC images, the number of bytes between two rows of texels.
    * For linear images, this will equal the logical stride. For
    * images that are compressed or interleaved, this will be greater than
    * the logical stride.
    */
   unsigned row_stride_B;

   unsigned surface_stride_B;

   struct {
      /* Size of the AFBC header preceding each slice */
      unsigned header_size_B;

      /* Size of the AFBC body */
      unsigned body_size_B;

      /* Stride between AFBC headers of two consecutive surfaces.
       * For 3D textures, this must be set to header size since
       * AFBC headers are allocated together, for 2D arrays this
       * should be set to size0, since AFBC headers are placed at
       * the beginning of each layer
       */
      unsigned surface_stride_B;
   } afbc;

   /* If checksumming is enabled following the slice, what
    * is its offset/stride? */
   struct {
      uint64_t offset_B;
      unsigned stride_B;
      unsigned size_B;
   } crc;
};

struct pan_image_extent {
   unsigned width;
   unsigned height;
   unsigned depth;
};

struct pan_image_props {
   uint64_t modifier;
   enum pipe_format format;
   struct pan_image_extent extent_px;
   unsigned nr_samples;
   enum mali_texture_dimension dim;
   unsigned nr_slices;
   unsigned array_size;
   bool crc;
};

struct pan_image_layout {
   struct pan_image_slice_layout slices[MAX_MIP_LEVELS];

   /* Image plane data size in bytes */
   uint64_t data_size_B;
   uint64_t array_stride_B;
};

struct pan_image_layout_constraints {
   /*
    * Plane offset in bytes
    * - For native images, it's the planar plane offset.
    * - For imported images, it's the user specified explicit offset.
    *
    * To be noted, this offset might be adjusted to choose an optimal alignment,
    * unless the layout constraints are explicit (wsi_row_patch_B != 0).
    */
   uint64_t offset_B;

   /* Row pitch in bytes. Non-zero if layout is explicit. */
   unsigned wsi_row_pitch_B;
   bool strict;
};

/*
 * Represents the block size of a single plane. For AFBC, this represents the
 * superblock size. For u-interleaving, this represents the tile size.
 */
struct pan_image_block_size {
   /** Width of block */
   unsigned width;

   /** Height of blocks */
   unsigned height;
};

/*
 * Determine the required alignment for the slice offset of an image. For
 * now, this is always aligned on 64-byte boundaries. */
static inline uint32_t
pan_image_slice_align(uint64_t modifier)
{
   return 64;
}

struct pan_image_block_size pan_image_block_size_el(uint64_t modifier,
                                                    enum pipe_format format,
                                                    unsigned plane_idx);

struct pan_image_block_size
pan_image_renderblock_size_el(uint64_t modifier, enum pipe_format format,
                              unsigned plane_idx);

unsigned pan_image_surface_stride(const struct pan_image_props *props,
                                  const struct pan_image_layout *layout,
                                  unsigned level);

unsigned pan_image_surface_offset(const struct pan_image_layout *layout,
                                  unsigned level, unsigned array_idx,
                                  unsigned surface_idx);

static inline uint64_t
pan_image_mip_level_size(const struct pan_image_props *props,
                         const struct pan_image_layout *layout, unsigned level)
{
   assert(level < props->nr_slices);
   uint64_t size = layout->slices[level].size_B;

   /* If this is an array, we need to cover the whole array. */
   if (props->array_size > 1)
      size += layout->array_stride_B * (props->array_size - 1);

   return size;
}

bool pan_image_layout_init(
   unsigned arch, const struct pan_image_props *props, unsigned plane_idx,
   const struct pan_image_layout_constraints *layout_constraints,
   struct pan_image_layout *layout);

static inline uint64_t
pan_image_get_wsi_offset(const struct pan_image_layout *layout, unsigned level)
{
   return layout->slices[level].offset_B;
}

unsigned pan_image_get_wsi_row_pitch(const struct pan_image_props *props,
                                     unsigned plane_idx,
                                     const struct pan_image_layout *layout,
                                     unsigned level);

static inline unsigned
pan_linear_or_tiled_row_align_req(unsigned arch, enum pipe_format format,
                                  unsigned plane_idx)
{
   if (arch < 7) {
      unsigned nplanes = util_format_get_num_planes(format);

      /* If this is a planar format, align on the plane blocksize. */
      if (nplanes > 1) {
         enum pipe_format plane_format =
            util_format_get_plane_format(format, plane_idx);

         return util_next_power_of_two(util_format_get_blocksize(plane_format));
      }

      /* Align on blocksize if the format is compressed. */
      if (util_format_is_compressed(format))
         return util_next_power_of_two(util_format_get_blocksize(format));

      const struct util_format_description *fdesc =
         util_format_description(format);
      unsigned comp_sz_bits = 0;
      for (unsigned i = 0; i < ARRAY_SIZE(fdesc->channel); i++) {
         if (!fdesc->channel[0].size)
            continue;

         /* Align on a pixel if any component is not 8-bit aligned or not a
          * power of two. */
         if (fdesc->channel[0].size % 8 != 0 ||
             !util_is_power_of_two_nonzero(fdesc->channel[0].size))
            return util_next_power_of_two(util_format_get_blocksize(format));

         /* Align on a pixel if not all components have the same size. */
         if (comp_sz_bits != 0 && comp_sz_bits != fdesc->channel[0].size)
            return util_next_power_of_two(util_format_get_blocksize(format));

         comp_sz_bits = fdesc->channel[0].size;
      }

      /* If all components are the same size, 8-bit aligned and a power of two,
       * align on a component. */
      return comp_sz_bits / 8;
   }

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

#ifdef __cplusplus
} /* extern C */
#endif

#endif
