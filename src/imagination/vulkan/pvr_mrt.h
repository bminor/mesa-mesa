/*
 * Copyright © 2025 Imagination Technologies Ltd.
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef PVR_MRT_H
#define PVR_MRT_H

#include <stdbool.h>
#include <stdint.h>
#include <vulkan/vulkan.h>

struct pvr_device;

/* Specifies the location of render target writes. */
enum usc_mrt_resource_type {
   USC_MRT_RESOURCE_TYPE_INVALID = 0, /* explicitly treat 0 as invalid. */
   USC_MRT_RESOURCE_TYPE_OUTPUT_REG,
   USC_MRT_RESOURCE_TYPE_MEMORY,
};

#define PVR_USC_RENDER_TARGET_MAXIMUM_SIZE_IN_DWORDS (4)

struct usc_mrt_desc {
   /* Size (in bytes) of the intermediate storage required for each pixel in the
    * render target.
    */
   uint32_t intermediate_size;

   /* Mask of the bits from each dword which are read by the PBE. */
   uint32_t valid_mask[PVR_USC_RENDER_TARGET_MAXIMUM_SIZE_IN_DWORDS];

   /* Higher number = higher priority. Used to decide which render targets get
    * allocated dedicated output registers.
    */
   uint32_t priority;
};

struct usc_mrt_resource {
   /* Input description of render target. */
   struct usc_mrt_desc mrt_desc;

   /* Resource type allocated for render target. */
   enum usc_mrt_resource_type type;

   /* Intermediate pixel size (in bytes). */
   uint32_t intermediate_size;

   union {
      /* If type == USC_MRT_RESOURCE_TYPE_OUTPUT_REG. */
      struct {
         /* The output register to use. */
         uint32_t output_reg;

         /* The offset in bytes into the output register. */
         uint32_t offset;
      } reg;

      /* If type == USC_MRT_RESOURCE_TYPE_MEMORY. */
      struct {
         /* The index of the tile buffer to use. */
         uint32_t tile_buffer;

         /* The offset in dwords within the tile buffer. */
         uint32_t offset_dw;
      } mem;
   };
};

struct usc_mrt_setup {
   /* Number of render targets present. */
   uint32_t num_render_targets;

   /* Number of output registers used per-pixel (1, 2 or 4). */
   uint32_t num_output_regs;

   /* Number of tile buffers used. */
   uint32_t num_tile_buffers;

   /* Array of MRT resources allocated for each render target. The number of
    * elements is determined by usc_mrt_setup::num_render_targets.
    */
   struct usc_mrt_resource *mrt_resources;

   /* Don't set up source pos in emit. */
   bool disable_source_pos_override;

   /* Hash unique to this particular setup. */
   uint32_t hash;
};

VkResult
pvr_init_usc_mrt_setup(struct pvr_device *device,
                       uint32_t attachment_count,
                       const VkFormat attachment_formats[attachment_count],
                       struct usc_mrt_setup *setup);

void
pvr_destroy_mrt_setup(const struct pvr_device *device,
                      struct usc_mrt_setup *setup);

void pvr_init_mrt_desc(VkFormat format, struct usc_mrt_desc *desc);

#endif
