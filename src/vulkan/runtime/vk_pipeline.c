/*
 * Copyright © 2022 Collabora, LTD
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
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
 * IN THE SOFTWARE.
 */

#include "vk_pipeline.h"

#include "vk_alloc.h"
#include "vk_common_entrypoints.h"
#include "vk_command_buffer.h"
#include "vk_descriptor_set_layout.h"
#include "vk_device.h"
#include "vk_graphics_state.h"
#include "vk_log.h"
#include "vk_nir.h"
#include "vk_physical_device.h"
#include "vk_physical_device_features.h"
#include "vk_pipeline_layout.h"
#include "vk_shader.h"
#include "vk_shader_module.h"
#include "vk_util.h"

#include "nir_serialize.h"
#include "nir.h"

#include "util/mesa-sha1.h"

bool
vk_pipeline_shader_stage_is_null(const VkPipelineShaderStageCreateInfo *info)
{
   if (info->module != VK_NULL_HANDLE)
      return false;

   vk_foreach_struct_const(ext, info->pNext) {
      if (ext->sType == VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO ||
          ext->sType == VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_MODULE_IDENTIFIER_CREATE_INFO_EXT)
         return false;
   }

   return true;
}

bool
vk_pipeline_shader_stage_has_identifier(const VkPipelineShaderStageCreateInfo *info)
{
   const VkPipelineShaderStageModuleIdentifierCreateInfoEXT *id_info =
      vk_find_struct_const(info->pNext, PIPELINE_SHADER_STAGE_MODULE_IDENTIFIER_CREATE_INFO_EXT);

   return id_info && id_info->identifierSize != 0;
}

static nir_shader *
get_builtin_nir(const VkPipelineShaderStageCreateInfo *info)
{
   VK_FROM_HANDLE(vk_shader_module, module, info->module);

   nir_shader *nir = NULL;
   if (module != NULL) {
      nir = module->nir;
   } else {
      const VkPipelineShaderStageNirCreateInfoMESA *nir_info =
         vk_find_struct_const(info->pNext, PIPELINE_SHADER_STAGE_NIR_CREATE_INFO_MESA);
      if (nir_info != NULL)
         nir = nir_info->nir;
   }

   if (nir == NULL)
      return NULL;

   assert(nir->info.stage == vk_to_mesa_shader_stage(info->stage));
   ASSERTED nir_function_impl *entrypoint = nir_shader_get_entrypoint(nir);
   assert(strcmp(entrypoint->function->name, info->pName) == 0);
   assert(info->pSpecializationInfo == NULL);

   return nir;
}

static uint32_t
get_required_subgroup_size(const void *info_pNext)
{
   const VkPipelineShaderStageRequiredSubgroupSizeCreateInfo *rss_info =
      vk_find_struct_const(info_pNext,
                           PIPELINE_SHADER_STAGE_REQUIRED_SUBGROUP_SIZE_CREATE_INFO);
   return rss_info != NULL ? rss_info->requiredSubgroupSize : 0;
}

void
vk_set_subgroup_size(struct vk_device *device,
                     nir_shader *shader,
                     uint32_t spirv_version,
                     const void *info_pNext,
                     bool allow_varying,
                     bool require_full)
{
   struct vk_properties *properties = &device->physical->properties;
   uint32_t req_subgroup_size = get_required_subgroup_size(info_pNext);
   if (req_subgroup_size) {
      assert(util_is_power_of_two_nonzero(req_subgroup_size));
      assert(req_subgroup_size >= 1 && req_subgroup_size <= 128);
      shader->info.api_subgroup_size = req_subgroup_size;
      shader->info.max_subgroup_size = req_subgroup_size;
      shader->info.min_subgroup_size = req_subgroup_size;
   } else if (allow_varying || spirv_version >= 0x10600) {
      /* Starting with SPIR-V 1.6, varying subgroup size is the default */
   } else if (properties->subgroupSize) {
      shader->info.api_subgroup_size = properties->subgroupSize;
      shader->info.max_subgroup_size = properties->subgroupSize;
      if (require_full) {
         assert(shader->info.stage == MESA_SHADER_COMPUTE ||
                shader->info.stage == MESA_SHADER_MESH ||
                shader->info.stage == MESA_SHADER_TASK);
         shader->info.min_subgroup_size = properties->subgroupSize;
      }
   }

   if (properties->maxSubgroupSize) {
      assert(properties->minSubgroupSize);
      shader->info.max_subgroup_size =
         MIN2(shader->info.max_subgroup_size, properties->maxSubgroupSize);
      shader->info.min_subgroup_size =
         MAX2(shader->info.min_subgroup_size, properties->minSubgroupSize);
   }

   assert(shader->info.max_subgroup_size >= shader->info.min_subgroup_size);
}

VkResult
vk_pipeline_shader_stage_to_nir(struct vk_device *device,
                                VkPipelineCreateFlags2KHR pipeline_flags,
                                const VkPipelineShaderStageCreateInfo *info,
                                const struct spirv_to_nir_options *spirv_options,
                                const struct nir_shader_compiler_options *nir_options,
                                void *mem_ctx, nir_shader **nir_out)
{
   VK_FROM_HANDLE(vk_shader_module, module, info->module);
   const mesa_shader_stage stage = vk_to_mesa_shader_stage(info->stage);

   assert(info->sType == VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO);

   nir_shader *builtin_nir = get_builtin_nir(info);
   if (builtin_nir != NULL) {
      nir_validate_shader(builtin_nir, "internal shader");

      nir_shader *clone = nir_shader_clone(mem_ctx, builtin_nir);
      if (clone == NULL)
         return vk_error(device, VK_ERROR_OUT_OF_HOST_MEMORY);

      assert(clone->options == NULL || clone->options == nir_options);
      clone->options = nir_options;

      *nir_out = clone;
      return VK_SUCCESS;
   }

   const uint32_t *spirv_data;
   uint32_t spirv_size;
   if (module != NULL) {
      spirv_data = (uint32_t *)module->data;
      spirv_size = module->size;
   } else {
      const VkShaderModuleCreateInfo *minfo =
         vk_find_struct_const(info->pNext, SHADER_MODULE_CREATE_INFO);
      if (unlikely(minfo == NULL)) {
         return vk_errorf(device, VK_ERROR_UNKNOWN,
                          "No shader module provided");
      }
      spirv_data = minfo->pCode;
      spirv_size = minfo->codeSize;
   }

   nir_shader *nir = vk_spirv_to_nir(device, spirv_data, spirv_size, stage,
                                     info->pName,
                                     info->pSpecializationInfo,
                                     spirv_options, nir_options,
                                     false /* internal */,
                                     mem_ctx);
   if (nir == NULL)
      return vk_errorf(device, VK_ERROR_UNKNOWN, "spirv_to_nir failed");

   vk_set_subgroup_size(
      device, nir,
      vk_spirv_version(spirv_data, spirv_size),
      info->pNext,
      info->flags & VK_PIPELINE_SHADER_STAGE_CREATE_ALLOW_VARYING_SUBGROUP_SIZE_BIT,
      info->flags & VK_PIPELINE_SHADER_STAGE_CREATE_REQUIRE_FULL_SUBGROUPS_BIT);

   if (pipeline_flags & VK_PIPELINE_CREATE_2_VIEW_INDEX_FROM_DEVICE_INDEX_BIT_KHR)
      NIR_PASS(_, nir, nir_lower_view_index_to_device_index);

   *nir_out = nir;

   return VK_SUCCESS;
}

void
vk_pipeline_hash_shader_stage(VkPipelineCreateFlags2KHR pipeline_flags,
                              const VkPipelineShaderStageCreateInfo *info,
                              const struct vk_pipeline_robustness_state *rstate,
                              unsigned char *stage_sha1)
{
   VK_FROM_HANDLE(vk_shader_module, module, info->module);

   const nir_shader *builtin_nir = get_builtin_nir(info);
   if (builtin_nir != NULL) {
      /* Internal NIR module: serialize and hash the NIR shader.
       * We don't need to hash other info fields since they should match the
       * NIR data.
       */
      struct blob blob;

      blob_init(&blob);
      nir_serialize(&blob, builtin_nir, false);
      assert(!blob.out_of_memory);
      _mesa_sha1_compute(blob.data, blob.size, stage_sha1);
      blob_finish(&blob);
      return;
   }

   const VkShaderModuleCreateInfo *minfo =
      vk_find_struct_const(info->pNext, SHADER_MODULE_CREATE_INFO);
   const VkPipelineShaderStageModuleIdentifierCreateInfoEXT *iinfo =
      vk_find_struct_const(info->pNext, PIPELINE_SHADER_STAGE_MODULE_IDENTIFIER_CREATE_INFO_EXT);

   struct mesa_sha1 ctx;

   _mesa_sha1_init(&ctx);

   /* We only care about one of the pipeline flags */
   pipeline_flags &= VK_PIPELINE_CREATE_2_VIEW_INDEX_FROM_DEVICE_INDEX_BIT_KHR;
   _mesa_sha1_update(&ctx, &pipeline_flags, sizeof(pipeline_flags));

   _mesa_sha1_update(&ctx, &info->flags, sizeof(info->flags));

   assert(util_bitcount(info->stage) == 1);
   _mesa_sha1_update(&ctx, &info->stage, sizeof(info->stage));

   if (module) {
      _mesa_sha1_update(&ctx, module->hash, sizeof(module->hash));
   } else if (minfo) {
      blake3_hash spirv_hash;

      _mesa_blake3_compute(minfo->pCode, minfo->codeSize, spirv_hash);
      _mesa_sha1_update(&ctx, spirv_hash, sizeof(spirv_hash));
   } else {
      /* It is legal to pass in arbitrary identifiers as long as they don't exceed
       * the limit. Shaders with bogus identifiers are more or less guaranteed to fail. */
      assert(iinfo);
      assert(iinfo->identifierSize <= VK_MAX_SHADER_MODULE_IDENTIFIER_SIZE_EXT);
      _mesa_sha1_update(&ctx, iinfo->pIdentifier, iinfo->identifierSize);
   }

   if (rstate) {
      _mesa_sha1_update(&ctx, &rstate->storage_buffers, sizeof(rstate->storage_buffers));
      _mesa_sha1_update(&ctx, &rstate->uniform_buffers, sizeof(rstate->uniform_buffers));
      _mesa_sha1_update(&ctx, &rstate->vertex_inputs, sizeof(rstate->vertex_inputs));
      _mesa_sha1_update(&ctx, &rstate->images, sizeof(rstate->images));
      _mesa_sha1_update(&ctx, &rstate->null_uniform_buffer_descriptor,
                        sizeof(rstate->null_uniform_buffer_descriptor));
      _mesa_sha1_update(&ctx, &rstate->null_storage_buffer_descriptor,
                        sizeof(rstate->null_storage_buffer_descriptor));
   }

   _mesa_sha1_update(&ctx, info->pName, strlen(info->pName));

   if (info->pSpecializationInfo) {
      _mesa_sha1_update(&ctx, info->pSpecializationInfo->pMapEntries,
                        info->pSpecializationInfo->mapEntryCount *
                        sizeof(*info->pSpecializationInfo->pMapEntries));
      _mesa_sha1_update(&ctx, info->pSpecializationInfo->pData,
                        info->pSpecializationInfo->dataSize);
   }

   uint32_t req_subgroup_size = get_required_subgroup_size(info);
   _mesa_sha1_update(&ctx, &req_subgroup_size, sizeof(req_subgroup_size));

   _mesa_sha1_final(&ctx, stage_sha1);
}

static VkPipelineRobustnessBufferBehaviorEXT
vk_device_default_robust_buffer_behavior(const struct vk_device *device)
{
   if (device->enabled_features.robustBufferAccess2) {
      return VK_PIPELINE_ROBUSTNESS_BUFFER_BEHAVIOR_ROBUST_BUFFER_ACCESS_2_EXT;
   } else if (device->enabled_features.robustBufferAccess) {
      return VK_PIPELINE_ROBUSTNESS_BUFFER_BEHAVIOR_ROBUST_BUFFER_ACCESS_EXT;
   } else {
      return VK_PIPELINE_ROBUSTNESS_BUFFER_BEHAVIOR_DISABLED_EXT;
   }
}

static VkPipelineRobustnessImageBehaviorEXT
vk_device_default_robust_image_behavior(const struct vk_device *device)
{
   if (device->enabled_features.robustImageAccess2) {
      return VK_PIPELINE_ROBUSTNESS_IMAGE_BEHAVIOR_ROBUST_IMAGE_ACCESS_2_EXT;
   } else if (device->enabled_features.robustImageAccess) {
      return VK_PIPELINE_ROBUSTNESS_IMAGE_BEHAVIOR_ROBUST_IMAGE_ACCESS_EXT;
   } else {
      return VK_PIPELINE_ROBUSTNESS_IMAGE_BEHAVIOR_DISABLED_EXT;
   }
}

void
vk_pipeline_robustness_state_fill(const struct vk_device *device,
                                  struct vk_pipeline_robustness_state *rs,
                                  const void *pipeline_pNext,
                                  const void *shader_stage_pNext)
{
   rs->uniform_buffers = VK_PIPELINE_ROBUSTNESS_BUFFER_BEHAVIOR_DEVICE_DEFAULT_EXT;
   rs->storage_buffers = VK_PIPELINE_ROBUSTNESS_BUFFER_BEHAVIOR_DEVICE_DEFAULT_EXT;
   rs->vertex_inputs = VK_PIPELINE_ROBUSTNESS_BUFFER_BEHAVIOR_DEVICE_DEFAULT_EXT;
   rs->images = VK_PIPELINE_ROBUSTNESS_IMAGE_BEHAVIOR_DEVICE_DEFAULT_EXT;
   rs->null_uniform_buffer_descriptor = device->enabled_features.nullDescriptor;
   rs->null_storage_buffer_descriptor = device->enabled_features.nullDescriptor;

   const VkPipelineRobustnessCreateInfoEXT *shader_info =
      vk_find_struct_const(shader_stage_pNext,
                           PIPELINE_ROBUSTNESS_CREATE_INFO_EXT);
   if (shader_info) {
      rs->storage_buffers = shader_info->storageBuffers;
      rs->uniform_buffers = shader_info->uniformBuffers;
      rs->vertex_inputs = shader_info->vertexInputs;
      rs->images = shader_info->images;
   } else {
      const VkPipelineRobustnessCreateInfoEXT *pipeline_info =
         vk_find_struct_const(pipeline_pNext,
                              PIPELINE_ROBUSTNESS_CREATE_INFO_EXT);
      if (pipeline_info) {
         rs->storage_buffers = pipeline_info->storageBuffers;
         rs->uniform_buffers = pipeline_info->uniformBuffers;
         rs->vertex_inputs = pipeline_info->vertexInputs;
         rs->images = pipeline_info->images;
      }
   }

   if (rs->storage_buffers ==
       VK_PIPELINE_ROBUSTNESS_BUFFER_BEHAVIOR_DEVICE_DEFAULT_EXT)
      rs->storage_buffers = vk_device_default_robust_buffer_behavior(device);

   if (rs->uniform_buffers ==
       VK_PIPELINE_ROBUSTNESS_BUFFER_BEHAVIOR_DEVICE_DEFAULT_EXT)
      rs->uniform_buffers = vk_device_default_robust_buffer_behavior(device);

   if (rs->vertex_inputs ==
       VK_PIPELINE_ROBUSTNESS_BUFFER_BEHAVIOR_DEVICE_DEFAULT_EXT)
      rs->vertex_inputs = vk_device_default_robust_buffer_behavior(device);

   if (rs->images == VK_PIPELINE_ROBUSTNESS_IMAGE_BEHAVIOR_DEVICE_DEFAULT_EXT)
      rs->images = vk_device_default_robust_image_behavior(device);
}

static void
vk_pipeline_init(struct vk_pipeline *pipeline,
                 const struct vk_pipeline_ops *ops,
                 VkPipelineBindPoint bind_point,
                 VkPipelineCreateFlags2KHR flags)
{
   pipeline->ops = ops;
   pipeline->bind_point = bind_point;
   pipeline->flags = flags;
}

void *
vk_pipeline_zalloc(struct vk_device *device,
                   const struct vk_pipeline_ops *ops,
                   VkPipelineBindPoint bind_point,
                   VkPipelineCreateFlags2KHR flags,
                   const VkAllocationCallbacks *alloc,
                   size_t size)
{
   struct vk_pipeline *pipeline =
      vk_object_zalloc(device, alloc, size, VK_OBJECT_TYPE_PIPELINE);
   if (pipeline == NULL)
      return NULL;

   vk_pipeline_init(pipeline, ops, bind_point, flags);

   return pipeline;
}

