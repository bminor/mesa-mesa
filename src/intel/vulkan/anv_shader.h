/* Copyright © 2024 Intel Corporation
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include "anv_private.h"
#include "anv_nir.h"

#include "nir/nir_xfb_info.h"

struct anv_shader_data {
   struct vk_shader_compile_info *info;

   struct vk_shader **shader_out;

   union brw_any_prog_key key;
   uint32_t key_size;

   union brw_any_prog_data prog_data;

   uint32_t source_hash;

   const nir_xfb_info *xfb_info;

   uint32_t num_stats;
   struct brw_compile_stats stats[3];
   char *disasm[3];

   bool use_primitive_replication;
   uint32_t instance_multiplier;

   /* For fragment shaders only */
   struct brw_mue_map *mue_map;

   struct anv_push_descriptor_info push_desc_info;

   struct anv_pipeline_bind_map bind_map;

   struct anv_pipeline_push_map push_map;

   bool uses_bt_for_push_descs;

   unsigned *code;
};

VkResult anv_shader_create(struct anv_device *device,
                           mesa_shader_stage stage,
                           void *mem_ctx,
                           struct anv_shader_data *shader_data,
                           const VkAllocationCallbacks *pAllocator,
                           struct vk_shader **shader_out);

VkResult anv_shader_deserialize(struct vk_device *device,
                                struct blob_reader *blob,
                                uint32_t binary_version,
                                const VkAllocationCallbacks* pAllocator,
                                struct vk_shader **shader_out);

extern struct vk_device_shader_ops anv_device_shader_ops;
