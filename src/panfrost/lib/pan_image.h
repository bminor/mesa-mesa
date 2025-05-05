/*
 * Copyright (C) 2008 VMware, Inc.
 * Copyright (C) 2014 Broadcom
 * Copyright (C) 2018-2019 Alyssa Rosenzweig
 * Copyright (C) 2019-2020 Collabora, Ltd.
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef __PAN_IMAGE_H
#define __PAN_IMAGE_H

#include "genxml/gen_macros.h"

#include <stdbool.h>
#include "util/format/u_format.h"
#include "pan_format.h"
#include "pan_layout.h"

#ifdef __cplusplus
extern "C" {
#endif

struct pan_image_mem {
   uint64_t base;
};

struct pan_image {
   struct pan_image_mem data;
   struct pan_image_layout layout;
};

struct pan_image_surface {
   union {
      uint64_t data;
      struct {
         uint64_t header;
         uint64_t body;
      } afbc;
   };
};

struct pan_image_view {
   /* Format, dimension and sample count of the view might differ from
    * those of the image (2D view of a 3D image surface for instance).
    */
   enum pipe_format format;
   enum mali_texture_dimension dim;
   unsigned first_level, last_level;
   unsigned first_layer, last_layer;
   unsigned char swizzle[4];

   /* planes 1 and 2 are NULL for single plane formats */
   const struct pan_image *planes[MAX_IMAGE_PLANES];

   /* If EXT_multisampled_render_to_texture is used, this may be
    * greater than image->layout.nr_samples. */
   unsigned nr_samples;

   /* Only valid if dim == 1D, needed to implement buffer views */
   struct {
      unsigned offset;
      unsigned size;
   } buf;

   struct {
      unsigned narrow;
   } astc;
};

static inline const struct pan_image *
pan_image_view_get_plane(const struct pan_image_view *iview, uint32_t idx)
{
   if (idx >= ARRAY_SIZE(iview->planes))
      return NULL;

   return iview->planes[idx];
}

static inline unsigned
pan_image_view_get_plane_mask(const struct pan_image_view *iview)
{
   unsigned mask = 0;

   for (unsigned i = 0; i < ARRAY_SIZE(iview->planes); i++) {
      if (iview->planes[i])
         mask |= BITFIELD_BIT(i);
   }

   return mask;
}

static inline unsigned
pan_image_view_get_first_plane_idx(const struct pan_image_view *iview)
{
   unsigned mask = pan_image_view_get_plane_mask(iview);

   assert(mask);
   return ffs(mask) - 1;
}

static inline const struct pan_image *
pan_image_view_get_first_plane(const struct pan_image_view *iview)
{
   unsigned first_plane_idx = pan_image_view_get_first_plane_idx(iview);
   return pan_image_view_get_plane(iview, first_plane_idx);
}

static inline uint32_t
pan_image_view_get_nr_samples(const struct pan_image_view *iview)
{
   const struct pan_image *image = pan_image_view_get_first_plane(iview);

   if (!image)
      return 0;

   return image->layout.nr_samples;
}

static inline const struct pan_image *
pan_image_view_get_color_plane(const struct pan_image_view *iview)
{
   /* We only support rendering to plane 0 */
   assert(pan_image_view_get_plane(iview, 1) == NULL);
   return pan_image_view_get_plane(iview, 0);
}

static inline bool
pan_image_view_has_crc(const struct pan_image_view *iview)
{
   const struct pan_image *image = pan_image_view_get_color_plane(iview);

   if (!image)
      return false;

   return image->layout.crc;
}

static inline const struct pan_image *
pan_image_view_get_s_plane(const struct pan_image_view *iview)
{
   ASSERTED const struct util_format_description *fdesc =
      util_format_description(iview->format);
   assert(util_format_has_stencil(fdesc));

   /* In case of multiplanar depth/stencil, the stencil is always on
    * plane 1. Combined depth/stencil only has one plane, so depth
    * will be on plane 0 in either case.
    */
   const struct pan_image *plane = iview->planes[1] ?: iview->planes[0];

   assert(plane);
   fdesc = util_format_description(plane->layout.format);
   assert(util_format_has_stencil(fdesc));
   return plane;
}

static inline const struct pan_image *
pan_image_view_get_zs_plane(const struct pan_image_view *iview)
{
   assert(util_format_is_depth_or_stencil(iview->format));

   /* Depth or combined depth-stencil is always on plane 0. */
   return pan_image_view_get_plane(iview, 0);
}

static inline void
pan_iview_get_surface(const struct pan_image_view *iview, unsigned level,
                      unsigned layer, unsigned sample,
                      struct pan_image_surface *surf)
{
   const struct util_format_description *fdesc =
      util_format_description(iview->format);

   /* In case of multiplanar depth/stencil, the stencil is always on
    * plane 1. Combined depth/stencil only has one plane, so depth
    * will be on plane 0 in either case.
    */
   const struct pan_image *image = util_format_has_stencil(fdesc)
                                      ? pan_image_view_get_s_plane(iview)
                                      : pan_image_view_get_plane(iview, 0);

   level += iview->first_level;
   assert(level < image->layout.nr_slices);

   layer += iview->first_layer;

   bool is_3d = image->layout.dim == MALI_TEXTURE_DIMENSION_3D;
   const struct pan_image_slice_layout *slice = &image->layout.slices[level];
   uint64_t base = image->data.base;

   memset(surf, 0, sizeof(*surf));

   if (drm_is_afbc(image->layout.modifier)) {
      assert(!sample);

      if (is_3d) {
         ASSERTED unsigned depth = u_minify(image->layout.depth, level);
         assert(layer < depth);
         surf->afbc.header =
            base + slice->offset + (layer * slice->afbc.surface_stride);
         surf->afbc.body = base + slice->offset + slice->afbc.header_size +
                           (slice->surface_stride * layer);
      } else {
         assert(layer < image->layout.array_size);
         surf->afbc.header =
            base + pan_image_surface_offset(&image->layout, level, layer, 0);
         surf->afbc.body = surf->afbc.header + slice->afbc.header_size;
      }
   } else {
      unsigned array_idx = is_3d ? 0 : layer;
      unsigned surface_idx = is_3d ? layer : sample;

      surf->data = base + pan_image_surface_offset(&image->layout, level,
                                                   array_idx, surface_idx);
   }
}

#ifdef __cplusplus
} /* extern C */
#endif

#endif