void *vk_pipeline_multizalloc(struct vk_device *device,
                              struct vk_multialloc *ma,
                              const struct vk_pipeline_ops *ops,
                              VkPipelineBindPoint bind_point,
                              VkPipelineCreateFlags2KHR flags,
                              const VkAllocationCallbacks *alloc)
{
   struct vk_pipeline *pipeline =
      vk_object_multizalloc(device, ma, alloc, VK_OBJECT_TYPE_PIPELINE);
   if (!pipeline)
      return NULL;

   vk_pipeline_init(pipeline, ops, bind_point, flags);

   return pipeline;
}

void
vk_pipeline_free(struct vk_device *device,
                 const VkAllocationCallbacks *alloc,
                 struct vk_pipeline *pipeline)
{
   vk_object_free(device, alloc, &pipeline->base);
}

VKAPI_ATTR void VKAPI_CALL
vk_common_DestroyPipeline(VkDevice _device,
                          VkPipeline _pipeline,
                          const VkAllocationCallbacks *pAllocator)
{
   VK_FROM_HANDLE(vk_device, device, _device);
   VK_FROM_HANDLE(vk_pipeline, pipeline, _pipeline);

   if (pipeline == NULL)
      return;

   pipeline->ops->destroy(device, pipeline, pAllocator);
}

VKAPI_ATTR VkResult VKAPI_CALL
vk_common_GetPipelineExecutablePropertiesKHR(
   VkDevice _device,
   const VkPipelineInfoKHR *pPipelineInfo,
   uint32_t *pExecutableCount,
   VkPipelineExecutablePropertiesKHR *pProperties)
{
   VK_FROM_HANDLE(vk_device, device, _device);
   VK_FROM_HANDLE(vk_pipeline, pipeline, pPipelineInfo->pipeline);

   return pipeline->ops->get_executable_properties(device, pipeline,
                                                   pExecutableCount,
                                                   pProperties);
}

VKAPI_ATTR VkResult VKAPI_CALL
vk_common_GetPipelineExecutableStatisticsKHR(
    VkDevice _device,
    const VkPipelineExecutableInfoKHR *pExecutableInfo,
    uint32_t *pStatisticCount,
    VkPipelineExecutableStatisticKHR *pStatistics)
{
   VK_FROM_HANDLE(vk_device, device, _device);
   VK_FROM_HANDLE(vk_pipeline, pipeline, pExecutableInfo->pipeline);

   return pipeline->ops->get_executable_statistics(device, pipeline,
                                                   pExecutableInfo->executableIndex,
                                                   pStatisticCount, pStatistics);
}

VKAPI_ATTR VkResult VKAPI_CALL
vk_common_GetPipelineExecutableInternalRepresentationsKHR(
    VkDevice _device,
    const VkPipelineExecutableInfoKHR *pExecutableInfo,
    uint32_t *pInternalRepresentationCount,
    VkPipelineExecutableInternalRepresentationKHR* pInternalRepresentations)
{
   VK_FROM_HANDLE(vk_device, device, _device);
   VK_FROM_HANDLE(vk_pipeline, pipeline, pExecutableInfo->pipeline);

   return pipeline->ops->get_internal_representations(device, pipeline,
                                                      pExecutableInfo->executableIndex,
                                                      pInternalRepresentationCount,
                                                      pInternalRepresentations);
}

VKAPI_ATTR void VKAPI_CALL
vk_common_CmdBindPipeline(VkCommandBuffer commandBuffer,
                          VkPipelineBindPoint pipelineBindPoint,
                          VkPipeline _pipeline)
{
   VK_FROM_HANDLE(vk_command_buffer, cmd_buffer, commandBuffer);
   VK_FROM_HANDLE(vk_pipeline, pipeline, _pipeline);

   assert(pipeline->bind_point == pipelineBindPoint);

   pipeline->ops->cmd_bind(cmd_buffer, pipeline);
}

static const struct vk_pipeline_cache_object_ops pipeline_shader_cache_ops;

static struct vk_shader *
vk_shader_from_cache_obj(struct vk_pipeline_cache_object *object)
{
   assert(object->ops == &pipeline_shader_cache_ops);
   return container_of(object, struct vk_shader, pipeline.cache_obj);
}

static bool
vk_pipeline_shader_serialize(struct vk_pipeline_cache_object *object,
                             struct blob *blob)
{
   struct vk_shader *shader = vk_shader_from_cache_obj(object);
   struct vk_device *device = shader->base.device;

   return shader->ops->serialize(device, shader, blob);
}

static void
vk_shader_init_cache_obj(struct vk_device *device, struct vk_shader *shader,
                         const void *key_data, size_t key_size)
{
   assert(key_size == sizeof(shader->pipeline.cache_key));
   memcpy(&shader->pipeline.cache_key, key_data,
          sizeof(shader->pipeline.cache_key));

   vk_pipeline_cache_object_init(device, &shader->pipeline.cache_obj,
                                 &pipeline_shader_cache_ops,
                                 &shader->pipeline.cache_key,
                                 sizeof(shader->pipeline.cache_key));
}

static struct vk_pipeline_cache_object *
vk_pipeline_shader_deserialize(struct vk_pipeline_cache *cache,
                               const void *key_data, size_t key_size,
                               struct blob_reader *blob)
{
   struct vk_device *device = cache->base.device;
   const struct vk_device_shader_ops *ops = device->shader_ops;

   /* TODO: Do we really want to always use the latest version? */
   const uint32_t version = device->physical->properties.shaderBinaryVersion;

   struct vk_shader *shader;
   VkResult result = ops->deserialize(device, blob, version,
                                      &device->alloc, &shader);
   if (result != VK_SUCCESS) {
      assert(result == VK_ERROR_OUT_OF_HOST_MEMORY);
      return NULL;
   }

   vk_shader_init_cache_obj(device, shader, key_data, key_size);

   return &shader->pipeline.cache_obj;
}

static void
vk_pipeline_shader_destroy(struct vk_device *device,
                           struct vk_pipeline_cache_object *object)
{
   struct vk_shader *shader = vk_shader_from_cache_obj(object);
   assert(shader->base.device == device);

   vk_shader_destroy(device, shader, &device->alloc);
}

static const struct vk_pipeline_cache_object_ops pipeline_shader_cache_ops = {
   .serialize = vk_pipeline_shader_serialize,
   .deserialize = vk_pipeline_shader_deserialize,
   .destroy = vk_pipeline_shader_destroy,
};

static struct vk_shader *
vk_shader_ref(struct vk_shader *shader)
{
   vk_pipeline_cache_object_ref(&shader->pipeline.cache_obj);
   return shader;
}

static void
vk_shader_unref(struct vk_device *device, struct vk_shader *shader)
{
   vk_pipeline_cache_object_unref(device, &shader->pipeline.cache_obj);
}

PRAGMA_DIAGNOSTIC_PUSH
PRAGMA_DIAGNOSTIC_ERROR(-Wpadded)
struct vk_pipeline_tess_info {
   unsigned tcs_vertices_out : 8;
   unsigned primitive_mode : 2; /* tess_primitive_mode */
   unsigned spacing : 2; /* gl_tess_spacing */
   unsigned ccw : 1;
   unsigned point_mode : 1;
   unsigned _pad : 18;
};
PRAGMA_DIAGNOSTIC_POP
static_assert(sizeof(struct vk_pipeline_tess_info) == 4,
              "This struct has no holes");

static void
vk_pipeline_gather_nir_tess_info(const nir_shader *nir,
                                 struct vk_pipeline_tess_info *info)
{
   info->tcs_vertices_out  = nir->info.tess.tcs_vertices_out;
   info->primitive_mode    = nir->info.tess._primitive_mode;
   info->spacing           = nir->info.tess.spacing;
   info->ccw               = nir->info.tess.ccw;
   info->point_mode        = nir->info.tess.point_mode;
}

static void
vk_pipeline_replace_nir_tess_info(nir_shader *nir,
                                  const struct vk_pipeline_tess_info *info)
{
   nir->info.tess.tcs_vertices_out  = info->tcs_vertices_out;
   nir->info.tess._primitive_mode   = info->primitive_mode;
   nir->info.tess.spacing           = info->spacing;
   nir->info.tess.ccw               = info->ccw;
   nir->info.tess.point_mode        = info->point_mode;
}

static void
vk_pipeline_tess_info_merge(struct vk_pipeline_tess_info *dst,
                            const struct vk_pipeline_tess_info *src)
{
   /* The Vulkan 1.0.38 spec, section 21.1 Tessellator says:
    *
    *    "PointMode. Controls generation of points rather than triangles
    *     or lines. This functionality defaults to disabled, and is
    *     enabled if either shader stage includes the execution mode.
    *
    * and about Triangles, Quads, IsoLines, VertexOrderCw, VertexOrderCcw,
    * PointMode, SpacingEqual, SpacingFractionalEven, SpacingFractionalOdd,
    * and OutputVertices, it says:
    *
    *    "One mode must be set in at least one of the tessellation
    *     shader stages."
    *
    * So, the fields can be set in either the TCS or TES, but they must
    * agree if set in both.
    */
   assert(dst->tcs_vertices_out == 0 ||
          src->tcs_vertices_out == 0 ||
          dst->tcs_vertices_out == src->tcs_vertices_out);
   dst->tcs_vertices_out |= src->tcs_vertices_out;

   static_assert(TESS_SPACING_UNSPECIFIED == 0, "");
   assert(dst->spacing == TESS_SPACING_UNSPECIFIED ||
          src->spacing == TESS_SPACING_UNSPECIFIED ||
          dst->spacing == src->spacing);
   dst->spacing |= src->spacing;

   static_assert(TESS_PRIMITIVE_UNSPECIFIED == 0, "");
   assert(dst->primitive_mode == TESS_PRIMITIVE_UNSPECIFIED ||
          src->primitive_mode == TESS_PRIMITIVE_UNSPECIFIED ||
          dst->primitive_mode == src->primitive_mode);
   dst->primitive_mode |= src->primitive_mode;
   dst->ccw |= src->ccw;
   dst->point_mode |= src->point_mode;
}

struct vk_pipeline_precomp_shader {
   struct vk_pipeline_cache_object cache_obj;

   /* Key for this cache_obj in the pipeline cache.
    *
    * This is always the output of vk_pipeline_hash_shader_stage() so it must
    * be a SHA1 hash.
    */
   uint8_t cache_key[SHA1_DIGEST_LENGTH];

   mesa_shader_stage stage;

   struct vk_pipeline_robustness_state rs;

   /* Tessellation info if the shader is a tessellation shader */
   struct vk_pipeline_tess_info tess;

   /* Hash of the vk_pipeline_precomp_shader
    *
    * This is the hash of the final compiled NIR together with tess info and
    * robustness state.  It's used as a key for final binary lookups.  By
    * having this as a separate key, we can de-duplicate cases where you have
    * different SPIR-V or specialization constants but end up compiling the
    * same NIR shader in the end anyway.
    */
   blake3_hash blake3;

   struct blob nir_blob;
};

static struct vk_pipeline_precomp_shader *
vk_pipeline_precomp_shader_ref(struct vk_pipeline_precomp_shader *shader)
{
   vk_pipeline_cache_object_ref(&shader->cache_obj);
   return shader;
}

static void
vk_pipeline_precomp_shader_unref(struct vk_device *device,
                                 struct vk_pipeline_precomp_shader *shader)
{
   vk_pipeline_cache_object_unref(device, &shader->cache_obj);
}

static const struct vk_pipeline_cache_object_ops pipeline_precomp_shader_cache_ops;

static struct vk_pipeline_precomp_shader *
vk_pipeline_precomp_shader_from_cache_obj(struct vk_pipeline_cache_object *obj)
{
   assert(obj->ops == & pipeline_precomp_shader_cache_ops);
   return container_of(obj, struct vk_pipeline_precomp_shader, cache_obj);
}

static struct vk_pipeline_precomp_shader *
vk_pipeline_precomp_shader_create(struct vk_device *device,
                                  const void *key_data, size_t key_size,
                                  const struct vk_pipeline_robustness_state *rs,
                                  nir_shader *nir)
{
   struct blob blob;
   blob_init(&blob);

   nir_serialize(&blob, nir, false);

   if (blob.out_of_memory)
      goto fail_blob;

   struct vk_pipeline_precomp_shader *shader =
      vk_zalloc(&device->alloc, sizeof(*shader), 8,
                VK_SYSTEM_ALLOCATION_SCOPE_DEVICE);
   if (shader == NULL)
      goto fail_blob;

   assert(sizeof(shader->cache_key) == key_size);
   memcpy(shader->cache_key, key_data, sizeof(shader->cache_key));

   vk_pipeline_cache_object_init(device, &shader->cache_obj,
                                 &pipeline_precomp_shader_cache_ops,
                                 shader->cache_key,
                                 sizeof(shader->cache_key));

   shader->stage = nir->info.stage;
   shader->rs = *rs;

   vk_pipeline_gather_nir_tess_info(nir, &shader->tess);

   struct mesa_blake3 blake3_ctx;
   _mesa_blake3_init(&blake3_ctx);
   _mesa_blake3_update(&blake3_ctx, rs, sizeof(*rs));
   _mesa_blake3_update(&blake3_ctx, blob.data, blob.size);
   _mesa_blake3_final(&blake3_ctx, shader->blake3);

   shader->nir_blob = blob;

   return shader;

fail_blob:
   blob_finish(&blob);

   return NULL;
}

static bool
vk_pipeline_precomp_shader_serialize(struct vk_pipeline_cache_object *obj,
                                     struct blob *blob)
{
   struct vk_pipeline_precomp_shader *shader =
      vk_pipeline_precomp_shader_from_cache_obj(obj);

   blob_write_uint32(blob, shader->stage);
   blob_write_bytes(blob, &shader->rs, sizeof(shader->rs));
   blob_write_bytes(blob, &shader->tess, sizeof(shader->tess));
   blob_write_bytes(blob, shader->blake3, sizeof(shader->blake3));
   blob_write_uint64(blob, shader->nir_blob.size);
   blob_write_bytes(blob, shader->nir_blob.data, shader->nir_blob.size);

   return !blob->out_of_memory;
}

static struct vk_pipeline_cache_object *
vk_pipeline_precomp_shader_deserialize(struct vk_pipeline_cache *cache,
                                       const void *key_data, size_t key_size,
                                       struct blob_reader *blob)
{
   struct vk_device *device = cache->base.device;

   struct vk_pipeline_precomp_shader *shader =
      vk_zalloc(&device->alloc, sizeof(*shader), 8,
                VK_SYSTEM_ALLOCATION_SCOPE_DEVICE);
   if (shader == NULL)
      return NULL;

   assert(sizeof(shader->cache_key) == key_size);
   memcpy(shader->cache_key, key_data, sizeof(shader->cache_key));

   vk_pipeline_cache_object_init(device, &shader->cache_obj,
                                 &pipeline_precomp_shader_cache_ops,
                                 shader->cache_key,
                                 sizeof(shader->cache_key));

   shader->stage = blob_read_uint32(blob);
   blob_copy_bytes(blob, &shader->rs, sizeof(shader->rs));
   blob_copy_bytes(blob, &shader->tess, sizeof(shader->tess));
   blob_copy_bytes(blob, shader->blake3, sizeof(shader->blake3));

   uint64_t nir_size = blob_read_uint64(blob);
   if (blob->overrun || nir_size > SIZE_MAX)
      goto fail_shader;

   const void *nir_data = blob_read_bytes(blob, nir_size);
   if (blob->overrun)
      goto fail_shader;

   blob_init(&shader->nir_blob);
   blob_write_bytes(&shader->nir_blob, nir_data, nir_size);
   if (shader->nir_blob.out_of_memory)
      goto fail_nir_blob;

   return &shader->cache_obj;

fail_nir_blob:
   blob_finish(&shader->nir_blob);
fail_shader:
   vk_pipeline_cache_object_finish(&shader->cache_obj);
   vk_free(&device->alloc, shader);

   return NULL;
}

static void
vk_pipeline_precomp_shader_destroy(struct vk_device *device,
                                   struct vk_pipeline_cache_object *obj)
{
   struct vk_pipeline_precomp_shader *shader =
      vk_pipeline_precomp_shader_from_cache_obj(obj);

   blob_finish(&shader->nir_blob);
   vk_pipeline_cache_object_finish(&shader->cache_obj);
   vk_free(&device->alloc, shader);
}

static nir_shader *
vk_pipeline_precomp_shader_get_nir(const struct vk_pipeline_precomp_shader *shader,
                                   const struct nir_shader_compiler_options *nir_options)
{
   struct blob_reader blob;
   blob_reader_init(&blob, shader->nir_blob.data, shader->nir_blob.size);

   nir_shader *nir = nir_deserialize(NULL, nir_options, &blob);
   if (blob.overrun) {
      ralloc_free(nir);
      return NULL;
   }

   return nir;
}

