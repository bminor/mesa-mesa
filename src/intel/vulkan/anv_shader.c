/* Copyright © 2024 Intel Corporation
 * SPDX-License-Identifier: MIT
 */

#include "anv_private.h"
#include "anv_shader.h"

#include "nir/nir_serialize.h"

static void
anv_shader_destroy(struct vk_device *vk_device,
                   struct vk_shader *vk_shader,
                   const VkAllocationCallbacks *pAllocator)
{
   struct anv_device *device =
      container_of(vk_device, struct anv_device, vk);
   struct anv_shader *shader =
      container_of(vk_shader, struct anv_shader, vk);

   for (uint32_t i = 0; i < shader->bind_map.embedded_sampler_count; i++)
      anv_embedded_sampler_unref(device, shader->embedded_samplers[i]);

   anv_state_pool_free(&device->instruction_state_pool, shader->kernel);
   vk_shader_free(vk_device, pAllocator, vk_shader);
}

VkResult
anv_shader_deserialize(struct vk_device *vk_device,
                       struct blob_reader *blob,
                       uint32_t binary_version,
                       const VkAllocationCallbacks* pAllocator,
                       struct vk_shader **shader_out)
{
   struct anv_device *device = container_of(vk_device, struct anv_device, vk);

   struct anv_shader_data data = {};

   mesa_shader_stage stage = blob_read_uint32(blob);

   uint32_t code_len = blob_read_uint32(blob);
   data.code = (void *)blob_read_bytes(blob, code_len);

   blob_copy_bytes(blob, &data.prog_data, brw_prog_data_size(stage));

   data.prog_data.base.relocs =
      blob_read_bytes(blob, data.prog_data.base.num_relocs *
                      sizeof(data.prog_data.base.relocs[0]));

   data.num_stats = blob_read_uint32(blob);
   blob_copy_bytes(blob, data.stats, data.num_stats * sizeof(data.stats[0]));

   uint32_t xfb_size = blob_read_uint32(blob);
   if (xfb_size)
      data.xfb_info = blob_read_bytes(blob, xfb_size);

   data.instance_multiplier = blob_read_uint32(blob);

   data.push_desc_info.used_descriptors = blob_read_uint32(blob);
   data.push_desc_info.fully_promoted_ubo_descriptors = blob_read_uint32(blob);
   data.push_desc_info.push_set_buffer = blob_read_uint8(blob);

   blob_copy_bytes(blob, data.bind_map.surface_sha1, sizeof(data.bind_map.surface_sha1));
   blob_copy_bytes(blob, data.bind_map.sampler_sha1, sizeof(data.bind_map.sampler_sha1));
   blob_copy_bytes(blob, data.bind_map.push_sha1, sizeof(data.bind_map.push_sha1));
   data.bind_map.layout_type = blob_read_uint32(blob);
   data.bind_map.surface_count = blob_read_uint32(blob);
   data.bind_map.sampler_count = blob_read_uint32(blob);
   data.bind_map.embedded_sampler_count = blob_read_uint32(blob);
   data.bind_map.surface_to_descriptor = (void *)
      blob_read_bytes(blob, data.bind_map.surface_count *
                            sizeof(*data.bind_map.surface_to_descriptor));
   data.bind_map.sampler_to_descriptor = (void *)
      blob_read_bytes(blob, data.bind_map.sampler_count *
                            sizeof(*data.bind_map.sampler_to_descriptor));
   data.bind_map.embedded_sampler_to_binding = (void *)
      blob_read_bytes(blob, data.bind_map.embedded_sampler_count *
                            sizeof(*data.bind_map.embedded_sampler_to_binding));
   blob_copy_bytes(blob, data.bind_map.input_attachments,
                   sizeof(data.bind_map.input_attachments));
   blob_copy_bytes(blob, data.bind_map.push_ranges, sizeof(data.bind_map.push_ranges));

   if (blob->overrun)
      return vk_error(device, VK_ERROR_UNKNOWN);

