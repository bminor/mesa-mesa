/*
 * Copyright © 2024 Collabora Ltd.
 * SPDX-License-Identifier: MIT
 */

#ifndef PANVK_CMD_DRAW_H
#define PANVK_CMD_DRAW_H

#ifndef PAN_ARCH
#error "PAN_ARCH must be defined"
#endif

#include "panvk_blend.h"
#include "panvk_physical_device.h"

#include "pan_props.h"

#define MAX_VBS 16
#define MAX_RTS 8

struct panvk_attrib_buf {
   mali_ptr address;
   unsigned size;
};

struct panvk_resolve_attachment {
   VkResolveModeFlagBits mode;
   struct panvk_image_view *dst_iview;
};

struct panvk_rendering_state {
   VkRenderingFlags flags;
   uint32_t layer_count;

   enum vk_rp_attachment_flags bound_attachments;
   struct {
      struct panvk_image_view *iviews[MAX_RTS];
      VkFormat fmts[MAX_RTS];
      uint8_t samples[MAX_RTS];
      struct panvk_resolve_attachment resolve[MAX_RTS];
   } color_attachments;

   struct pan_image_view zs_pview;

   struct {
      struct panvk_image_view *iview;
      VkFormat fmt;
      struct panvk_resolve_attachment resolve;
   } z_attachment, s_attachment;

   struct {
      struct pan_fb_info info;
      bool crc_valid[MAX_RTS];

#if PAN_ARCH <= 7
      uint32_t bo_count;
      struct pan_kmod_bo *bos[MAX_RTS + 2];
#endif
   } fb;

#if PAN_ARCH >= 10
   struct panfrost_ptr fbds;
   mali_ptr tiler;
   bool dirty;
#endif
};

struct panvk_cmd_graphics_state {
   struct panvk_descriptor_state desc_state;

   struct {
      struct vk_vertex_input_state vi;
      struct vk_sample_locations_state sl;
   } dynamic;

   struct panvk_graphics_sysvals sysvals;

   struct panvk_shader_link link;
   bool linked;

   struct {
      const struct panvk_shader *shader;
      struct panvk_shader_desc_state desc;
#if PAN_ARCH <= 7
      mali_ptr rsd;
#else
      mali_ptr spd;
#endif
   } fs;

   struct {
      const struct panvk_shader *shader;
      struct panvk_shader_desc_state desc;
#if PAN_ARCH <= 7
      mali_ptr attribs;
      mali_ptr attrib_bufs;
#else
      struct {
         mali_ptr pos, var;
      } spds;
#endif
   } vs;

   struct {
      struct panvk_attrib_buf bufs[MAX_VBS];
      unsigned count;
      bool dirty;
   } vb;

   /* Index buffer */
   struct {
      struct panvk_buffer *buffer;
      uint64_t offset;
      uint8_t index_size;
      bool dirty;
   } ib;

   struct {
      struct panvk_blend_info info;
   } cb;

   struct panvk_rendering_state render;

   mali_ptr push_uniforms;

#if PAN_ARCH <= 7
   mali_ptr vpd;
#endif

#if PAN_ARCH >= 10
   mali_ptr tsd;
#endif
};

static inline uint32_t
panvk_select_tiler_hierarchy_mask(const struct panvk_physical_device *phys_dev,
                                  const struct panvk_cmd_graphics_state *state)
{
   struct panfrost_tiler_features tiler_features =
      panfrost_query_tiler_features(&phys_dev->kmod.props);
   uint32_t max_fb_wh = MAX2(state->render.fb.info.width,
                             state->render.fb.info.height);
   uint32_t last_hierarchy_bit = util_last_bit(DIV_ROUND_UP(max_fb_wh, 16));
   uint32_t hierarchy_mask = BITFIELD_MASK(tiler_features.max_levels);

   /* Always enable the level covering the whole FB, and disable the finest
    * levels if we don't have enough to cover everything.
    * This is suboptimal for small primitives, since it might force
    * primitives to be walked multiple times even if they don't cover the
    * the tile being processed. On the other hand, it's hard to guess
    * the draw pattern, so it's probably good enough for now.
    */
   if (last_hierarchy_bit > tiler_features.max_levels)
      hierarchy_mask <<= last_hierarchy_bit - tiler_features.max_levels;

   return hierarchy_mask;
}

#endif