static const struct vk_pipeline_cache_object_ops pipeline_precomp_shader_cache_ops = {
   .serialize = vk_pipeline_precomp_shader_serialize,
   .deserialize = vk_pipeline_precomp_shader_deserialize,
   .destroy = vk_pipeline_precomp_shader_destroy,
};

static VkResult
vk_pipeline_precompile_shader(struct vk_device *device,
                              struct vk_pipeline_cache *cache,
                              VkPipelineCreateFlags2KHR pipeline_flags,
                              const void *pipeline_info_pNext,
                              const VkPipelineShaderStageCreateInfo *info,
                              struct vk_pipeline_precomp_shader **ps_out)
{
   const struct vk_device_shader_ops *ops = device->shader_ops;
   VkResult result;

   struct vk_pipeline_robustness_state rs = { 0 };
   vk_pipeline_robustness_state_fill(device, &rs,
                                     pipeline_info_pNext,
                                     info->pNext);

   uint8_t stage_sha1[SHA1_DIGEST_LENGTH];
   vk_pipeline_hash_shader_stage(pipeline_flags, info, &rs, stage_sha1);

   if (cache != NULL) {
      struct vk_pipeline_cache_object *cache_obj =
         vk_pipeline_cache_lookup_object(cache, stage_sha1, sizeof(stage_sha1),
                                         &pipeline_precomp_shader_cache_ops,
                                         NULL /* cache_hit */);
      if (cache_obj != NULL) {
         *ps_out = vk_pipeline_precomp_shader_from_cache_obj(cache_obj);
         return VK_SUCCESS;
      }
   }

   if (pipeline_flags &
       VK_PIPELINE_CREATE_2_FAIL_ON_PIPELINE_COMPILE_REQUIRED_BIT_KHR)
      return VK_PIPELINE_COMPILE_REQUIRED;

   const mesa_shader_stage stage = vk_to_mesa_shader_stage(info->stage);
   const struct nir_shader_compiler_options *nir_options =
      ops->get_nir_options(device->physical, stage, &rs);
   const struct spirv_to_nir_options spirv_options =
      ops->get_spirv_options(device->physical, stage, &rs);

   nir_shader *nir;
   result = vk_pipeline_shader_stage_to_nir(device, pipeline_flags, info,
                                            &spirv_options, nir_options,
                                            NULL, &nir);
   if (result != VK_SUCCESS)
      return result;

   if (ops->preprocess_nir != NULL)
      ops->preprocess_nir(device->physical, nir, &rs);

   struct vk_pipeline_precomp_shader *shader =
      vk_pipeline_precomp_shader_create(device, stage_sha1,
                                        sizeof(stage_sha1),
                                        &rs, nir);
   ralloc_free(nir);
   if (shader == NULL)
      return vk_error(device, VK_ERROR_OUT_OF_HOST_MEMORY);

   if (cache != NULL) {
      struct vk_pipeline_cache_object *cache_obj = &shader->cache_obj;
      cache_obj = vk_pipeline_cache_add_object(cache, cache_obj);
      shader = vk_pipeline_precomp_shader_from_cache_obj(cache_obj);
   }

   *ps_out = shader;

   return VK_SUCCESS;
}

struct vk_pipeline_stage {
   mesa_shader_stage stage;

   struct vk_pipeline_precomp_shader *precomp;
   struct vk_shader *shader;
};

static int
cmp_vk_pipeline_stages(const void *_a, const void *_b)
{
   const struct vk_pipeline_stage *a = _a, *b = _b;
   return vk_shader_cmp_graphics_stages(a->stage, b->stage);
}

static bool
vk_pipeline_stage_is_null(const struct vk_pipeline_stage *stage)
{
   return stage->precomp == NULL && stage->shader == NULL;
}

static void
vk_pipeline_stage_finish(struct vk_device *device,
                         struct vk_pipeline_stage *stage)
{
   if (stage->precomp != NULL)
      vk_pipeline_precomp_shader_unref(device, stage->precomp);

   if (stage->shader)
      vk_shader_unref(device, stage->shader);
}

static struct vk_pipeline_stage
vk_pipeline_stage_clone(const struct vk_pipeline_stage *in)
{
   struct vk_pipeline_stage out = {
      .stage = in->stage,
   };

   if (in->precomp)
      out.precomp = vk_pipeline_precomp_shader_ref(in->precomp);

   if (in->shader)
      out.shader = vk_shader_ref(in->shader);

   return out;
}

struct vk_graphics_pipeline {
   struct vk_pipeline base;

   union {
      struct {
         struct vk_graphics_pipeline_all_state all_state;
         struct vk_graphics_pipeline_state state;
      } lib;

      struct {
         struct vk_vertex_input_state _dynamic_vi;
         struct vk_sample_locations_state _dynamic_sl;
         struct vk_dynamic_graphics_state dynamic;
      } linked;
   };

   uint32_t set_layout_count;
   struct vk_descriptor_set_layout *set_layouts[MESA_VK_MAX_DESCRIPTOR_SETS];

   uint32_t stage_count;
   struct vk_pipeline_stage stages[MESA_VK_MAX_GRAPHICS_PIPELINE_STAGES];
};

static void
vk_graphics_pipeline_destroy(struct vk_device *device,
                             struct vk_pipeline *pipeline,
                             const VkAllocationCallbacks *pAllocator)
{
   struct vk_graphics_pipeline *gfx_pipeline =
      container_of(pipeline, struct vk_graphics_pipeline, base);

   for (uint32_t i = 0; i < gfx_pipeline->stage_count; i++)
      vk_pipeline_stage_finish(device, &gfx_pipeline->stages[i]);

   for (uint32_t i = 0; i < gfx_pipeline->set_layout_count; i++) {
      if (gfx_pipeline->set_layouts[i] != NULL)
         vk_descriptor_set_layout_unref(device, gfx_pipeline->set_layouts[i]);
   }

   vk_pipeline_free(device, pAllocator, pipeline);
}

static bool
vk_device_supports_stage(struct vk_device *device,
                         mesa_shader_stage stage)
{
   const struct vk_features *features = &device->physical->supported_features;

   switch (stage) {
   case MESA_SHADER_VERTEX:
   case MESA_SHADER_FRAGMENT:
   case MESA_SHADER_COMPUTE:
      return true;
   case MESA_SHADER_TESS_CTRL:
   case MESA_SHADER_TESS_EVAL:
      return features->tessellationShader;
   case MESA_SHADER_GEOMETRY:
      return features->geometryShader;
   case MESA_SHADER_TASK:
      return features->taskShader;
   case MESA_SHADER_MESH:
      return features->meshShader;
   default:
      return false;
   }
}

static const mesa_shader_stage all_gfx_stages[] = {
   MESA_SHADER_VERTEX,
   MESA_SHADER_TESS_CTRL,
   MESA_SHADER_TESS_EVAL,
   MESA_SHADER_GEOMETRY,
   MESA_SHADER_TASK,
   MESA_SHADER_MESH,
   MESA_SHADER_FRAGMENT,
};

static void
vk_graphics_pipeline_cmd_bind(struct vk_command_buffer *cmd_buffer,
                              struct vk_pipeline *pipeline)
{
   struct vk_device *device = cmd_buffer->base.device;
   const struct vk_device_shader_ops *ops = device->shader_ops;

   struct vk_graphics_pipeline *gfx_pipeline = NULL;
   struct vk_shader *stage_shader[MESA_SHADER_MESH_STAGES] = { NULL, };
   if (pipeline != NULL) {
      assert(pipeline->bind_point == VK_PIPELINE_BIND_POINT_GRAPHICS);
      assert(!(pipeline->flags & VK_PIPELINE_CREATE_2_LIBRARY_BIT_KHR));
      gfx_pipeline = container_of(pipeline, struct vk_graphics_pipeline, base);

      for (uint32_t i = 0; i < gfx_pipeline->stage_count; i++) {
         struct vk_shader *shader = gfx_pipeline->stages[i].shader;
         stage_shader[shader->stage] = shader;
      }
   }

   uint32_t stage_count = 0;
   mesa_shader_stage stages[ARRAY_SIZE(all_gfx_stages)];
   struct vk_shader *shaders[ARRAY_SIZE(all_gfx_stages)];

   VkShaderStageFlags vk_stages = 0;
   for (uint32_t i = 0; i < ARRAY_SIZE(all_gfx_stages); i++) {
      mesa_shader_stage stage = all_gfx_stages[i];
      if (!vk_device_supports_stage(device, stage)) {
         assert(stage_shader[stage] == NULL);
         continue;
      }

      vk_stages |= mesa_to_vk_shader_stage(stage);

      stages[stage_count] = stage;
      shaders[stage_count] = stage_shader[stage];
      stage_count++;
   }
   ops->cmd_bind_shaders(cmd_buffer, stage_count, stages, shaders);

   if (gfx_pipeline != NULL) {
      cmd_buffer->pipeline_shader_stages |= vk_stages;
      ops->cmd_set_dynamic_graphics_state(cmd_buffer,
                                          &gfx_pipeline->linked.dynamic);
   } else {
      cmd_buffer->pipeline_shader_stages &= ~vk_stages;
   }
}

static VkShaderCreateFlagsEXT
vk_pipeline_to_shader_flags(VkPipelineCreateFlags2KHR pipeline_flags,
                            mesa_shader_stage stage)
{
   VkShaderCreateFlagsEXT shader_flags = 0;

   if (pipeline_flags & VK_PIPELINE_CREATE_2_CAPTURE_INTERNAL_REPRESENTATIONS_BIT_KHR)
      shader_flags |= VK_SHADER_CREATE_CAPTURE_INTERNAL_REPRESENTATIONS_BIT_MESA;

   if (pipeline_flags & VK_PIPELINE_CREATE_2_INDIRECT_BINDABLE_BIT_EXT)
      shader_flags |= VK_SHADER_CREATE_INDIRECT_BINDABLE_BIT_EXT;

   if (stage == MESA_SHADER_FRAGMENT) {
      if (pipeline_flags & VK_PIPELINE_CREATE_2_RENDERING_FRAGMENT_SHADING_RATE_ATTACHMENT_BIT_KHR)
         shader_flags |= VK_SHADER_CREATE_FRAGMENT_SHADING_RATE_ATTACHMENT_BIT_EXT;

      if (pipeline_flags & VK_PIPELINE_CREATE_2_RENDERING_FRAGMENT_DENSITY_MAP_ATTACHMENT_BIT_EXT)
         shader_flags |= VK_SHADER_CREATE_FRAGMENT_DENSITY_MAP_ATTACHMENT_BIT_EXT;
   }

   if (stage == MESA_SHADER_COMPUTE) {
      if (pipeline_flags & VK_PIPELINE_CREATE_2_DISPATCH_BASE_BIT_KHR)
         shader_flags |= VK_SHADER_CREATE_DISPATCH_BASE_BIT_EXT;

      if (pipeline_flags & VK_PIPELINE_CREATE_2_UNALIGNED_DISPATCH_BIT_MESA)
         shader_flags |= VK_SHADER_CREATE_UNALIGNED_DISPATCH_BIT_MESA;
   }

   return shader_flags;
}