   VkResult result =
      anv_shader_create(device, stage, &data, pAllocator, shader_out);

   return result;
}

static bool
anv_shader_serialize(struct vk_device *device,
                     const struct vk_shader *vk_shader,
                     struct blob *blob)
{
   struct anv_shader *shader =
      container_of(vk_shader, struct anv_shader, vk);

   blob_write_uint32(blob, vk_shader->stage);

   blob_write_uint32(blob, shader->prog_data->program_size);
   blob_write_bytes(blob, shader->kernel.map,
                    shader->prog_data->program_size);

   union brw_any_prog_data prog_data;
   memcpy(&prog_data, shader->prog_data, brw_prog_data_size(vk_shader->stage));
   prog_data.base.relocs = NULL;
   prog_data.base.param = NULL;

   blob_write_bytes(blob, &prog_data, brw_prog_data_size(vk_shader->stage));

   blob_write_bytes(blob, shader->prog_data->relocs,
                    shader->prog_data->num_relocs *
                    sizeof(shader->prog_data->relocs[0]));

   blob_write_uint32(blob, shader->num_stats);
   blob_write_bytes(blob, shader->stats,
                    shader->num_stats * sizeof(shader->stats[0]));

   if (shader->xfb_info) {
      uint32_t xfb_info_size =
         nir_xfb_info_size(shader->xfb_info->output_count);
      blob_write_uint32(blob, xfb_info_size);
      blob_write_bytes(blob, shader->xfb_info, xfb_info_size);
   } else {
      blob_write_uint32(blob, 0);
   }

   blob_write_uint32(blob, shader->instance_multiplier);

   blob_write_uint32(blob, shader->push_desc_info.used_descriptors);
   blob_write_uint32(blob, shader->push_desc_info.fully_promoted_ubo_descriptors);
   blob_write_uint8(blob, shader->push_desc_info.push_set_buffer);

   blob_write_bytes(blob, shader->bind_map.surface_sha1,
                    sizeof(shader->bind_map.surface_sha1));
   blob_write_bytes(blob, shader->bind_map.sampler_sha1,
                    sizeof(shader->bind_map.sampler_sha1));
   blob_write_bytes(blob, shader->bind_map.push_sha1,
                    sizeof(shader->bind_map.push_sha1));
   blob_write_uint32(blob, shader->bind_map.layout_type);
   blob_write_uint32(blob, shader->bind_map.surface_count);
   blob_write_uint32(blob, shader->bind_map.sampler_count);
   blob_write_uint32(blob, shader->bind_map.embedded_sampler_count);
   blob_write_bytes(blob, shader->bind_map.surface_to_descriptor,
                    shader->bind_map.surface_count *
                    sizeof(*shader->bind_map.surface_to_descriptor));
   blob_write_bytes(blob, shader->bind_map.sampler_to_descriptor,
                    shader->bind_map.sampler_count *
                    sizeof(*shader->bind_map.sampler_to_descriptor));
   blob_write_bytes(blob, shader->bind_map.embedded_sampler_to_binding,
                    shader->bind_map.embedded_sampler_count *
                    sizeof(*shader->bind_map.embedded_sampler_to_binding));
   blob_write_bytes(blob, shader->bind_map.input_attachments,
                    sizeof(shader->bind_map.input_attachments));
   blob_write_bytes(blob, shader->bind_map.push_ranges,
                    sizeof(shader->bind_map.push_ranges));

   return !blob->out_of_memory;
}

static VkResult
anv_shader_get_executable_properties(struct vk_device *device,
                                     const struct vk_shader *shader,
                                     uint32_t *executable_count,
                                     VkPipelineExecutablePropertiesKHR *properties)
{
   return VK_SUCCESS;
}

static VkResult
anv_shader_get_executable_statistics(struct vk_device *device,
                                     const struct vk_shader *shader,
                                     uint32_t executable_index,
                                     uint32_t *statistic_count,
                                     VkPipelineExecutableStatisticKHR *statistics)
{
   return VK_SUCCESS;
}

