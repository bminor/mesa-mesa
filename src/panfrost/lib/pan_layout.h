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
   unsigned offset;

   /* For AFBC images, the number of bytes between two rows of AFBC
    * headers.
    *
    * For non-AFBC images, the number of bytes between two rows of texels.
    * For linear images, this will equal the logical stride. For
    * images that are compressed or interleaved, this will be greater than
    * the logical stride.
    */
   unsigned row_stride;

   unsigned surface_stride;

   struct {
      /* Stride in number of superblocks */
      unsigned stride;

      /* Number of superblocks */
      unsigned nr_blocks;

      /* Size of the AFBC header preceding each slice */
      unsigned header_size;

      /* Size of the AFBC body */
      unsigned body_size;

      /* Stride between AFBC headers of two consecutive surfaces.
       * For 3D textures, this must be set to header size since
       * AFBC headers are allocated together, for 2D arrays this
       * should be set to size0, since AFBC headers are placed at
       * the beginning of each layer
       */
      unsigned surface_stride;
   } afbc;

   /* If checksumming is enabled following the slice, what
    * is its offset/stride? */
   struct {
      unsigned offset;
      unsigned stride;
      unsigned size;
   } crc;

   unsigned size;
};

struct pan_image_layout {
   uint64_t modifier;
   enum pipe_format format;
   unsigned width, height, depth;
   unsigned nr_samples;
   enum mali_texture_dimension dim;
   unsigned nr_slices;
   unsigned array_size;
   bool crc;

   /* The remaining fields may be derived from the above by calling
    * pan_image_layout_init
    */

   struct pan_image_slice_layout slices[MAX_MIP_LEVELS];

   uint64_t data_size;
   uint64_t array_stride;
};

struct pan_image_wsi_layout {
   unsigned offset_B;
   unsigned row_pitch_B;
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

struct pan_image_block_size pan_image_block_size(uint64_t modifier,
                                                 enum pipe_format format);

struct pan_image_block_size pan_image_renderblock_size(uint64_t modifier,
                                                       enum pipe_format format);

unsigned pan_image_surface_stride(const struct pan_image_layout *layout,
                                  unsigned level);

unsigned pan_image_surface_offset(const struct pan_image_layout *layout,
                                  unsigned level, unsigned array_idx,
                                  unsigned surface_idx);

bool
pan_image_layout_init(unsigned arch, struct pan_image_layout *layout,
                      const struct pan_image_wsi_layout *wsi_layout);

struct pan_image_wsi_layout
pan_image_layout_get_wsi_layout(const struct pan_image_layout *layout,
                                unsigned level);

#ifdef __cplusplus
} /* extern C */
#endif

#endif