static VkResult
vk_graphics_pipeline_compile_shaders(struct vk_device *device,
                                     struct vk_pipeline_cache *cache,
                                     struct vk_graphics_pipeline *pipeline,
                                     struct vk_pipeline_layout *pipeline_layout,
                                     const struct vk_graphics_pipeline_state *state,
                                     bool link_time_optimize,
                                     uint32_t stage_count,
                                     struct vk_pipeline_stage *stages,
                                     VkPipelineCreationFeedback *stage_feedbacks)
{
   const struct vk_device_shader_ops *ops = device->shader_ops;
   VkResult result;

   if (stage_count == 0)
      return VK_SUCCESS;

   /* If we're linking, throw away any previously compiled shaders as they
    * likely haven't been properly linked.  We keep the precompiled shaders
    * and we still look it up in the cache so it may still be fast.
    */
   if (link_time_optimize) {
      for (uint32_t i = 0; i < stage_count; i++) {
         if (stages[i].shader != NULL) {
            vk_shader_unref(device, stages[i].shader);
            stages[i].shader = NULL;
         }
      }
   }

   bool have_all_shaders = true;
   VkShaderStageFlags all_stages = 0;
   struct vk_pipeline_precomp_shader *tcs_precomp = NULL, *tes_precomp = NULL;
   for (uint32_t i = 0; i < stage_count; i++) {
      all_stages |= mesa_to_vk_shader_stage(stages[i].stage);

      if (stages[i].shader == NULL)
         have_all_shaders = false;

      if (stages[i].stage == MESA_SHADER_TESS_CTRL)
         tcs_precomp = stages[i].precomp;

      if (stages[i].stage == MESA_SHADER_TESS_EVAL)
         tes_precomp = stages[i].precomp;
   }

   /* If we already have a shader for each stage, there's nothing to do. */
   if (have_all_shaders)
      return VK_SUCCESS;

   struct vk_pipeline_tess_info tess_info = { ._pad = 0 };
   if (tcs_precomp != NULL && tes_precomp != NULL) {
      tess_info = tcs_precomp->tess;
      vk_pipeline_tess_info_merge(&tess_info, &tes_precomp->tess);
   }

   struct mesa_blake3 blake3_ctx;
   _mesa_blake3_init(&blake3_ctx);
   for (uint32_t i = 0; i < pipeline->set_layout_count; i++) {
      if (pipeline->set_layouts[i] != NULL) {
         _mesa_blake3_update(&blake3_ctx, pipeline->set_layouts[i]->blake3,
                           sizeof(pipeline->set_layouts[i]->blake3));
      }
   }
   if (pipeline_layout != NULL) {
      _mesa_blake3_update(&blake3_ctx, &pipeline_layout->push_ranges,
                        sizeof(pipeline_layout->push_ranges[0]) *
                           pipeline_layout->push_range_count);
   }
   blake3_hash layout_blake3;
   _mesa_blake3_final(&blake3_ctx, layout_blake3);

   /* Partition the shaders. Whenever pipelines are used,
    * vertex/geometry/fragment stages are always specified together, so should
    * always be linked. That doesn't break the fast link since the relevant link
    * happens at pipeline library create time.
    *
    * We don't gate this behind an option since linking shaders is beneficial on
    * all hardware, to clean up the I/O mess that applications regularly leave.
    */
   uint32_t part_count;
   uint32_t partition[MESA_VK_MAX_GRAPHICS_PIPELINE_STAGES + 1] = { 0 };
   if (link_time_optimize) {
      partition[1] = stage_count;
      part_count = 1;
   } else if (stages[0].stage == MESA_SHADER_FRAGMENT) {
      assert(stage_count == 1);
      partition[1] = stage_count;
      part_count = 1;
   } else if (stages[stage_count - 1].stage == MESA_SHADER_FRAGMENT) {
      /* In this case we have both geometry stages and fragment */
      assert(stage_count > 1);
      partition[1] = stage_count - 1;
      partition[2] = stage_count;
      part_count = 2;
   } else {
      /* In this case we only have geometry stages */
      partition[1] = stage_count;
      part_count = 1;
   }

   for (uint32_t p = 0; p < part_count; p++) {
      const int64_t part_start = os_time_get_nano();

      /* Don't try to re-compile any fast-link shaders */
      if (!link_time_optimize && stages[partition[p]].shader != NULL)
         continue;

      struct vk_shader_pipeline_cache_key shader_key = { 0 };

      _mesa_blake3_init(&blake3_ctx);

      VkShaderStageFlags part_stages = 0;
      for (uint32_t i = partition[p]; i < partition[p + 1]; i++) {
         const struct vk_pipeline_stage *stage = &stages[i];

         part_stages |= mesa_to_vk_shader_stage(stage->stage);
         _mesa_blake3_update(&blake3_ctx, stage->precomp->blake3,
                             sizeof(stage->precomp->blake3));

         VkShaderCreateFlagsEXT shader_flags =
            vk_pipeline_to_shader_flags(pipeline->base.flags, stage->stage);
         _mesa_blake3_update(&blake3_ctx, &shader_flags, sizeof(shader_flags));
      }

      blake3_hash state_blake3;
      ops->hash_state(device->physical, state, &device->enabled_features,
                      part_stages, state_blake3);

      _mesa_blake3_update(&blake3_ctx, state_blake3, sizeof(state_blake3));
      _mesa_blake3_update(&blake3_ctx, layout_blake3, sizeof(layout_blake3));

      if (part_stages & (VK_SHADER_STAGE_TESSELLATION_CONTROL_BIT |
                         VK_SHADER_STAGE_TESSELLATION_EVALUATION_BIT))
         _mesa_blake3_update(&blake3_ctx, &tess_info, sizeof(tess_info));

      /* The set of geometry stages used together is used to generate the
       * nextStage mask as well as VK_SHADER_CREATE_NO_TASK_SHADER_BIT_EXT.
       */
      const VkShaderStageFlags geom_stages =
         all_stages & ~VK_SHADER_STAGE_FRAGMENT_BIT;
      _mesa_blake3_update(&blake3_ctx, &geom_stages, sizeof(geom_stages));

      _mesa_blake3_final(&blake3_ctx, shader_key.blake3);

      if (cache != NULL) {
         /* From the Vulkan 1.3.278 spec:
          *
          *    "VK_PIPELINE_CREATION_FEEDBACK_APPLICATION_PIPELINE_CACHE_HIT_BIT
          *    indicates that a readily usable pipeline or pipeline stage was
          *    found in the pipelineCache specified by the application in the
          *    pipeline creation command.
          *
          *    [...]
          *
          *    Note
          *
          *    Implementations are encouraged to provide a meaningful signal
          *    to applications using this bit. The intention is to communicate
          *    to the application that the pipeline or pipeline stage was
          *    created “as fast as it gets” using the pipeline cache provided
          *    by the application. If an implementation uses an internal
          *    cache, it is discouraged from setting this bit as the feedback
          *    would be unactionable."
          *
          * The cache_hit value returned by vk_pipeline_cache_lookup_object()
          * is only set to true when the shader is found in the provided
          * pipeline cache.  It is left false if we fail to find it in the
          * memory cache but find it in the disk cache even though that's
          * still a cache hit from the perspective of the compile pipeline.
          */
         bool all_shaders_found = true;
         bool all_cache_hits = true;
         for (uint32_t i = partition[p]; i < partition[p + 1]; i++) {
            struct vk_pipeline_stage *stage = &stages[i];

            shader_key.stage = stage->stage;

            if (stage->shader) {
               /* If we have a shader from some library pipeline and the key
                * matches, just use that.
                */
               if (memcmp(&stage->shader->pipeline.cache_key,
                          &shader_key, sizeof(shader_key)) == 0)
                  continue;

               /* Otherwise, throw it away */
               vk_shader_unref(device, stage->shader);
               stage->shader = NULL;
            }

            bool cache_hit = false;
            struct vk_pipeline_cache_object *cache_obj =
               vk_pipeline_cache_lookup_object(cache, &shader_key,
                                               sizeof(shader_key),
                                               &pipeline_shader_cache_ops,
                                               &cache_hit);
            if (cache_obj != NULL) {
               assert(stage->shader == NULL);
               stage->shader = vk_shader_from_cache_obj(cache_obj);
            } else {
               all_shaders_found = false;
            }

            if (cache_obj == NULL && !cache_hit)
               all_cache_hits = false;
         }

         if (all_cache_hits && cache != device->mem_cache) {
            /* The pipeline cache only really helps if we hit for everything
             * in the partition.  Otherwise, we have to go re-compile it all
             * anyway.
             */
            for (uint32_t i = partition[p]; i < partition[p + 1]; i++) {
               struct vk_pipeline_stage *stage = &stages[i];

               stage_feedbacks[stage->stage].flags |=
                  VK_PIPELINE_CREATION_FEEDBACK_APPLICATION_PIPELINE_CACHE_HIT_BIT;
            }
         }

         if (all_shaders_found) {
            /* Update duration to take cache lookups into account */
            const int64_t part_end = os_time_get_nano();
            for (uint32_t i = partition[p]; i < partition[p + 1]; i++) {
               struct vk_pipeline_stage *stage = &stages[i];
               stage_feedbacks[stage->stage].duration += part_end - part_start;
            }
            continue;
         }
      }

      if (pipeline->base.flags &
          VK_PIPELINE_CREATE_2_FAIL_ON_PIPELINE_COMPILE_REQUIRED_BIT_KHR)
         return VK_PIPELINE_COMPILE_REQUIRED;

      struct vk_shader_compile_info infos[MESA_VK_MAX_GRAPHICS_PIPELINE_STAGES];
      for (uint32_t i = partition[p]; i < partition[p + 1]; i++) {
         struct vk_pipeline_stage *stage = &stages[i];

         VkShaderCreateFlagsEXT shader_flags =
            vk_pipeline_to_shader_flags(pipeline->base.flags, stage->stage);

         if (partition[p + 1] - partition[p] > 1)
            shader_flags |= VK_SHADER_CREATE_LINK_STAGE_BIT_EXT;

         if ((part_stages & VK_SHADER_STAGE_MESH_BIT_EXT) &&
             !(geom_stages & VK_SHADER_STAGE_TASK_BIT_EXT))
            shader_flags = VK_SHADER_CREATE_NO_TASK_SHADER_BIT_EXT;

         VkShaderStageFlags next_stage;
         if (stage->stage == MESA_SHADER_FRAGMENT) {
            next_stage = 0;
         } else if (i + 1 < stage_count) {
            /* We hash geom_stages above so this is safe */
            next_stage = mesa_to_vk_shader_stage(stages[i + 1].stage);
         } else {
            /* We're the last geometry stage */
            next_stage = VK_SHADER_STAGE_FRAGMENT_BIT;
         }

         const struct nir_shader_compiler_options *nir_options =
            ops->get_nir_options(device->physical, stage->stage,
                                 &stage->precomp->rs);

         nir_shader *nir =
            vk_pipeline_precomp_shader_get_nir(stage->precomp, nir_options);
         if (nir == NULL) {
            for (uint32_t j = partition[p]; j < i; j++)
               ralloc_free(infos[i].nir);

            return vk_error(device, VK_ERROR_OUT_OF_HOST_MEMORY);
         }

         if (stage->stage == MESA_SHADER_TESS_CTRL ||
             stage->stage == MESA_SHADER_TESS_EVAL)
            vk_pipeline_replace_nir_tess_info(nir, &tess_info);

         const VkPushConstantRange *push_range = NULL;
         if (pipeline_layout != NULL) {
            for (uint32_t r = 0; r < pipeline_layout->push_range_count; r++) {
               if (pipeline_layout->push_ranges[r].stageFlags &
                   mesa_to_vk_shader_stage(stage->stage)) {
                  assert(push_range == NULL);
                  push_range = &pipeline_layout->push_ranges[r];
               }
            }
         }

         infos[i] = (struct vk_shader_compile_info) {
            .stage = stage->stage,
            .flags = shader_flags,
            .next_stage_mask = next_stage,
            .nir = nir,
            .robustness = &stage->precomp->rs,
            .set_layout_count = pipeline->set_layout_count,
            .set_layouts = pipeline->set_layouts,
            .push_constant_range_count = push_range != NULL,
            .push_constant_ranges = push_range != NULL ? push_range : NULL,
         };
      }

      /* vk_shader_ops::compile() consumes the NIR regardless of whether or
       * not it succeeds and only generates shaders on success. Once this
       * returns, we own the shaders but not the NIR in infos.
       */
      struct vk_shader *shaders[MESA_VK_MAX_GRAPHICS_PIPELINE_STAGES];
      result = ops->compile(device, partition[p + 1] - partition[p],
                            &infos[partition[p]],
                            state, &device->enabled_features,
                            &device->alloc,
                            &shaders[partition[p]]);
      if (result != VK_SUCCESS)
         return result;

      const int64_t part_end = os_time_get_nano();
      for (uint32_t i = partition[p]; i < partition[p + 1]; i++) {
         struct vk_pipeline_stage *stage = &stages[i];

         shader_key.stage = stage->stage;
         vk_shader_init_cache_obj(device, shaders[i], &shader_key,
                                  sizeof(shader_key));

         if (stage->shader == NULL) {
            struct vk_pipeline_cache_object *cache_obj =
               &shaders[i]->pipeline.cache_obj;
            if (cache != NULL)
               cache_obj = vk_pipeline_cache_add_object(cache, cache_obj);

            stage->shader = vk_shader_from_cache_obj(cache_obj);
         } else {
            /* This can fail to happen if only some of the shaders were found
             * in the pipeline cache.  In this case, we just throw away the
             * shader as vk_pipeline_cache_add_object() would throw it away
             * for us anyway.
             */
            assert(memcmp(&stage->shader->pipeline.cache_key,
                          &shaders[i]->pipeline.cache_key,
                          sizeof(shaders[i]->pipeline.cache_key)) == 0);

            vk_shader_unref(device, shaders[i]);
         }

         stage_feedbacks[stage->stage].duration += part_end - part_start;
      }
   }

   return VK_SUCCESS;
}

static VkResult
vk_graphics_pipeline_get_executable_properties(
   struct vk_device *device,
   struct vk_pipeline *pipeline,
   uint32_t *executable_count,
   VkPipelineExecutablePropertiesKHR *properties)
{
   struct vk_graphics_pipeline *gfx_pipeline =
      container_of(pipeline, struct vk_graphics_pipeline, base);
   VkResult result;

   if (properties == NULL) {
      *executable_count = 0;
      for (uint32_t i = 0; i < gfx_pipeline->stage_count; i++) {
         struct vk_shader *shader = gfx_pipeline->stages[i].shader;

         uint32_t shader_exec_count = 0;
         result = shader->ops->get_executable_properties(device, shader,
                                                         &shader_exec_count,
                                                         NULL);
         assert(result == VK_SUCCESS);
         *executable_count += shader_exec_count;
      }
   } else {
      uint32_t arr_len = *executable_count;
      *executable_count = 0;
      for (uint32_t i = 0; i < gfx_pipeline->stage_count; i++) {
         struct vk_shader *shader = gfx_pipeline->stages[i].shader;

         uint32_t shader_exec_count = arr_len - *executable_count;
         result = shader->ops->get_executable_properties(device, shader,
                                                         &shader_exec_count,
                                                         &properties[*executable_count]);
         if (result != VK_SUCCESS)
            return result;

         *executable_count += shader_exec_count;
      }
   }

   return VK_SUCCESS;
}

static inline struct vk_shader *
vk_graphics_pipeline_executable_shader(struct vk_device *device,
                                       struct vk_graphics_pipeline *gfx_pipeline,
                                       uint32_t *executable_index)
{
   for (uint32_t i = 0; i < gfx_pipeline->stage_count; i++) {
      struct vk_shader *shader = gfx_pipeline->stages[i].shader;

      uint32_t shader_exec_count = 0;
      shader->ops->get_executable_properties(device, shader,
                                             &shader_exec_count, NULL);

      if (*executable_index < shader_exec_count)
         return shader;
      else
         *executable_index -= shader_exec_count;
   }

   return NULL;
}

static VkResult
vk_graphics_pipeline_get_executable_statistics(
   struct vk_device *device,
   struct vk_pipeline *pipeline,
   uint32_t executable_index,
   uint32_t *statistic_count,
   VkPipelineExecutableStatisticKHR *statistics)
{
   struct vk_graphics_pipeline *gfx_pipeline =
      container_of(pipeline, struct vk_graphics_pipeline, base);

   struct vk_shader *shader =
      vk_graphics_pipeline_executable_shader(device, gfx_pipeline,
                                             &executable_index);
   if (shader == NULL) {
      *statistic_count = 0;
      return VK_SUCCESS;
   }

   return shader->ops->get_executable_statistics(device, shader,
                                                 executable_index,
                                                 statistic_count,
                                                 statistics);
}

static VkResult
vk_graphics_pipeline_get_internal_representations(
   struct vk_device *device,
   struct vk_pipeline *pipeline,
   uint32_t executable_index,
   uint32_t *internal_representation_count,
   VkPipelineExecutableInternalRepresentationKHR* internal_representations)
{
   struct vk_graphics_pipeline *gfx_pipeline =
      container_of(pipeline, struct vk_graphics_pipeline, base);

   struct vk_shader *shader =
      vk_graphics_pipeline_executable_shader(device, gfx_pipeline,
                                             &executable_index);
   if (shader == NULL) {
      *internal_representation_count = 0;
      return VK_SUCCESS;
   }

   return shader->ops->get_executable_internal_representations(
      device, shader, executable_index,
      internal_representation_count, internal_representations);
}

static struct vk_shader *
vk_graphics_pipeline_get_shader(struct vk_pipeline *pipeline,
                                mesa_shader_stage stage)
{
   struct vk_graphics_pipeline *gfx_pipeline =
      container_of(pipeline, struct vk_graphics_pipeline, base);

   for (uint32_t i = 0; i < gfx_pipeline->stage_count; i++) {
      if (gfx_pipeline->stages[i].stage == stage)
         return gfx_pipeline->stages[i].shader;
   }

   return NULL;
}

static const struct vk_pipeline_ops vk_graphics_pipeline_ops = {
   .destroy = vk_graphics_pipeline_destroy,
   .get_executable_statistics = vk_graphics_pipeline_get_executable_statistics,
   .get_executable_properties = vk_graphics_pipeline_get_executable_properties,
   .get_internal_representations = vk_graphics_pipeline_get_internal_representations,
   .cmd_bind = vk_graphics_pipeline_cmd_bind,
   .get_shader = vk_graphics_pipeline_get_shader,
};