static VkResult
anv_shader_get_executable_internal_representations(
   struct vk_device *device,
   const struct vk_shader *shader,
   uint32_t executable_index,
   uint32_t *internal_representation_count,
   VkPipelineExecutableInternalRepresentationKHR *internal_representations)
{
   return VK_SUCCESS;
}

static struct vk_shader_ops anv_shader_ops = {
   .destroy                   = anv_shader_destroy,
   .serialize                 = anv_shader_serialize,
   .get_executable_properties = anv_shader_get_executable_properties,
   .get_executable_statistics = anv_shader_get_executable_statistics,
   .get_executable_internal_representations =
      anv_shader_get_executable_internal_representations,
};

static VkResult
anv_shader_reloc(struct anv_device *device,
                 void *code,
                 struct anv_shader *shader,
                 const VkAllocationCallbacks *pAllocator)
{
   uint64_t shader_data_addr =
      device->physical->va.instruction_state_pool.addr +
      shader->kernel.offset +
      shader->prog_data->const_data_offset;

   const uint32_t max_relocs =
      BRW_SHADER_RELOC_EMBEDDED_SAMPLER_HANDLE +
      shader->bind_map.embedded_sampler_count;
   uint32_t rv_count = 0;
   struct brw_shader_reloc_value *reloc_values =
      vk_zalloc2(&device->vk.alloc, pAllocator,
                 sizeof(struct brw_shader_reloc_value) * max_relocs, 8,
                 VK_SYSTEM_ALLOCATION_SCOPE_COMMAND);
   if (reloc_values == NULL)
      return vk_error(device, VK_ERROR_OUT_OF_HOST_MEMORY);

   assert((device->physical->va.dynamic_visible_pool.addr & 0xffffffff) == 0);
   reloc_values[rv_count++] = (struct brw_shader_reloc_value) {
      .id = BRW_SHADER_RELOC_INSTRUCTION_BASE_ADDR_HIGH,
      .value = device->physical->va.instruction_state_pool.addr >> 32,
   };
   reloc_values[rv_count++] = (struct brw_shader_reloc_value) {
      .id = BRW_SHADER_RELOC_DESCRIPTORS_BUFFER_ADDR_HIGH,
      .value = device->physical->va.dynamic_visible_pool.addr >> 32,
   };
   assert((device->physical->va.indirect_descriptor_pool.addr & 0xffffffff) == 0);
   assert((device->physical->va.internal_surface_state_pool.addr & 0xffffffff) == 0);
   reloc_values[rv_count++] = (struct brw_shader_reloc_value) {
      .id = BRW_SHADER_RELOC_DESCRIPTORS_ADDR_HIGH,
      .value = device->physical->indirect_descriptors ?
               (device->physical->va.indirect_descriptor_pool.addr >> 32) :
               (device->physical->va.internal_surface_state_pool.addr >> 32),
   };
   assert((device->physical->va.instruction_state_pool.addr & 0xffffffff) == 0);
   reloc_values[rv_count++] = (struct brw_shader_reloc_value) {
      .id = BRW_SHADER_RELOC_CONST_DATA_ADDR_LOW,
      .value = shader_data_addr,
   };
   assert((device->physical->va.instruction_state_pool.addr & 0xffffffff) == 0);
   assert(shader_data_addr >> 32 == device->physical->va.instruction_state_pool.addr >> 32);
   reloc_values[rv_count++] = (struct brw_shader_reloc_value) {
      .id = BRW_SHADER_RELOC_CONST_DATA_ADDR_HIGH,
      .value = device->physical->va.instruction_state_pool.addr >> 32,
   };
   reloc_values[rv_count++] = (struct brw_shader_reloc_value) {
      .id = BRW_SHADER_RELOC_SHADER_START_OFFSET,
      .value = shader->kernel.offset,
   };
   if (brw_shader_stage_is_bindless(shader->vk.stage)) {
      const struct brw_bs_prog_data *bs_prog_data =
         brw_bs_prog_data_const(shader->prog_data);
      uint64_t resume_sbt_addr =
         device->physical->va.instruction_state_pool.addr +
         shader->kernel.offset +
         bs_prog_data->resume_sbt_offset;
      reloc_values[rv_count++] = (struct brw_shader_reloc_value) {
         .id = BRW_SHADER_RELOC_RESUME_SBT_ADDR_LOW,
         .value = resume_sbt_addr,
      };
      reloc_values[rv_count++] = (struct brw_shader_reloc_value) {
         .id = BRW_SHADER_RELOC_RESUME_SBT_ADDR_HIGH,
         .value = resume_sbt_addr >> 32,
      };
   }

   if (INTEL_DEBUG(DEBUG_SHADER_PRINT)) {
      struct anv_bo *bo = device->printf.bo;
      assert(bo != NULL);

      reloc_values[rv_count++] = (struct brw_shader_reloc_value) {
         .id = BRW_SHADER_RELOC_PRINTF_BUFFER_ADDR_LOW,
         .value = bo->offset & 0xffffffff,
      };
      reloc_values[rv_count++] = (struct brw_shader_reloc_value) {
         .id = BRW_SHADER_RELOC_PRINTF_BUFFER_ADDR_HIGH,
         .value = bo->offset >> 32,
      };
      reloc_values[rv_count++] = (struct brw_shader_reloc_value) {
         .id = BRW_SHADER_RELOC_PRINTF_BUFFER_SIZE,
         .value = anv_printf_buffer_size(),
      };
   }

   for (uint32_t i = 0; i < shader->bind_map.embedded_sampler_count; i++) {
      reloc_values[rv_count++] = (struct brw_shader_reloc_value) {
         .id = BRW_SHADER_RELOC_EMBEDDED_SAMPLER_HANDLE + i,
         .value = shader->embedded_samplers[i]->sampler_state.offset,
      };
   }

   assert(rv_count <= max_relocs);

   brw_write_shader_relocs(&device->physical->compiler->isa,
                           code, shader->prog_data,
                           reloc_values, rv_count);

   vk_free2(&device->vk.alloc, pAllocator, reloc_values);

   return VK_SUCCESS;
}