static VkResult
vk_create_graphics_pipeline(struct vk_device *device,
                            struct vk_pipeline_cache *cache,
                            const VkGraphicsPipelineCreateInfo *pCreateInfo,
                            const VkAllocationCallbacks *pAllocator,
                            VkPipeline *pPipeline)
{
   VK_FROM_HANDLE(vk_pipeline_layout, pipeline_layout, pCreateInfo->layout);
   const int64_t pipeline_start = os_time_get_nano();
   VkResult result;

   const VkPipelineCreateFlags2KHR pipeline_flags =
      vk_graphics_pipeline_create_flags(pCreateInfo);

   const VkPipelineCreationFeedbackCreateInfo *feedback_info =
      vk_find_struct_const(pCreateInfo->pNext,
                           PIPELINE_CREATION_FEEDBACK_CREATE_INFO);

   const VkPipelineLibraryCreateInfoKHR *libs_info =
      vk_find_struct_const(pCreateInfo->pNext,
                           PIPELINE_LIBRARY_CREATE_INFO_KHR);

   struct vk_graphics_pipeline *pipeline =
      vk_pipeline_zalloc(device, &vk_graphics_pipeline_ops,
                         VK_PIPELINE_BIND_POINT_GRAPHICS,
                         pipeline_flags, pAllocator, sizeof(*pipeline));
   if (pipeline == NULL)
      return vk_error(device, VK_ERROR_OUT_OF_HOST_MEMORY);

   struct vk_pipeline_stage stages[MESA_SHADER_MESH_STAGES];
   memset(stages, 0, sizeof(stages));

   VkPipelineCreationFeedback stage_feedbacks[MESA_SHADER_MESH_STAGES];
   memset(stage_feedbacks, 0, sizeof(stage_feedbacks));

   struct vk_graphics_pipeline_state state_tmp, *state;
   struct vk_graphics_pipeline_all_state all_state_tmp, *all_state;
   if (pipeline->base.flags & VK_PIPELINE_CREATE_2_LIBRARY_BIT_KHR) {
      /* For pipeline libraries, the state is stored in the pipeline */
      state = &pipeline->lib.state;
      all_state = &pipeline->lib.all_state;
   } else {
      /* For linked pipelines, we throw the state away at the end of pipeline
       * creation and only keep the dynamic state.
       */
      memset(&state_tmp, 0, sizeof(state_tmp));
      state = &state_tmp;
      all_state = &all_state_tmp;
   }

   /* If we have libraries, import them first. */
   if (libs_info) {
      for (uint32_t i = 0; i < libs_info->libraryCount; i++) {
         VK_FROM_HANDLE(vk_pipeline, lib_pipeline, libs_info->pLibraries[i]);
         assert(lib_pipeline->bind_point == VK_PIPELINE_BIND_POINT_GRAPHICS);
         assert(lib_pipeline->flags & VK_PIPELINE_CREATE_2_LIBRARY_BIT_KHR);
         struct vk_graphics_pipeline *lib_gfx_pipeline =
            container_of(lib_pipeline, struct vk_graphics_pipeline, base);

         vk_graphics_pipeline_state_merge(state, &lib_gfx_pipeline->lib.state);

         pipeline->set_layout_count = MAX2(pipeline->set_layout_count,
                                           lib_gfx_pipeline->set_layout_count);
         for (uint32_t i = 0; i < lib_gfx_pipeline->set_layout_count; i++) {
            if (lib_gfx_pipeline->set_layouts[i] == NULL)
               continue;

            if (pipeline->set_layouts[i] == NULL) {
               pipeline->set_layouts[i] =
                  vk_descriptor_set_layout_ref(lib_gfx_pipeline->set_layouts[i]);
            }
         }

         for (uint32_t i = 0; i < lib_gfx_pipeline->stage_count; i++) {
            const struct vk_pipeline_stage *lib_stage =
               &lib_gfx_pipeline->stages[i];

            /* We shouldn't have duplicated stages in the imported pipeline
             * but it's cheap enough to protect against it so we may as well.
             */
            assert(lib_stage->stage < ARRAY_SIZE(stages));
            assert(vk_pipeline_stage_is_null(&stages[lib_stage->stage]));
            if (!vk_pipeline_stage_is_null(&stages[lib_stage->stage]))
               continue;

            stages[lib_stage->stage] = vk_pipeline_stage_clone(lib_stage);
         }
      }
   }

   result = vk_graphics_pipeline_state_fill(device, state,
                                            pCreateInfo,
                                            NULL /* driver_rp */,
                                            0 /* driver_rp_flags */,
                                            all_state,
                                            NULL, 0, NULL);
   if (result != VK_SUCCESS)
      goto fail_stages;

   if (!(pipeline->base.flags & VK_PIPELINE_CREATE_2_LIBRARY_BIT_KHR)) {
      pipeline->linked.dynamic.vi = &pipeline->linked._dynamic_vi;
      pipeline->linked.dynamic.ms.sample_locations =
         &pipeline->linked._dynamic_sl;
      vk_dynamic_graphics_state_fill(&pipeline->linked.dynamic, &state_tmp);
   }

   if (pipeline_layout != NULL) {
      pipeline->set_layout_count = MAX2(pipeline->set_layout_count,
                                        pipeline_layout->set_count);
      for (uint32_t i = 0; i < pipeline_layout->set_count; i++) {
         if (pipeline_layout->set_layouts[i] == NULL)
            continue;

         if (pipeline->set_layouts[i] == NULL) {
            pipeline->set_layouts[i] =
               vk_descriptor_set_layout_ref(pipeline_layout->set_layouts[i]);
         }
      }
   }

   for (uint32_t i = 0; i < pCreateInfo->stageCount; i++) {
      const VkPipelineShaderStageCreateInfo *stage_info =
         &pCreateInfo->pStages[i];

      const int64_t stage_start = os_time_get_nano();

      assert(util_bitcount(stage_info->stage) == 1);
      if (!(state->shader_stages & stage_info->stage))
         continue;

      mesa_shader_stage stage = vk_to_mesa_shader_stage(stage_info->stage);
      assert(vk_device_supports_stage(device, stage));

      stage_feedbacks[stage].flags |=
         VK_PIPELINE_CREATION_FEEDBACK_VALID_BIT;

      if (!vk_pipeline_stage_is_null(&stages[stage]))
         continue;

      struct vk_pipeline_precomp_shader *precomp;
      result = vk_pipeline_precompile_shader(device, cache, pipeline_flags,
                                             pCreateInfo->pNext,
                                             stage_info,
                                             &precomp);
      if (result != VK_SUCCESS)
         goto fail_stages;

      stages[stage] = (struct vk_pipeline_stage) {
         .stage = stage,
         .precomp = precomp,
      };

      const int64_t stage_end = os_time_get_nano();
      stage_feedbacks[stage].duration += stage_end - stage_start;
   }

   /* Compact the array of stages */
   uint32_t stage_count = 0;
   for (uint32_t s = 0; s < ARRAY_SIZE(stages); s++) {
      assert(s >= stage_count);
      if (!vk_pipeline_stage_is_null(&stages[s]))
         stages[stage_count++] = stages[s];
   }
   for (uint32_t s = stage_count; s < ARRAY_SIZE(stages); s++)
      memset(&stages[s], 0, sizeof(stages[s]));

   /* Sort so we always give the driver shaders in order.
    *
    * This makes everything easier for everyone.  This also helps stabilize
    * shader keys so that we get a cache hit even if the client gives us
    * the stages in a different order.
    */
   qsort(stages, stage_count, sizeof(*stages), cmp_vk_pipeline_stages);

   /* Decide whether we should apply link-time optimizations. The spec says:
    *
    *    VK_PIPELINE_CREATE_2_LINK_TIME_OPTIMIZATION_BIT_EXT specifies that
    *    pipeline libraries being linked into this library should have link time
    *    optimizations applied. If this bit is omitted, implementations should
    *    instead perform linking as rapidly as possible.
    *
    *    ...
    *
    *    Using VK_PIPELINE_CREATE_2_LINK_TIME_OPTIMIZATION_BIT_EXT (or not) when
    *    linking pipeline libraries is intended as a performance tradeoff
    *    between host and device. If the bit is omitted, linking should be
    *    faster and produce a pipeline more rapidly, but performance of the
    *    pipeline on the target device may be reduced. If the bit is included,
    *    linking may be slower but should produce a pipeline with device
    *    performance comparable to a monolithically created pipeline.
    *
    * The key phrase here is "pipeline libraries". When we are linking pipeline
    * libraries, we look at this bit to determine whether to apply link-time
    * optimizations. When there are not pipeline libraries, however, we are
    * compiling a monolithic pipeline, which the last sentence implies should
    * always have link-time optimizations applied.
    *
    * Summarizing, we want to link-time optimize monolithic pipelines and
    * non-monolithic pipelines with LINK_TIME_OPTIMIZATION_BIT.
    *
    * (Strictly speaking, there's a corner case here, where a pipeline without
    * LINK_TIME_OPTIMIZATION_BIT links pipeline libraries for graphics state but
    * supplies shaders directly outside of the pipeline library. This logic does
    * not link those shaders, which is a conservative choice. GPL is a disaster
    * of combinatoric complexity, and this simplified approach gets good
    * performance for the cases that actually matter: monolithic, GPL fast link,
    * GPL optimized link.)
    */
    bool lto = libs_info == NULL ||
              (pipeline->base.flags &
               VK_PIPELINE_CREATE_2_LINK_TIME_OPTIMIZATION_BIT_EXT);

   result = vk_graphics_pipeline_compile_shaders(device, cache, pipeline,
                                                 pipeline_layout, state, lto,
                                                 stage_count, stages,
                                                 stage_feedbacks);
   if (result != VK_SUCCESS)
      goto fail_stages;

   /* Throw away precompiled shaders unless the client explicitly asks us to
    * keep them.
    */
   if (!(pipeline_flags &
         VK_PIPELINE_CREATE_2_RETAIN_LINK_TIME_OPTIMIZATION_INFO_BIT_EXT)) {
      for (uint32_t i = 0; i < stage_count; i++) {
         if (stages[i].precomp != NULL) {
            vk_pipeline_precomp_shader_unref(device, stages[i].precomp);
            stages[i].precomp = NULL;
         }
      }
   }

   pipeline->stage_count = stage_count;
   for (uint32_t i = 0; i < stage_count; i++) {
      pipeline->base.stages |= mesa_to_vk_shader_stage(stages[i].stage);
      pipeline->stages[i] = stages[i];
   }

   const int64_t pipeline_end = os_time_get_nano();
   if (feedback_info != NULL) {
      VkPipelineCreationFeedback pipeline_feedback = {
         .flags = VK_PIPELINE_CREATION_FEEDBACK_VALID_BIT,
         .duration = pipeline_end - pipeline_start,
      };

      /* From the Vulkan 1.3.275 spec:
       *
       *    "An implementation should set the
       *    VK_PIPELINE_CREATION_FEEDBACK_APPLICATION_PIPELINE_CACHE_HIT_BIT
       *    bit if it was able to avoid the large majority of pipeline or
       *    pipeline stage creation work by using the pipelineCache parameter"
       *
       * We really shouldn't set this bit unless all the shaders hit the
       * cache.
       */
      uint32_t cache_hit_count = 0;
      for (uint32_t i = 0; i < stage_count; i++) {
         const mesa_shader_stage stage = stages[i].stage;
         if (stage_feedbacks[stage].flags &
             VK_PIPELINE_CREATION_FEEDBACK_APPLICATION_PIPELINE_CACHE_HIT_BIT)
            cache_hit_count++;
      }
      if (cache_hit_count > 0 && cache_hit_count == stage_count) {
         pipeline_feedback.flags |=
            VK_PIPELINE_CREATION_FEEDBACK_APPLICATION_PIPELINE_CACHE_HIT_BIT;
      }

      *feedback_info->pPipelineCreationFeedback = pipeline_feedback;

      /* VUID-VkGraphicsPipelineCreateInfo-pipelineStageCreationFeedbackCount-06594 */
      assert(feedback_info->pipelineStageCreationFeedbackCount == 0 ||
             feedback_info->pipelineStageCreationFeedbackCount ==
             pCreateInfo->stageCount);
      for (uint32_t i = 0;
           i < feedback_info->pipelineStageCreationFeedbackCount; i++) {
         const mesa_shader_stage stage =
            vk_to_mesa_shader_stage(pCreateInfo->pStages[i].stage);

         feedback_info->pPipelineStageCreationFeedbacks[i] =
            stage_feedbacks[stage];
      }
   }

   *pPipeline = vk_pipeline_to_handle(&pipeline->base);

   return VK_SUCCESS;

fail_stages:
   for (uint32_t i = 0; i < ARRAY_SIZE(stages); i++)
      vk_pipeline_stage_finish(device, &stages[i]);

   vk_graphics_pipeline_destroy(device, &pipeline->base, pAllocator);

   return result;
}

VKAPI_ATTR VkResult VKAPI_CALL
vk_common_CreateGraphicsPipelines(VkDevice _device,
                                  VkPipelineCache pipelineCache,
                                  uint32_t createInfoCount,
                                  const VkGraphicsPipelineCreateInfo *pCreateInfos,
                                  const VkAllocationCallbacks *pAllocator,
                                  VkPipeline *pPipelines)
{
   VK_FROM_HANDLE(vk_device, device, _device);
   VK_FROM_HANDLE(vk_pipeline_cache, cache, pipelineCache);
   VkResult first_error_or_success = VK_SUCCESS;

   /* Use implicit pipeline cache if there's no cache set */
   if (!cache && device->mem_cache)
      cache = device->mem_cache;

   /* From the Vulkan 1.3.274 spec:
    *
    *    "When attempting to create many pipelines in a single command, it is
    *    possible that creation may fail for a subset of them. In this case,
    *    the corresponding elements of pPipelines will be set to
    *    VK_NULL_HANDLE.
    */
   memset(pPipelines, 0, createInfoCount * sizeof(*pPipelines));

   unsigned i = 0;
   for (; i < createInfoCount; i++) {
      VkResult result = vk_create_graphics_pipeline(device, cache,
                                                    &pCreateInfos[i],
                                                    pAllocator,
                                                    &pPipelines[i]);
      if (result == VK_SUCCESS)
         continue;

      if (first_error_or_success == VK_SUCCESS)
         first_error_or_success = result;

      /* Bail out on the first error != VK_PIPELINE_COMPILE_REQUIRED as it
       * is not obvious what error should be report upon 2 different failures.
       */
      if (result != VK_PIPELINE_COMPILE_REQUIRED)
         return result;

      const VkPipelineCreateFlags2KHR flags =
         vk_graphics_pipeline_create_flags(&pCreateInfos[i]);
      if (flags & VK_PIPELINE_CREATE_2_EARLY_RETURN_ON_FAILURE_BIT_KHR)
         return result;
   }

   return first_error_or_success;
}

struct vk_compute_pipeline {
   struct vk_pipeline base;
   struct vk_shader *shader;
};

static void
vk_compute_pipeline_destroy(struct vk_device *device,
                            struct vk_pipeline *pipeline,
                            const VkAllocationCallbacks *pAllocator)
{
   struct vk_compute_pipeline *comp_pipeline =
      container_of(pipeline, struct vk_compute_pipeline, base);

   vk_shader_unref(device, comp_pipeline->shader);
   vk_pipeline_free(device, pAllocator, pipeline);
}

static void
vk_compute_pipeline_cmd_bind(struct vk_command_buffer *cmd_buffer,
                             struct vk_pipeline *pipeline)
{
   struct vk_device *device = cmd_buffer->base.device;
   const struct vk_device_shader_ops *ops = device->shader_ops;

   struct vk_shader *shader = NULL;
   if (pipeline != NULL) {
      assert(pipeline->bind_point == VK_PIPELINE_BIND_POINT_COMPUTE);
      struct vk_compute_pipeline *comp_pipeline =
         container_of(pipeline, struct vk_compute_pipeline, base);

      shader = comp_pipeline->shader;

      cmd_buffer->pipeline_shader_stages |= VK_SHADER_STAGE_COMPUTE_BIT;
   } else {
      cmd_buffer->pipeline_shader_stages &= ~VK_SHADER_STAGE_COMPUTE_BIT;
   }

   mesa_shader_stage stage = MESA_SHADER_COMPUTE;
   ops->cmd_bind_shaders(cmd_buffer, 1, &stage, &shader);
}

static VkResult
vk_pipeline_compile_compute_stage(struct vk_device *device,
                                  struct vk_pipeline_cache *cache,
                                  struct vk_compute_pipeline *pipeline,
                                  struct vk_pipeline_layout *pipeline_layout,
                                  struct vk_pipeline_stage *stage,
                                  bool *cache_hit)
{
   const struct vk_device_shader_ops *ops = device->shader_ops;
   VkResult result;

   const VkPushConstantRange *push_range = NULL;
   if (pipeline_layout != NULL) {
      for (uint32_t r = 0; r < pipeline_layout->push_range_count; r++) {
         if (pipeline_layout->push_ranges[r].stageFlags &
             VK_SHADER_STAGE_COMPUTE_BIT) {
            assert(push_range == NULL);
            push_range = &pipeline_layout->push_ranges[r];
         }
      }
   }

   VkShaderCreateFlagsEXT shader_flags =
      vk_pipeline_to_shader_flags(pipeline->base.flags, MESA_SHADER_COMPUTE);

   struct mesa_blake3 blake3_ctx;
   _mesa_blake3_init(&blake3_ctx);

   _mesa_blake3_update(&blake3_ctx, stage->precomp->blake3,
                     sizeof(stage->precomp->blake3));

   _mesa_blake3_update(&blake3_ctx, &shader_flags, sizeof(shader_flags));

   blake3_hash features_blake3;
   ops->hash_state(device->physical, NULL /* state */,
                   &device->enabled_features, VK_SHADER_STAGE_COMPUTE_BIT,
                   features_blake3);
   _mesa_blake3_update(&blake3_ctx, features_blake3, sizeof(features_blake3));

   for (uint32_t i = 0; i < pipeline_layout->set_count; i++) {
      if (pipeline_layout->set_layouts[i] != NULL) {
         _mesa_blake3_update(&blake3_ctx,
                             pipeline_layout->set_layouts[i]->blake3,
                             sizeof(pipeline_layout->set_layouts[i]->blake3));
      }
   }
   if (push_range != NULL)
      _mesa_blake3_update(&blake3_ctx, push_range, sizeof(*push_range));

   struct vk_shader_pipeline_cache_key shader_key = {
      .stage = MESA_SHADER_COMPUTE,
   };
   _mesa_blake3_final(&blake3_ctx, shader_key.blake3);

   if (cache != NULL) {
      struct vk_pipeline_cache_object *cache_obj =
         vk_pipeline_cache_lookup_object(cache, &shader_key,
                                         sizeof(shader_key),
                                         &pipeline_shader_cache_ops,
                                         cache_hit);
      if (cache_obj != NULL) {
         stage->shader = vk_shader_from_cache_obj(cache_obj);
         return VK_SUCCESS;
      }
   }

   if (pipeline->base.flags &
       VK_PIPELINE_CREATE_2_FAIL_ON_PIPELINE_COMPILE_REQUIRED_BIT_KHR)
      return VK_PIPELINE_COMPILE_REQUIRED;

   const struct nir_shader_compiler_options *nir_options =
      ops->get_nir_options(device->physical, stage->stage,
                           &stage->precomp->rs);

   nir_shader *nir = vk_pipeline_precomp_shader_get_nir(stage->precomp,
                                                        nir_options);
   if (nir == NULL)
      return vk_error(device, VK_ERROR_OUT_OF_HOST_MEMORY);

   /* vk_device_shader_ops::compile() consumes the NIR regardless of whether
    * or not it succeeds and only generates shaders on success. Once compile()
    * returns, we own the shaders but not the NIR in infos.
    */
   struct vk_shader_compile_info compile_info = {
      .stage = stage->stage,
      .flags = shader_flags,
      .next_stage_mask = 0,
      .nir = nir,
      .robustness = &stage->precomp->rs,
      .set_layout_count = pipeline_layout->set_count,
      .set_layouts = pipeline_layout->set_layouts,
      .push_constant_range_count = push_range != NULL,
      .push_constant_ranges = push_range != NULL ? push_range : NULL,
   };

   struct vk_shader *shader;
   result = ops->compile(device, 1, &compile_info, NULL,
                         &device->enabled_features, &device->alloc, &shader);
   if (result != VK_SUCCESS)
      return result;

   vk_shader_init_cache_obj(device, shader, &shader_key, sizeof(shader_key));

   struct vk_pipeline_cache_object *cache_obj = &shader->pipeline.cache_obj;
   if (cache != NULL)
      cache_obj = vk_pipeline_cache_add_object(cache, cache_obj);

   stage->shader = vk_shader_from_cache_obj(cache_obj);

   return VK_SUCCESS;
}

static VkResult
vk_compute_pipeline_get_executable_properties(
   struct vk_device *device,
   struct vk_pipeline *pipeline,
   uint32_t *executable_count,
   VkPipelineExecutablePropertiesKHR *properties)
{
   struct vk_compute_pipeline *comp_pipeline =
      container_of(pipeline, struct vk_compute_pipeline, base);
   struct vk_shader *shader = comp_pipeline->shader;

   return shader->ops->get_executable_properties(device, shader,
                                                 executable_count,
                                                 properties);
}

static VkResult
vk_compute_pipeline_get_executable_statistics(
   struct vk_device *device,
   struct vk_pipeline *pipeline,
   uint32_t executable_index,
   uint32_t *statistic_count,
   VkPipelineExecutableStatisticKHR *statistics)
{
   struct vk_compute_pipeline *comp_pipeline =
      container_of(pipeline, struct vk_compute_pipeline, base);
   struct vk_shader *shader = comp_pipeline->shader;

   return shader->ops->get_executable_statistics(device, shader,
                                                 executable_index,
                                                 statistic_count,
                                                 statistics);
}

static VkResult
vk_compute_pipeline_get_internal_representations(
   struct vk_device *device,
   struct vk_pipeline *pipeline,
   uint32_t executable_index,
   uint32_t *internal_representation_count,
   VkPipelineExecutableInternalRepresentationKHR* internal_representations)
{
   struct vk_compute_pipeline *comp_pipeline =
      container_of(pipeline, struct vk_compute_pipeline, base);
   struct vk_shader *shader = comp_pipeline->shader;

   return shader->ops->get_executable_internal_representations(
      device, shader, executable_index,
      internal_representation_count, internal_representations);
}

static struct vk_shader *
vk_compute_pipeline_get_shader(struct vk_pipeline *pipeline,
                               mesa_shader_stage stage)
{
   struct vk_compute_pipeline *comp_pipeline =
      container_of(pipeline, struct vk_compute_pipeline, base);

   assert(stage == MESA_SHADER_COMPUTE);
   return comp_pipeline->shader;
}

static const struct vk_pipeline_ops vk_compute_pipeline_ops = {
   .destroy = vk_compute_pipeline_destroy,
   .get_executable_statistics = vk_compute_pipeline_get_executable_statistics,
   .get_executable_properties = vk_compute_pipeline_get_executable_properties,
   .get_internal_representations = vk_compute_pipeline_get_internal_representations,
   .cmd_bind = vk_compute_pipeline_cmd_bind,
   .get_shader = vk_compute_pipeline_get_shader,
};

static VkResult
vk_create_compute_pipeline(struct vk_device *device,
                           struct vk_pipeline_cache *cache,
                           const VkComputePipelineCreateInfo *pCreateInfo,
                           const VkAllocationCallbacks *pAllocator,
                           VkPipeline *pPipeline)
{
   VK_FROM_HANDLE(vk_pipeline_layout, pipeline_layout, pCreateInfo->layout);
   int64_t pipeline_start = os_time_get_nano();
   VkResult result;

   const VkPipelineCreateFlags2KHR pipeline_flags =
      vk_compute_pipeline_create_flags(pCreateInfo);

   const VkPipelineCreationFeedbackCreateInfo *feedback_info =
      vk_find_struct_const(pCreateInfo->pNext,
                           PIPELINE_CREATION_FEEDBACK_CREATE_INFO);

   struct vk_compute_pipeline *pipeline =
      vk_pipeline_zalloc(device, &vk_compute_pipeline_ops,
                         VK_PIPELINE_BIND_POINT_COMPUTE,
                         pipeline_flags, pAllocator, sizeof(*pipeline));
   if (pipeline == NULL)
      return vk_error(device, VK_ERROR_OUT_OF_HOST_MEMORY);

   pipeline->base.stages = VK_SHADER_STAGE_COMPUTE_BIT;

   struct vk_pipeline_stage stage = {
      .stage = MESA_SHADER_COMPUTE,
   };
   result = vk_pipeline_precompile_shader(device, cache, pipeline_flags,
                                          pCreateInfo->pNext,
                                          &pCreateInfo->stage,
                                          &stage.precomp);
   if (result != VK_SUCCESS)
      goto fail_pipeline;

   bool cache_hit;
   result = vk_pipeline_compile_compute_stage(device, cache, pipeline,
                                              pipeline_layout, &stage,
                                              &cache_hit);
   if (result != VK_SUCCESS)
      goto fail_stage;

   if (stage.precomp != NULL)
      vk_pipeline_precomp_shader_unref(device, stage.precomp);
   pipeline->shader = stage.shader;

   const int64_t pipeline_end = os_time_get_nano();
   if (feedback_info != NULL) {
      VkPipelineCreationFeedback pipeline_feedback = {
         .flags = VK_PIPELINE_CREATION_FEEDBACK_VALID_BIT,
         .duration = pipeline_end - pipeline_start,
      };
      if (cache_hit && cache != device->mem_cache) {
         pipeline_feedback.flags |=
            VK_PIPELINE_CREATION_FEEDBACK_APPLICATION_PIPELINE_CACHE_HIT_BIT;
      }

      *feedback_info->pPipelineCreationFeedback = pipeline_feedback;
      if (feedback_info->pipelineStageCreationFeedbackCount > 0) {
         feedback_info->pPipelineStageCreationFeedbacks[0] =
            pipeline_feedback;
      }
   }

   *pPipeline = vk_pipeline_to_handle(&pipeline->base);

   return VK_SUCCESS;

fail_stage:
   vk_pipeline_stage_finish(device, &stage);
fail_pipeline:
   vk_pipeline_free(device, pAllocator, &pipeline->base);

   return result;
}

VKAPI_ATTR VkResult VKAPI_CALL
vk_common_CreateComputePipelines(VkDevice _device,
                                 VkPipelineCache pipelineCache,
                                 uint32_t createInfoCount,
                                 const VkComputePipelineCreateInfo *pCreateInfos,
                                 const VkAllocationCallbacks *pAllocator,
                                 VkPipeline *pPipelines)
{
   VK_FROM_HANDLE(vk_device, device, _device);
   VK_FROM_HANDLE(vk_pipeline_cache, cache, pipelineCache);
   VkResult first_error_or_success = VK_SUCCESS;

   /* Use implicit pipeline cache if there's no cache set */
   if (!cache && device->mem_cache)
      cache = device->mem_cache;

   /* From the Vulkan 1.3.274 spec:
    *
    *    "When attempting to create many pipelines in a single command, it is
    *    possible that creation may fail for a subset of them. In this case,
    *    the corresponding elements of pPipelines will be set to
    *    VK_NULL_HANDLE.
    */
   memset(pPipelines, 0, createInfoCount * sizeof(*pPipelines));

   unsigned i = 0;
   for (; i < createInfoCount; i++) {
      VkResult result = vk_create_compute_pipeline(device, cache,
                                                   &pCreateInfos[i],
                                                   pAllocator,
                                                   &pPipelines[i]);
      if (result == VK_SUCCESS)
         continue;

      if (first_error_or_success == VK_SUCCESS)
         first_error_or_success = result;

      /* Bail out on the first error != VK_PIPELINE_COMPILE_REQUIRED as it
       * is not obvious what error should be report upon 2 different failures.
       */
      if (result != VK_PIPELINE_COMPILE_REQUIRED)
         return result;

      const VkPipelineCreateFlags2KHR flags =
         vk_compute_pipeline_create_flags(&pCreateInfos[i]);
      if (flags & VK_PIPELINE_CREATE_2_EARLY_RETURN_ON_FAILURE_BIT_KHR)
         return result;
   }

   return first_error_or_success;
}

void
vk_cmd_unbind_pipelines_for_stages(struct vk_command_buffer *cmd_buffer,
                                   VkShaderStageFlags stages)
{
   stages &= cmd_buffer->pipeline_shader_stages;

   if (stages & ~VK_SHADER_STAGE_COMPUTE_BIT)
      vk_graphics_pipeline_cmd_bind(cmd_buffer, NULL);

   if (stages & VK_SHADER_STAGE_COMPUTE_BIT)
      vk_compute_pipeline_cmd_bind(cmd_buffer, NULL);
}

struct vk_rt_stage {
   bool linked : 1;
   struct vk_shader *shader;
};

struct vk_rt_shader_group {
   VkRayTracingShaderGroupTypeKHR type;

   struct vk_rt_stage stages[3];
   uint32_t stage_count;
};

struct vk_rt_pipeline {
   struct vk_pipeline base;

   uint32_t group_count;
   struct vk_rt_shader_group *groups;

   uint32_t stage_count;
   struct vk_rt_stage *stages;

   VkDeviceSize stack_size;
   VkDeviceSize scratch_size;
   uint32_t ray_queries;
};

static struct vk_rt_stage
vk_rt_stage_ref(struct vk_rt_stage *stage)
{
   if (stage->shader)
      vk_shader_ref(stage->shader);
   return *stage;
}

static void
vk_rt_shader_group_destroy(struct vk_device *device,
                           struct vk_rt_shader_group *group)
{
   for (uint32_t i = 0; i < group->stage_count; i++)
      vk_shader_unref(device, group->stages[i].shader);
}

static struct vk_rt_shader_group
vk_rt_shader_group_clone(struct vk_rt_shader_group *other)
{
   struct vk_rt_shader_group group = *other;

   for (uint32_t i = 0; i < ARRAY_SIZE(other->stages); i++)
      group.stages[i] = vk_rt_stage_ref(&other->stages[i]);

   return group;
}

static void
vk_rt_pipeline_destroy(struct vk_device *device,
                       struct vk_pipeline *pipeline,
                       const VkAllocationCallbacks *pAllocator)
{
   struct vk_rt_pipeline *rt_pipeline =
      container_of(pipeline, struct vk_rt_pipeline, base);

   for (uint32_t i = 0; i < rt_pipeline->group_count; i++)
      vk_rt_shader_group_destroy(device, &rt_pipeline->groups[i]);
   for (uint32_t i = 0; i < rt_pipeline->stage_count; i++)
      vk_shader_unref(device, rt_pipeline->stages[i].shader);
   vk_pipeline_free(device, pAllocator, pipeline);
}

static void
vk_rt_pipeline_cmd_bind(struct vk_command_buffer *cmd_buffer,
                        struct vk_pipeline *pipeline)
{
   if (pipeline != NULL) {
      struct vk_device *device = cmd_buffer->base.device;
      const struct vk_device_shader_ops *ops = device->shader_ops;

      struct vk_rt_pipeline *rt_pipeline =
         container_of(pipeline, struct vk_rt_pipeline, base);

      ops->cmd_set_rt_state(cmd_buffer,
                            rt_pipeline->scratch_size,
                            rt_pipeline->ray_queries);

      if (rt_pipeline->stack_size > 0)
         ops->cmd_set_stack_size(cmd_buffer, rt_pipeline->stack_size);

      assert(pipeline->bind_point == VK_PIPELINE_BIND_POINT_RAY_TRACING_KHR);
      cmd_buffer->pipeline_shader_stages |= pipeline->stages;
   } else {
      cmd_buffer->pipeline_shader_stages &= ~(VK_SHADER_STAGE_RAYGEN_BIT_KHR |
                                              VK_SHADER_STAGE_ANY_HIT_BIT_KHR |
                                              VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR |
                                              VK_SHADER_STAGE_MISS_BIT_KHR |
                                              VK_SHADER_STAGE_INTERSECTION_BIT_KHR |
                                              VK_SHADER_STAGE_CALLABLE_BIT_KHR);
   }
}

static uint32_t
stages_mask(uint32_t stage_count, struct vk_pipeline_stage *stages)
{
   uint32_t stage_mask = 0;
   for (uint32_t i = 0; i < stage_count; i++)
      stage_mask |= BITFIELD_BIT(stages[i].stage);

   return stage_mask;
}

static void
hash_rt_parameters(struct mesa_blake3 *blake3_ctx,
                   VkShaderCreateFlagsEXT shader_flags,
                   VkPipelineCreateFlags2KHR _rt_flags,
                   const VkPushConstantRange *push_range,
                   struct vk_pipeline_layout *pipeline_layout)
{
   /* We don't want all the flags to be part of the hash (things like
    * VK_PIPELINE_CREATE_FAIL_ON_PIPELINE_COMPILE_REQUIRED_BIT in
    * particular)
    */
   const VkPipelineCreateFlags2KHR rt_flags =
      _rt_flags & MESA_VK_PIPELINE_RAY_TRACING_FLAGS;

   _mesa_blake3_update(blake3_ctx, &shader_flags, sizeof(shader_flags));
   _mesa_blake3_update(blake3_ctx, &rt_flags, sizeof(rt_flags));

   for (uint32_t i = 0; i < pipeline_layout->set_count; i++) {
      if (pipeline_layout->set_layouts[i] != NULL) {
         _mesa_blake3_update(blake3_ctx,
                             pipeline_layout->set_layouts[i]->blake3,
                             sizeof(pipeline_layout->set_layouts[i]->blake3));
      }
   }
   if (push_range != NULL)
      _mesa_blake3_update(blake3_ctx, push_range, sizeof(*push_range));
}

static VkResult
vk_pipeline_compile_rt_shader(struct vk_device *device,
                              struct vk_pipeline_cache *cache,
                              VkPipelineCreateFlags2KHR pipeline_flags,
                              struct vk_pipeline_layout *pipeline_layout,
                              struct vk_pipeline_stage *stage,
                              VkPipelineCreationFeedback *stage_feedback)
{
   const struct vk_device_shader_ops *ops = device->shader_ops;

   int64_t stage_start = os_time_get_nano();

   const VkPushConstantRange *push_range = NULL;
   if (pipeline_layout != NULL) {
      for (uint32_t r = 0; r < pipeline_layout->push_range_count; r++) {
         if (pipeline_layout->push_ranges[r].stageFlags &
             mesa_to_vk_shader_stage(stage->stage)) {
            assert(push_range == NULL);
            push_range = &pipeline_layout->push_ranges[r];
         }
      }
   }

   struct mesa_blake3 blake3_ctx;
   _mesa_blake3_init(&blake3_ctx);

   VkShaderCreateFlagsEXT shader_flags =
      vk_pipeline_to_shader_flags(pipeline_flags, stage->stage);

   hash_rt_parameters(&blake3_ctx, shader_flags,
                      pipeline_flags,
                      push_range, pipeline_layout);

   _mesa_blake3_update(&blake3_ctx, stage->precomp->blake3,
                       sizeof(stage->precomp->blake3));

   struct vk_shader_pipeline_cache_key shader_key = {
      .stage = stage->stage,
   };
   _mesa_blake3_final(&blake3_ctx, shader_key.blake3);

   if (cache != NULL) {
      bool cache_hit = false;
      struct vk_pipeline_cache_object *cache_obj =
         vk_pipeline_cache_lookup_object(cache, &shader_key,
                                         sizeof(shader_key),
                                         &pipeline_shader_cache_ops,
                                         &cache_hit);
      if (cache_obj != NULL) {
         stage->shader = vk_shader_from_cache_obj(cache_obj);

         if (stage_feedback != NULL) {
            const int64_t stage_end = os_time_get_nano();
            stage_feedback->flags = VK_PIPELINE_CREATION_FEEDBACK_VALID_BIT;
            if (cache_hit) {
               stage_feedback->flags |=
                  VK_PIPELINE_CREATION_FEEDBACK_APPLICATION_PIPELINE_CACHE_HIT_BIT;
            }
            stage_feedback->duration = stage_end - stage_start;
         }

         return VK_SUCCESS;
      }
   }

   if (pipeline_flags &
       VK_PIPELINE_CREATE_2_FAIL_ON_PIPELINE_COMPILE_REQUIRED_BIT_KHR)
      return VK_PIPELINE_COMPILE_REQUIRED;

   const struct nir_shader_compiler_options *nir_options =
      ops->get_nir_options(device->physical, stage->stage, &stage->precomp->rs);
   nir_shader *nir = vk_pipeline_precomp_shader_get_nir(stage->precomp,
                                                        nir_options);
   if (nir == NULL)
      return vk_error(device, VK_ERROR_OUT_OF_HOST_MEMORY);

   /* vk_device_shader_ops::compile() consumes the NIR regardless of
    * whether or not it succeeds and only generates shaders on success.
    * Once compile() returns, we own the shaders but not the NIR in
    * infos.
    */
   struct vk_shader_compile_info compile_info = {
      .stage = stage->stage,
      .flags = shader_flags,
      .rt_flags = pipeline_flags & MESA_VK_PIPELINE_RAY_TRACING_FLAGS,
      .next_stage_mask = 0,
      .nir = nir,
      .robustness = &stage->precomp->rs,
      .set_layout_count = pipeline_layout->set_count,
      .set_layouts = pipeline_layout->set_layouts,
      .push_constant_range_count = push_range != NULL,
      .push_constant_ranges = push_range != NULL ? push_range : NULL,
   };

   struct vk_shader *shader;
   VkResult result = ops->compile(device, 1, &compile_info,
                                  NULL, &device->enabled_features,
                                  &device->alloc, &shader);
   if (result != VK_SUCCESS)
      return result;

   vk_shader_init_cache_obj(device, shader, &shader_key, sizeof(shader_key));

   struct vk_pipeline_cache_object *cache_obj = &shader->pipeline.cache_obj;
   if (cache != NULL)
      cache_obj = vk_pipeline_cache_add_object(cache, cache_obj);

   stage->shader = vk_shader_from_cache_obj(cache_obj);

   if (stage_feedback != NULL) {
      const int64_t stage_end = os_time_get_nano();
      stage_feedback->flags = VK_PIPELINE_CREATION_FEEDBACK_VALID_BIT;
      stage_feedback->duration = stage_end - stage_start;
   }

   return VK_SUCCESS;
}