VkResult
anv_shader_create(struct anv_device *device,
                  mesa_shader_stage stage,
                  struct anv_shader_data *shader_data,
                  const VkAllocationCallbacks *pAllocator,
                  struct vk_shader **shader_out)
{
   /* We never need this at runtime */
   shader_data->prog_data.base.param = NULL;

   VK_MULTIALLOC(ma);
   VK_MULTIALLOC_DECL(&ma, struct anv_shader, shader, 1);
   VK_MULTIALLOC_DECL_SIZE(&ma, void, obj_key_data, brw_prog_key_size(stage));
   VK_MULTIALLOC_DECL_SIZE(&ma, struct brw_stage_prog_data, prog_data,
                           brw_prog_data_size(stage));
   VK_MULTIALLOC_DECL(&ma, struct brw_shader_reloc, prog_data_relocs,
                      shader_data->prog_data.base.num_relocs);

   VK_MULTIALLOC_DECL_SIZE(&ma, nir_xfb_info, xfb_info,
                           shader_data->xfb_info == NULL ? 0 :
                           nir_xfb_info_size(shader_data->xfb_info->output_count));

   VK_MULTIALLOC_DECL(&ma, struct anv_pipeline_binding, surface_to_descriptor,
                      shader_data->bind_map.surface_count);
   VK_MULTIALLOC_DECL(&ma, struct anv_pipeline_binding, sampler_to_descriptor,
                      shader_data->bind_map.sampler_count);
   VK_MULTIALLOC_DECL(&ma, struct anv_pipeline_embedded_sampler_binding,
                      embedded_sampler_to_binding,
                      shader_data->bind_map.embedded_sampler_count);
   VK_MULTIALLOC_DECL(&ma, struct anv_embedded_sampler *, embedded_samplers,
                      shader_data->bind_map.embedded_sampler_count);