static VkResult
vk_pipeline_compile_rt_shader_group(struct vk_device *device,
                                    struct vk_pipeline_cache *cache,
                                    VkPipelineCreateFlags2KHR pipeline_flags,
                                    struct vk_pipeline_layout *pipeline_layout,
                                    uint32_t stage_count,
                                    struct vk_pipeline_stage *stages,
                                    bool *all_cache_hit)
{
   const struct vk_device_shader_ops *ops = device->shader_ops;

   assert(stage_count > 1 && stage_count <= 3);

   *all_cache_hit = true;

   struct vk_shader_pipeline_cache_key shader_keys[3];
   bool found_all_shaders = true;
   for (uint32_t i = 0; i < stage_count; i++) {
      struct mesa_blake3 blake3_ctx;
      _mesa_blake3_init(&blake3_ctx);

      const VkPushConstantRange *push_range = NULL;
      if (pipeline_layout != NULL) {
         for (uint32_t r = 0; r < pipeline_layout->push_range_count; r++) {
            if (pipeline_layout->push_ranges[r].stageFlags &
             mesa_to_vk_shader_stage(stages[i].stage)) {
               assert(push_range == NULL);
               push_range = &pipeline_layout->push_ranges[r];
            }
         }
      }

      VkShaderCreateFlagsEXT shader_flags =
         vk_pipeline_to_shader_flags(pipeline_flags, stages[i].stage);

      hash_rt_parameters(&blake3_ctx, shader_flags, pipeline_flags,
                         push_range, pipeline_layout);

      for (uint32_t j = 0; j < stage_count; j++) {
         _mesa_blake3_update(&blake3_ctx, stages[j].precomp->blake3,
                             sizeof(stages[j].precomp->blake3));
      }

      shader_keys[i] = (struct vk_shader_pipeline_cache_key) {
         .stage = stages[i].stage,
      };
      _mesa_blake3_final(&blake3_ctx, shader_keys[i].blake3);

      bool cache_hit = false;
      if (cache != NULL) {
         struct vk_pipeline_cache_object *cache_obj =
            vk_pipeline_cache_lookup_object(cache, &shader_keys[i],
                                            sizeof(shader_keys[i]),
                                            &pipeline_shader_cache_ops,
                                            &cache_hit);
         if (cache_obj != NULL) {
            stages[i].shader = vk_shader_from_cache_obj(cache_obj);
            continue;
         }
      }
      found_all_shaders = false;
      *all_cache_hit &= cache_hit;
   }

   if (found_all_shaders)
      return VK_SUCCESS;

   if (pipeline_flags &
       VK_PIPELINE_CREATE_2_FAIL_ON_PIPELINE_COMPILE_REQUIRED_BIT_KHR)
      return VK_PIPELINE_COMPILE_REQUIRED;

   struct vk_shader_compile_info compile_info[3] = { 0 };
   for (uint32_t i = 0; i < stage_count; i++) {
      if (stages[i].shader) {
         vk_shader_unref(device, stages[i].shader);
         stages[i].shader = NULL;
      }

      const VkPushConstantRange *push_range = NULL;
      if (pipeline_layout != NULL) {
         for (uint32_t r = 0; r < pipeline_layout->push_range_count; r++) {
            if (pipeline_layout->push_ranges[r].stageFlags &
                mesa_to_vk_shader_stage(stages[i].stage)) {
               assert(push_range == NULL);
               push_range = &pipeline_layout->push_ranges[r];
            }
         }
      }

      const struct nir_shader_compiler_options *nir_options =
         ops->get_nir_options(device->physical, stages[i].stage,
                              &stages[i].precomp->rs);

      compile_info[i] = (struct vk_shader_compile_info) {
         .stage = stages[i].stage,
         .flags = vk_pipeline_to_shader_flags(pipeline_flags,
                                              stages[i].stage),
         .rt_flags = pipeline_flags & MESA_VK_PIPELINE_RAY_TRACING_FLAGS,
         .next_stage_mask = 0,
         .nir = vk_pipeline_precomp_shader_get_nir(stages[i].precomp,
                                                   nir_options),
         .robustness = &stages[i].precomp->rs,
         .set_layout_count = pipeline_layout->set_count,
         .set_layouts = pipeline_layout->set_layouts,
         .push_constant_range_count = push_range != NULL,
         .push_constant_ranges = push_range != NULL ? push_range : NULL,
      };

      if (compile_info[i].nir == NULL) {
         for (uint32_t j = 0; j < i; j++)
            ralloc_free(compile_info[i].nir);

         return vk_error(device, VK_ERROR_OUT_OF_HOST_MEMORY);
      }
   }

   struct vk_shader *shaders[3];
   VkResult result = ops->compile(device, stage_count, compile_info,
                                  NULL, &device->enabled_features,
                                  &device->alloc, shaders);
   if (result != VK_SUCCESS)
      return result;

   for (uint32_t i = 0; i < stage_count; i++) {
      vk_shader_init_cache_obj(device, shaders[i],
                               &shader_keys[i], sizeof(shader_keys[i]));

      struct vk_pipeline_cache_object *cache_obj =
         &shaders[i]->pipeline.cache_obj;
      if (cache != NULL)
         cache_obj = vk_pipeline_cache_add_object(cache, cache_obj);

      stages[i].shader = vk_shader_from_cache_obj(cache_obj);
   }

   return VK_SUCCESS;
}

static VkResult
vk_rt_pipeline_get_executable_properties(
   struct vk_device *device,
   struct vk_pipeline *pipeline,
   uint32_t *executable_count,
   VkPipelineExecutablePropertiesKHR *properties)
{
   struct vk_rt_pipeline *rt_pipeline =
      container_of(pipeline, struct vk_rt_pipeline, base);
   VkResult result;

   if (properties == NULL) {
      *executable_count = 0;
      for (uint32_t i = 0; i < rt_pipeline->stage_count; i++) {
         struct vk_shader *shader = rt_pipeline->stages[i].shader;

         uint32_t shader_exec_count = 0;
         result = shader->ops->get_executable_properties(device, shader,
                                                         &shader_exec_count,
                                                         NULL);
         assert(shader_exec_count == 1);
         assert(result == VK_SUCCESS);
         *executable_count += shader_exec_count;
      }
   } else {
      uint32_t arr_len = *executable_count;
      *executable_count = 0;
      for (uint32_t i = 0; i < rt_pipeline->stage_count; i++) {
         struct vk_shader *shader = rt_pipeline->stages[i].shader;

         uint32_t shader_exec_count = arr_len - *executable_count;
         result = shader->ops->get_executable_properties(device, shader,
                                                         &shader_exec_count,
                                                         &properties[*executable_count]);
         if (result != VK_SUCCESS)
            return result;

         assert(shader_exec_count == 1);
         *executable_count += shader_exec_count;
      }
   }

   return VK_SUCCESS;
}

static VkResult
vk_rt_pipeline_get_executable_statistics(
   struct vk_device *device,
   struct vk_pipeline *pipeline,
   uint32_t executable_index,
   uint32_t *statistic_count,
   VkPipelineExecutableStatisticKHR *statistics)
{
   struct vk_rt_pipeline *rt_pipeline =
      container_of(pipeline, struct vk_rt_pipeline, base);
   assert(executable_index < rt_pipeline->stage_count);
   struct vk_shader *shader = rt_pipeline->stages[executable_index].shader;

   return shader->ops->get_executable_statistics(device, shader, 0,
                                                 statistic_count,
                                                 statistics);
}

static VkResult
vk_rt_pipeline_get_internal_representations(
   struct vk_device *device,
   struct vk_pipeline *pipeline,
   uint32_t executable_index,
   uint32_t *internal_representation_count,
   VkPipelineExecutableInternalRepresentationKHR* internal_representations)
{
   struct vk_rt_pipeline *rt_pipeline =
      container_of(pipeline, struct vk_rt_pipeline, base);
   assert(executable_index < rt_pipeline->stage_count);
   struct vk_shader *shader = rt_pipeline->stages[executable_index].shader;

   return shader->ops->get_executable_internal_representations(
      device, shader, 0,
      internal_representation_count, internal_representations);
}

static struct vk_shader *
vk_rt_pipeline_get_shader(struct vk_pipeline *pipeline,
                          mesa_shader_stage stage)
{
   UNREACHABLE("Invalid operation");
}

static const struct vk_pipeline_ops vk_rt_pipeline_ops = {
   .destroy = vk_rt_pipeline_destroy,
   .get_executable_statistics = vk_rt_pipeline_get_executable_statistics,
   .get_executable_properties = vk_rt_pipeline_get_executable_properties,
   .get_internal_representations = vk_rt_pipeline_get_internal_representations,
   .cmd_bind = vk_rt_pipeline_cmd_bind,
   .get_shader = vk_rt_pipeline_get_shader,
};

static bool
is_rt_stack_size_dynamic(const VkRayTracingPipelineCreateInfoKHR *info)
{
   if (info->pDynamicState == NULL)
      return false;

   for (unsigned i = 0; i < info->pDynamicState->dynamicStateCount; i++) {
      if (info->pDynamicState->pDynamicStates[i] ==
          VK_DYNAMIC_STATE_RAY_TRACING_PIPELINE_STACK_SIZE_KHR)
         return true;
   }

   return false;
}

static int
cmp_vk_rt_pipeline_stages(const void *_a, const void *_b)
{
   const struct vk_rt_stage *a = _a, *b = _b;
   return vk_shader_cmp_rt_stages(a->shader->stage, b->shader->stage);
}