   if (!vk_shader_multizalloc(&device->vk, &ma, &anv_shader_ops,
                              stage, pAllocator))
      return vk_error(device, VK_ERROR_OUT_OF_HOST_MEMORY);

   VkResult result;
   if (shader_data->bind_map.embedded_sampler_count > 0) {
      shader->embedded_samplers = embedded_samplers;
      result = anv_device_get_embedded_samplers(
         device, embedded_samplers, &shader_data->bind_map);
      if (result != VK_SUCCESS)
         goto error_shader;
   }

   shader->kernel =
      anv_state_pool_alloc(&device->instruction_state_pool,
                           shader_data->prog_data.base.program_size, 64);
   if (shader->kernel.alloc_size == 0) {
      result = vk_error(device, VK_ERROR_OUT_OF_DEVICE_MEMORY);
      goto error_embedded_samplers;
   }

   memcpy(prog_data, &shader_data->prog_data, brw_prog_data_size(stage));

   typed_memcpy(prog_data_relocs,
                shader_data->prog_data.base.relocs,
                shader_data->prog_data.base.num_relocs);
   prog_data->relocs = prog_data_relocs;

   shader->prog_data = prog_data;

   shader->num_stats = shader_data->num_stats;
   typed_memcpy(shader->stats, shader_data->stats, shader_data->num_stats);

   if (shader_data->xfb_info) {
      *xfb_info = *shader_data->xfb_info;
      typed_memcpy(xfb_info->outputs, shader_data->xfb_info->outputs,
                   shader_data->xfb_info->output_count);
      shader->xfb_info = xfb_info;
   }

   typed_memcpy(&shader->push_desc_info, &shader_data->push_desc_info, 1);

   typed_memcpy(&shader->bind_map, &shader_data->bind_map, 1);
   typed_memcpy(surface_to_descriptor,
                shader_data->bind_map.surface_to_descriptor,
                shader_data->bind_map.surface_count);
   typed_memcpy(sampler_to_descriptor,
                shader_data->bind_map.sampler_to_descriptor,
                shader_data->bind_map.sampler_count);
   typed_memcpy(embedded_sampler_to_binding,
                shader_data->bind_map.embedded_sampler_to_binding,
                shader_data->bind_map.embedded_sampler_count);
   typed_memcpy(shader->bind_map.input_attachments,
                shader_data->bind_map.input_attachments,
                ARRAY_SIZE(shader_data->bind_map.input_attachments));
   shader->bind_map.surface_to_descriptor = surface_to_descriptor;
   shader->bind_map.sampler_to_descriptor = sampler_to_descriptor;
   shader->bind_map.embedded_sampler_to_binding = embedded_sampler_to_binding;

   shader->instance_multiplier = shader_data->instance_multiplier;

   result = anv_shader_reloc(device, shader_data->code, shader, pAllocator);
   if (result != VK_SUCCESS)
      goto error_embedded_samplers;

   memcpy(shader->kernel.map, shader_data->code,
          shader_data->prog_data.base.program_size);

   if (mesa_shader_stage_is_rt(shader->vk.stage)) {
      const struct brw_bs_prog_data *bs_prog_data =
         (const struct brw_bs_prog_data *)shader->prog_data;
      shader->vk.stack_size = bs_prog_data->max_stack_size;
   }
   shader->vk.scratch_size = shader->prog_data->total_scratch;
   shader->vk.ray_queries = shader->prog_data->ray_queries;

   *shader_out = &shader->vk;

   return VK_SUCCESS;

 error_embedded_samplers:
   for (uint32_t s = 0; s < shader->bind_map.embedded_sampler_count; s++)
      anv_embedded_sampler_unref(device, shader->embedded_samplers[s]);
   anv_state_pool_free(&device->instruction_state_pool, shader->kernel);
 error_shader:
   vk_shader_free(&device->vk, pAllocator, &shader->vk);
   return result;
}