static VkResult
vk_create_rt_pipeline(struct vk_device *device,
                      struct vk_pipeline_cache *cache,
                      const VkRayTracingPipelineCreateInfoKHR *pCreateInfo,
                      const VkAllocationCallbacks *pAllocator,
                      VkPipeline *pPipeline)
{
   const struct vk_device_shader_ops *ops = device->shader_ops;

   VK_FROM_HANDLE(vk_pipeline_layout, pipeline_layout, pCreateInfo->layout);
   int64_t pipeline_start = os_time_get_nano();
   VkResult result;

   const VkPipelineCreateFlags2KHR pipeline_flags =
      vk_rt_pipeline_create_flags(pCreateInfo);

   const VkPipelineCreationFeedbackCreateInfo *feedback_info =
      vk_find_struct_const(pCreateInfo->pNext,
                           PIPELINE_CREATION_FEEDBACK_CREATE_INFO);

   struct vk_pipeline_stage *stages = NULL;
   if (pCreateInfo->stageCount > 0) {
      stages = vk_zalloc2(&device->alloc, pAllocator,
                          pCreateInfo->stageCount * sizeof(*stages), 8,
                          VK_SYSTEM_ALLOCATION_SCOPE_COMMAND);
      if (stages == NULL)
         return vk_error(device, VK_ERROR_OUT_OF_HOST_MEMORY);
   }

   uint32_t libraries_stage_count = 0;
   uint32_t libraries_group_count = 0;
   const VkPipelineLibraryCreateInfoKHR *libs_info = pCreateInfo->pLibraryInfo;
   if (libs_info != NULL) {
      for (uint32_t i = 0; i < libs_info->libraryCount; i++) {
         VK_FROM_HANDLE(vk_pipeline, lib_pipeline, libs_info->pLibraries[i]);
         assert(lib_pipeline->bind_point == VK_PIPELINE_BIND_POINT_RAY_TRACING_KHR);
         assert(lib_pipeline->flags & VK_PIPELINE_CREATE_2_LIBRARY_BIT_KHR);
         struct vk_rt_pipeline *lib_rt_pipeline =
            container_of(lib_pipeline, struct vk_rt_pipeline, base);

         libraries_stage_count += lib_rt_pipeline->stage_count;
         libraries_group_count += lib_rt_pipeline->group_count;
      }
   }

   VK_MULTIALLOC(ma);
   VK_MULTIALLOC_DECL(&ma, struct vk_rt_pipeline, _pipeline, 1);
   VK_MULTIALLOC_DECL(&ma, struct vk_rt_stage, pipeline_stages,
                      libraries_stage_count + pCreateInfo->stageCount);
   VK_MULTIALLOC_DECL(&ma, struct vk_rt_shader_group, pipeline_groups,
                      libraries_group_count + pCreateInfo->groupCount);

   struct vk_rt_pipeline *pipeline =
      vk_pipeline_multizalloc(device, &ma, &vk_rt_pipeline_ops,
                              VK_PIPELINE_BIND_POINT_RAY_TRACING_KHR,
                              pipeline_flags, pAllocator);
   if (pipeline == NULL) {
      result = vk_error(device, VK_ERROR_OUT_OF_HOST_MEMORY);
      goto fail_stages;
   }

   pipeline->stages = pipeline_stages;
   pipeline->groups = pipeline_groups;

   bool all_cache_hit = true;

   uint32_t stack_max[MESA_SHADER_KERNEL] = { 0 };

   /* Load/Compile individual shaders */
   for (uint32_t i = 0; i < pCreateInfo->stageCount; i++) {
      const VkPipelineShaderStageCreateInfo *stage_info =
         &pCreateInfo->pStages[i];

      pipeline->base.stages |= pCreateInfo->pStages[i].stage;
      stages[i] = (struct vk_pipeline_stage) {
         .stage = vk_to_mesa_shader_stage(stage_info->stage),
      };

      result = vk_pipeline_precompile_shader(device, cache, pipeline_flags,
                                             pCreateInfo->pNext,
                                             stage_info,
                                             &stages[i].precomp);
      if (result != VK_SUCCESS)
         goto fail_stages_compile;

      VkPipelineCreationFeedback feedback = { 0 };
      result = vk_pipeline_compile_rt_shader(device, cache,
                                             pipeline_flags,
                                             pipeline_layout,
                                             &stages[i],
                                             &feedback);

      if ((feedback.flags &
           VK_PIPELINE_CREATION_FEEDBACK_APPLICATION_PIPELINE_CACHE_HIT_BIT) == 0 &&
          (pipeline->base.flags &
           VK_PIPELINE_CREATE_2_FAIL_ON_PIPELINE_COMPILE_REQUIRED_BIT_KHR)) {
         result = VK_PIPELINE_COMPILE_REQUIRED;
         goto fail_stages_compile;
      }

      if (result != VK_SUCCESS)
         goto fail_stages_compile;


      /* No need to take a reference, either the pipeline creation succeeds
       * and the ownership is transfered from from stages[] to the pipeline or
       * it fails and all stages[] elements are unref.
       */
      pipeline->stages[pipeline->stage_count++] = (struct vk_rt_stage) {
         .shader = stages[i].shader,
      };

      stack_max[stages[i].stage] = MAX2(stages[i].shader->stack_size,
                                        stack_max[stages[i].stage]);
      pipeline->scratch_size = MAX2(stages[i].shader->scratch_size,
                                    pipeline->scratch_size);
      pipeline->ray_queries = MAX2(stages[i].shader->ray_queries,
                                   pipeline->ray_queries);

      if (feedback_info &&
          feedback_info->pipelineStageCreationFeedbackCount > 0) {
         feedback_info->pPipelineStageCreationFeedbacks[i] = feedback;
         all_cache_hit &= !!(feedback.flags &
                             VK_PIPELINE_CREATION_FEEDBACK_APPLICATION_PIPELINE_CACHE_HIT_BIT);
      }
   }

   /* Create/Compile groups */
   for (uint32_t i = 0; i < pCreateInfo->groupCount; i++) {
      const VkRayTracingShaderGroupCreateInfoKHR *group_info =
         &pCreateInfo->pGroups[i];
      struct vk_rt_shader_group *group = &pipeline->groups[i];

      group->type = group_info->type;

      struct vk_pipeline_stage group_stages[3];
      uint32_t group_stage_count = 0;
      switch (group_info->type) {
      case VK_RAY_TRACING_SHADER_GROUP_TYPE_GENERAL_KHR:
         assert(group_info->generalShader < pCreateInfo->stageCount);
         group_stages[group_stage_count++] = stages[group_info->generalShader];
         break;

      case VK_RAY_TRACING_SHADER_GROUP_TYPE_TRIANGLES_HIT_GROUP_KHR:
         if (group_info->anyHitShader < pCreateInfo->stageCount)
            group_stages[group_stage_count++] = stages[group_info->anyHitShader];
         if (group_info->closestHitShader < pCreateInfo->stageCount)
            group_stages[group_stage_count++] = stages[group_info->closestHitShader];
         break;

      case VK_RAY_TRACING_SHADER_GROUP_TYPE_PROCEDURAL_HIT_GROUP_KHR:
         if (group_info->closestHitShader < pCreateInfo->stageCount)
            group_stages[group_stage_count++] = stages[group_info->closestHitShader];
         if (group_info->anyHitShader < pCreateInfo->stageCount)
            group_stages[group_stage_count++] = stages[group_info->anyHitShader];
         assert(group_info->intersectionShader < pCreateInfo->stageCount);
         group_stages[group_stage_count++] = stages[group_info->intersectionShader];
         break;

      default:
         UNREACHABLE("Invalid shader group");
      }
      assert(group_stage_count <= ARRAY_SIZE(group_stages));

      VkShaderStageFlags group_stages_flags = 0;
      for (uint32_t s = 0; s < group_stage_count; s++)
         group_stages_flags |= mesa_to_vk_shader_stage(group_stages[s].stage);

      VkShaderStageFlags group_linked_stages = ops->get_rt_group_linking != NULL ?
         ops->get_rt_group_linking(device->physical, group_stages_flags) : 0;

      struct vk_pipeline_stage linked_stages[3];
      uint32_t linked_stage_count = 0;
      if (group_linked_stages) {
         assert(util_bitcount(group_linked_stages) > 1);

         /* Build of list of shader to link */
         for (uint32_t s = 0; s < group_stage_count; s++) {
            if (mesa_to_vk_shader_stage(group_stages[s].stage) &
                group_linked_stages) {
               linked_stages[linked_stage_count] = group_stages[s];
               linked_stages[linked_stage_count].shader = NULL;
               linked_stage_count++;
            }
         }
      }

      if (linked_stage_count > 0) {
         assert(linked_stage_count > 1);

         bool cache_hit;
         result = vk_pipeline_compile_rt_shader_group(device, cache, pipeline_flags,
                                                      pipeline_layout,
                                                      linked_stage_count,
                                                      linked_stages,
                                                      &cache_hit);
         if (result != VK_SUCCESS)
            goto fail_stages_compile;

         all_cache_hit &= cache_hit;
      }

      for (uint32_t s = 0; s < linked_stage_count; s++) {
         group->stages[group->stage_count++] = (struct vk_rt_stage) {
            .shader = linked_stages[s].shader,
            .linked = true,
         };
      }
      for (uint32_t s = 0; s < group_stage_count; s++) {
         if (mesa_to_vk_shader_stage(
                group_stages[s].stage) & group_linked_stages)
            continue;

         group->stages[group->stage_count++] = (struct vk_rt_stage) {
            .shader = vk_shader_ref(group_stages[s].shader),
         };
      }

      qsort(group->stages, group->stage_count, sizeof(*group->stages),
            cmp_vk_rt_pipeline_stages);

      for (uint32_t s = 0; s < group->stage_count; s++) {
         pipeline->ray_queries =
            MAX2(group->stages[s].shader->ray_queries, pipeline->ray_queries);
         pipeline->scratch_size =
            MAX2(group->stages[s].shader->scratch_size, pipeline->scratch_size);
      }

      pipeline->group_count++;
   }

   /* Throw away precompiled shaders, unlike GPL, we never do linking with
    * shaders coming from libraries.
    */
   for (uint32_t i = 0; i < pCreateInfo->stageCount; i++)
      vk_pipeline_precomp_shader_unref(device, stages[i].precomp);

   /* Import libraries */
   if (libs_info) {
      for (uint32_t i = 0; i < libs_info->libraryCount; i++) {
         VK_FROM_HANDLE(vk_pipeline, lib_pipeline, libs_info->pLibraries[i]);
         struct vk_rt_pipeline *lib_rt_pipeline =
            container_of(lib_pipeline, struct vk_rt_pipeline, base);

         /* Import library shaders */
         for (uint32_t s = 0; s < lib_rt_pipeline->stage_count; s++) {
            pipeline->stages[pipeline->stage_count++] =
               vk_rt_stage_ref(&lib_rt_pipeline->stages[s]);
         }

         /* Import library groups */
         for (uint32_t g = 0; g < lib_rt_pipeline->group_count; g++) {
            pipeline->groups[pipeline->group_count++] =
               vk_rt_shader_group_clone(&lib_rt_pipeline->groups[g]);
         }
      }
   }

   /* Compute final stats */
   for (uint32_t i = 0; i < pipeline->stage_count; i++) {
      struct vk_shader *shader = pipeline->stages[i].shader;

      stack_max[shader->stage] =
         MAX2(shader->stack_size, stack_max[shader->stage]);

      pipeline->base.stages |= mesa_to_vk_shader_stage(shader->stage);
      pipeline->scratch_size = MAX2(shader->scratch_size, pipeline->scratch_size);
      pipeline->ray_queries = MAX2(shader->ray_queries, pipeline->ray_queries);
      pipeline->stack_size = MAX2(shader->stack_size, pipeline->stack_size);
   }
   for (uint32_t g = 0; g < pipeline->group_count; g++) {
      const struct vk_rt_shader_group *group = &pipeline->groups[g];
      for (uint32_t s = 0; s < group->stage_count; s++) {
         struct vk_shader *shader = group->stages[s].shader;

         stack_max[shader->stage] =
            MAX2(shader->stack_size, stack_max[shader->stage]);

         pipeline->base.stages |=
            mesa_to_vk_shader_stage(group->stages[s].shader->stage);
         pipeline->scratch_size =
            MAX2(shader->scratch_size, pipeline->scratch_size);
         pipeline->ray_queries = MAX2(shader->ray_queries, pipeline->ray_queries);
         pipeline->stack_size = MAX2(shader->stack_size, pipeline->stack_size);
      }
   }

   if (is_rt_stack_size_dynamic(pCreateInfo)) {
      pipeline->stack_size = 0; /* 0 means dynamic */
   } else {
      /* From the Vulkan spec:
       *
       *    "If the stack size is not set explicitly, the stack size for a
       *    pipeline is:
       *
       *       rayGenStackMax +
       *       min(1, maxPipelineRayRecursionDepth) ×
       *       max(closestHitStackMax, missStackMax,
       *           intersectionStackMax + anyHitStackMax) +
       *       max(0, maxPipelineRayRecursionDepth-1) ×
       *       max(closestHitStackMax, missStackMax) +
       *       2 × callableStackMax"
       */
      pipeline->stack_size = MAX2(
         pipeline->stack_size,
         stack_max[MESA_SHADER_RAYGEN] +
         MIN2(1, pCreateInfo->maxPipelineRayRecursionDepth) *
         MAX4(stack_max[MESA_SHADER_CLOSEST_HIT],
              stack_max[MESA_SHADER_MISS],
              stack_max[MESA_SHADER_INTERSECTION],
              stack_max[MESA_SHADER_ANY_HIT]) +
         MAX2(0, (int)pCreateInfo->maxPipelineRayRecursionDepth - 1) *
         MAX2(stack_max[MESA_SHADER_CLOSEST_HIT],
              stack_max[MESA_SHADER_MISS]) +
         2 * stack_max[MESA_SHADER_CALLABLE]);

      /* This is an extremely unlikely case but we need to set it to some
       * non-zero value so that we don't accidentally think it's dynamic.
       */
      if (pipeline->stack_size == 0)
         pipeline->stack_size = 1;
   }

   const int64_t pipeline_end = os_time_get_nano();
   if (feedback_info != NULL) {
      VkPipelineCreationFeedback pipeline_feedback = {
         .flags = VK_PIPELINE_CREATION_FEEDBACK_VALID_BIT,
         .duration = pipeline_end - pipeline_start,
      };
      if (all_cache_hit && cache != device->mem_cache) {
         pipeline_feedback.flags |=
            VK_PIPELINE_CREATION_FEEDBACK_APPLICATION_PIPELINE_CACHE_HIT_BIT;
      }

      *feedback_info->pPipelineCreationFeedback = pipeline_feedback;
   }

   *pPipeline = vk_pipeline_to_handle(&pipeline->base);

   return VK_SUCCESS;

 fail_stages_compile:
   for (uint32_t i = 0; i < pipeline->group_count; i++)
      vk_rt_shader_group_destroy(device, &pipeline->groups[i]);
   for (uint32_t i = 0; i < pipeline->stage_count; i++)
      vk_shader_unref(device, pipeline->stages[i].shader);
   for (uint32_t i = 0; i < pCreateInfo->stageCount && stages[i].precomp != NULL; i++)
      vk_pipeline_precomp_shader_unref(device, stages[i].precomp);
   vk_pipeline_free(device, pAllocator, &pipeline->base);
 fail_stages:
   vk_free(&device->alloc, stages);

   return result;
}

VKAPI_ATTR VkResult VKAPI_CALL
vk_common_CreateRayTracingPipelinesKHR(
   VkDevice                                    _device,
   VkDeferredOperationKHR                      deferredOperation,
   VkPipelineCache                             pipelineCache,
   uint32_t                                    createInfoCount,
   const VkRayTracingPipelineCreateInfoKHR*    pCreateInfos,
   const VkAllocationCallbacks*                pAllocator,
   VkPipeline*                                 pPipelines)
{
   VK_FROM_HANDLE(vk_device, device, _device);
   VK_FROM_HANDLE(vk_pipeline_cache, cache, pipelineCache);
   VkResult first_error_or_success = VK_SUCCESS;

   /* Use implicit pipeline cache if there's no cache set */
   if (!cache && device->mem_cache)
      cache = device->mem_cache;

   /* From the Vulkan 1.3.274 spec:
    *
    *    "When attempting to create many pipelines in a single command, it is
    *    possible that creation may fail for a subset of them. In this case,
    *    the corresponding elements of pPipelines will be set to
    *    VK_NULL_HANDLE.
    */
   memset(pPipelines, 0, createInfoCount * sizeof(*pPipelines));

   unsigned i = 0;
   for (; i < createInfoCount; i++) {
      VkResult result = vk_create_rt_pipeline(device, cache,
                                              &pCreateInfos[i],
                                              pAllocator,
                                              &pPipelines[i]);
      if (result == VK_SUCCESS)
         continue;

      if (first_error_or_success == VK_SUCCESS)
         first_error_or_success = result;

      /* Bail out on the first error != VK_PIPELINE_COMPILE_REQUIRED as it
       * is not obvious what error should be report upon 2 different failures.
       */
      if (result != VK_PIPELINE_COMPILE_REQUIRED)
         return result;

      const VkPipelineCreateFlags2KHR flags =
         vk_rt_pipeline_create_flags(&pCreateInfos[i]);
      if (flags & VK_PIPELINE_CREATE_2_EARLY_RETURN_ON_FAILURE_BIT_KHR)
         return result;
   }

   return first_error_or_success;
}

VKAPI_ATTR VkResult VKAPI_CALL
vk_common_GetRayTracingShaderGroupHandlesKHR(
    VkDevice                                    _device,
    VkPipeline                                  _pipeline,
    uint32_t                                    firstGroup,
    uint32_t                                    groupCount,
    size_t                                      dataSize,
    void*                                       pData)
{
   VK_FROM_HANDLE(vk_device, device, _device);
   VK_FROM_HANDLE(vk_pipeline, pipeline, _pipeline);
   const struct vk_device_shader_ops *ops = device->shader_ops;

   assert(pipeline->bind_point == VK_PIPELINE_BIND_POINT_RAY_TRACING_KHR);

   struct vk_rt_pipeline *rt_pipeline =
      container_of(pipeline, struct vk_rt_pipeline, base);

   assert(dataSize >= device->physical->properties.shaderGroupHandleSize * groupCount);
   assert(firstGroup + groupCount <= rt_pipeline->group_count);

   for (uint32_t i = 0; i < groupCount; i++) {
      struct vk_rt_shader_group *group = &rt_pipeline->groups[firstGroup + i];
      struct vk_shader *shaders[3];
      for (uint32_t s = 0; s < group->stage_count; s++)
         shaders[s] = group->stages[s].shader;

      ops->write_rt_shader_group(device, group->type,
                                 (const struct vk_shader **)shaders,
                                 group->stage_count, pData);

      pData = (uint8_t *)pData + device->physical->properties.shaderGroupHandleSize;
   }

   return VK_SUCCESS;
}

VKAPI_ATTR VkResult VKAPI_CALL
vk_common_GetRayTracingCaptureReplayShaderGroupHandlesKHR(
    VkDevice                                    _device,
    VkPipeline                                  _pipeline,
    uint32_t                                    firstGroup,
    uint32_t                                    groupCount,
    size_t                                      dataSize,
    void*                                       pData)
{
   VK_FROM_HANDLE(vk_device, device, _device);
   VK_FROM_HANDLE(vk_pipeline, pipeline, _pipeline);
   const struct vk_device_shader_ops *ops = device->shader_ops;

   assert(pipeline->bind_point == VK_PIPELINE_BIND_POINT_RAY_TRACING_KHR);

   struct vk_rt_pipeline *rt_pipeline =
      container_of(pipeline, struct vk_rt_pipeline, base);

   assert(dataSize >= device->physical->properties.shaderGroupHandleSize * groupCount);
   assert(firstGroup + groupCount <= rt_pipeline->group_count);

   for (uint32_t i = 0; i < groupCount; i++) {
      struct vk_rt_shader_group *group = &rt_pipeline->groups[firstGroup + i];
      struct vk_shader *shaders[3] = { 0 };
      for (uint32_t s = 0; s < group->stage_count; s++)
         shaders[s] = group->stages[s].shader;

      ops->write_rt_shader_group_replay_handle(device,
                                               (const struct vk_shader **)shaders,
                                               group->stage_count, pData);

      pData = (uint8_t *)pData + device->physical->properties.shaderGroupHandleCaptureReplaySize;
   }

   return VK_SUCCESS;
}

VKAPI_ATTR VkDeviceSize VKAPI_CALL
vk_common_GetRayTracingShaderGroupStackSizeKHR(
    VkDevice                                    device,
    VkPipeline                                  _pipeline,
    uint32_t                                    _group,
    VkShaderGroupShaderKHR                      groupShader)
{
   VK_FROM_HANDLE(vk_pipeline, pipeline, _pipeline);
   assert(pipeline->bind_point == VK_PIPELINE_BIND_POINT_RAY_TRACING_KHR);

   struct vk_rt_pipeline *rt_pipeline =
      container_of(pipeline, struct vk_rt_pipeline, base);

   assert(_group < rt_pipeline->group_count);

   struct vk_rt_shader_group *group = &rt_pipeline->groups[_group];

   struct vk_shader *shader = NULL;
   for (uint32_t i = 0; i < group->stage_count; i++) {
      switch (groupShader) {
      case VK_SHADER_GROUP_SHADER_GENERAL_KHR:
         shader = (group->stages[i].shader->stage == MESA_SHADER_RAYGEN ||
                   group->stages[i].shader->stage == MESA_SHADER_CALLABLE ||
                   group->stages[i].shader->stage == MESA_SHADER_MISS) ?
            group->stages[i].shader : NULL;
         break;

      case VK_SHADER_GROUP_SHADER_CLOSEST_HIT_KHR:
         shader = group->stages[i].shader->stage == MESA_SHADER_CLOSEST_HIT ?
            group->stages[i].shader : NULL;
         break;

      case VK_SHADER_GROUP_SHADER_ANY_HIT_KHR:
         shader = group->stages[i].shader->stage == MESA_SHADER_ANY_HIT ?
            group->stages[i].shader : NULL;
         break;

      case VK_SHADER_GROUP_SHADER_INTERSECTION_KHR:
         shader = group->stages[i].shader->stage == MESA_SHADER_INTERSECTION ?
            group->stages[i].shader : NULL;
         break;

      default:
         UNREACHABLE("Invalid VkShaderGroupShader enum");
      }

      if (shader != NULL)
         break;
   }

   return shader ? shader->stack_size : 0;
}

VKAPI_ATTR void VKAPI_CALL
vk_common_CmdSetRayTracingPipelineStackSizeKHR(
    VkCommandBuffer                             commandBuffer,
    uint32_t                                    pipelineStackSize)
{
   VK_FROM_HANDLE(vk_command_buffer, cmd_buffer, commandBuffer);
   struct vk_device *device = cmd_buffer->base.device;
   const struct vk_device_shader_ops *ops = device->shader_ops;

   ops->cmd_set_stack_size(cmd_buffer, pipelineStackSize);
}
