/*
 * Copyright © 2019 Red Hat.
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

#include "lvp_private.h"
#include "lvp_conv.h"
#include "lvp_acceleration_structure.h"

#include "pipe-loader/pipe_loader.h"
#include "git_sha1.h"
#include "vk_cmd_enqueue_entrypoints.h"
#include "vk_sampler.h"
#include "vk_util.h"
#include "util/detect.h"
#include "pipe/p_defines.h"
#include "pipe/p_state.h"
#include "pipe/p_context.h"
#include "draw/draw_context.h"
#include "frontend/drisw_api.h"

#include "util/u_inlines.h"
#include "util/os_file.h"
#include "util/os_memory.h"
#include "util/os_time.h"
#include "util/u_thread.h"
#include "util/u_atomic.h"
#include "util/timespec.h"
#include "util/ptralloc.h"
#include "nir.h"
#include "nir_builder.h"

#if DETECT_OS_LINUX
#include <sys/mman.h>
#include <sys/resource.h>
#endif

#if DETECT_OS_ANDROID
#include "vk_android.h"
#endif

#if defined(VK_USE_PLATFORM_WAYLAND_KHR) || \
    defined(VK_USE_PLATFORM_WIN32_KHR) || \
    defined(VK_USE_PLATFORM_XCB_KHR) || \
    defined(VK_USE_PLATFORM_XLIB_KHR) || \
    defined(VK_USE_PLATFORM_METAL_EXT)
#define LVP_USE_WSI_PLATFORM
#endif

#if LLVM_VERSION_MAJOR >= 10
#define LVP_API_VERSION VK_MAKE_VERSION(1, 4, VK_HEADER_VERSION)
#else
#define LVP_API_VERSION VK_MAKE_VERSION(1, 3, VK_HEADER_VERSION)
#endif

#define LVP_SAMPLE_COUNTS (VK_SAMPLE_COUNT_1_BIT | VK_SAMPLE_COUNT_4_BIT | \
                           VK_SAMPLE_COUNT_8_BIT)

VKAPI_ATTR VkResult VKAPI_CALL lvp_EnumerateInstanceVersion(uint32_t* pApiVersion)
{
   *pApiVersion = LVP_API_VERSION;
   return VK_SUCCESS;
}

static const struct vk_instance_extension_table lvp_instance_extensions_supported = {
   .KHR_device_group_creation                = true,
   .KHR_external_fence_capabilities          = true,
   .KHR_external_memory_capabilities         = true,
   .KHR_external_semaphore_capabilities      = true,
   .KHR_get_physical_device_properties2      = true,
   .EXT_debug_report                         = true,
   .EXT_debug_utils                          = true,
#ifdef LVP_USE_WSI_PLATFORM
   .KHR_get_surface_capabilities2            = true,
   .KHR_surface                              = true,
   .KHR_surface_protected_capabilities       = true,
   .EXT_swapchain_colorspace                 = true,
   .EXT_surface_maintenance1                 = true,
#endif
#ifdef VK_USE_PLATFORM_WAYLAND_KHR
   .KHR_wayland_surface                      = true,
#endif
#ifdef VK_USE_PLATFORM_WIN32_KHR
   .KHR_win32_surface                        = true,
#endif
#ifdef VK_USE_PLATFORM_XCB_KHR
   .KHR_xcb_surface                          = true,
#endif
#ifdef VK_USE_PLATFORM_XLIB_KHR
   .KHR_xlib_surface                         = true,
#endif
#ifdef VK_USE_PLATFORM_METAL_EXT
   .EXT_metal_surface                        = true,
#endif
#ifndef VK_USE_PLATFORM_WIN32_KHR
   .EXT_headless_surface                     = true,
#endif
};

static const struct vk_device_extension_table lvp_device_extensions_supported = {
   .KHR_8bit_storage                      = true,
   .KHR_16bit_storage                     = true,
   .KHR_acceleration_structure            = true,
   .KHR_bind_memory2                      = true,
   .KHR_buffer_device_address             = true,
   .KHR_create_renderpass2                = true,
   .KHR_compute_shader_derivatives        = true,
   .KHR_copy_commands2                    = true,
   .KHR_copy_memory_indirect              = true,
   .KHR_dedicated_allocation              = true,
   .KHR_deferred_host_operations          = true,
   .KHR_depth_stencil_resolve             = true,
   .KHR_descriptor_update_template        = true,
   .KHR_device_group                      = true,
   .KHR_draw_indirect_count               = true,
   .KHR_driver_properties                 = true,
   .KHR_dynamic_rendering                 = true,
   .KHR_dynamic_rendering_local_read      = true,
   .KHR_format_feature_flags2             = true,
   .KHR_external_fence                    = true,
   .KHR_external_memory                   = true,
#ifdef PIPE_MEMORY_FD
   .KHR_external_memory_fd                = true,
#endif
   .KHR_external_semaphore                = true,
   .KHR_shader_float_controls             = true,
   .KHR_shader_float_controls2            = true,
   .KHR_get_memory_requirements2          = true,
   .KHR_global_priority                   = true,
#ifdef LVP_USE_WSI_PLATFORM
   .KHR_incremental_present               = true,
#endif
   .KHR_image_format_list                 = true,
   .KHR_imageless_framebuffer             = true,
   .KHR_index_type_uint8                  = true,
   .KHR_line_rasterization                = true,
   .KHR_load_store_op_none                = true,
   .KHR_maintenance1                      = true,
   .KHR_maintenance2                      = true,
   .KHR_maintenance3                      = true,
   .KHR_maintenance4                      = true,
   .KHR_maintenance5                      = true,
   .KHR_maintenance6                      = true,
   .KHR_maintenance7                      = true,
   .KHR_maintenance8                      = true,
   .KHR_maintenance9                      = true,
   .KHR_maintenance10                     = true,
   .KHR_map_memory2                       = true,
   .KHR_multiview                         = true,
   .KHR_push_descriptor                   = true,
   .KHR_pipeline_library                  = true,
   .KHR_ray_query                         = true,
   .KHR_ray_tracing_maintenance1          = true,
   .KHR_ray_tracing_pipeline              = true,
   .KHR_ray_tracing_position_fetch        = true,
   .KHR_relaxed_block_layout              = true,
   .KHR_sampler_mirror_clamp_to_edge      = true,
   .KHR_sampler_ycbcr_conversion          = true,
   .KHR_separate_depth_stencil_layouts    = true,
   .KHR_shader_atomic_int64               = true,
   .KHR_shader_clock                      = true,
   .KHR_shader_draw_parameters            = true,
   .KHR_shader_expect_assume              = true,
   .KHR_shader_float16_int8               = true,
   .KHR_shader_integer_dot_product        = true,
   .KHR_shader_maximal_reconvergence      = true,
   .KHR_shader_non_semantic_info          = true,
   .KHR_shader_quad_control               = true,
   .KHR_shader_relaxed_extended_instruction = true,
   .KHR_shader_subgroup_extended_types    = true,
   .KHR_shader_subgroup_rotate            = true,
   .KHR_shader_terminate_invocation       = true,
   .KHR_spirv_1_4                         = true,
   .KHR_storage_buffer_storage_class      = true,
#ifdef LVP_USE_WSI_PLATFORM
   .KHR_swapchain                         = true,
   .KHR_swapchain_mutable_format          = true,
#endif
   .KHR_synchronization2                  = true,
   .KHR_timeline_semaphore                = true,
   .KHR_uniform_buffer_standard_layout    = true,
   .KHR_unified_image_layouts             = true,
   .KHR_variable_pointers                 = true,
   .KHR_vertex_attribute_divisor          = true,
   .KHR_vulkan_memory_model               = true,
   .KHR_workgroup_memory_explicit_layout  = true,
   .KHR_zero_initialize_workgroup_memory  = true,
   .ARM_rasterization_order_attachment_access = true,
   .EXT_4444_formats                      = true,
   .EXT_attachment_feedback_loop_layout   = true,
   .EXT_attachment_feedback_loop_dynamic_state = true,
   .EXT_border_color_swizzle              = true,
   .EXT_calibrated_timestamps             = true,
   .EXT_color_write_enable                = true,
   .EXT_conditional_rendering             = true,
   .EXT_depth_bias_control                = true,
   .EXT_depth_clip_enable                 = true,
   .EXT_depth_clip_control                = true,
   .EXT_depth_range_unrestricted          = true,
   .EXT_dynamic_rendering_unused_attachments = true,
   .EXT_descriptor_buffer                 = true,
   .EXT_descriptor_indexing               = true,
   .EXT_device_generated_commands         = true,
   .EXT_extended_dynamic_state            = true,
   .EXT_extended_dynamic_state2           = true,
   .EXT_extended_dynamic_state3           = true,
   .EXT_external_memory_host              = true,
   .EXT_fragment_shader_interlock         = true,
   .EXT_graphics_pipeline_library         = true,
   .EXT_hdr_metadata = true,
   .EXT_host_image_copy                   = true,
   .EXT_host_query_reset                  = true,
   .EXT_image_2d_view_of_3d               = true,
   .EXT_image_sliced_view_of_3d           = true,
   .EXT_image_robustness                  = true,
   .EXT_index_type_uint8                  = true,
   .EXT_inline_uniform_block              = true,
   .EXT_load_store_op_none                = true,
   .EXT_legacy_vertex_attributes          = true,
   .EXT_memory_budget                     = true,
#if DETECT_OS_LINUX
   .EXT_memory_priority                   = true,
#endif
   .EXT_mesh_shader                       = true,
   .EXT_multisampled_render_to_single_sampled = true,
   .EXT_multi_draw                        = true,
   .EXT_mutable_descriptor_type           = true,
   .EXT_nested_command_buffer             = true,
   .EXT_non_seamless_cube_map             = true,
#if DETECT_OS_LINUX
   .EXT_pageable_device_local_memory      = true,
#endif
   .EXT_pipeline_creation_feedback        = true,
   .EXT_pipeline_creation_cache_control   = true,
   .EXT_pipeline_library_group_handles    = true,
   .EXT_pipeline_protected_access         = true,
   .EXT_pipeline_robustness               = true,
   .EXT_post_depth_coverage               = true,
   .EXT_private_data                      = true,
   .EXT_primitives_generated_query        = true,
   .EXT_primitive_topology_list_restart   = true,
   .EXT_rasterization_order_attachment_access = true,
   .EXT_queue_family_foreign              = true,
   .EXT_sample_locations                  = true,
   .EXT_sampler_filter_minmax             = true,
   .EXT_scalar_block_layout               = true,
   .EXT_separate_stencil_usage            = true,
   .EXT_shader_atomic_float               = true,
   .EXT_shader_atomic_float2              = true,
   .EXT_shader_demote_to_helper_invocation= true,
   .EXT_shader_image_atomic_int64         = true,
   .EXT_shader_object                     = true,
   .EXT_shader_replicated_composites      = true,
   .EXT_shader_stencil_export             = true,
   .EXT_shader_subgroup_ballot            = true,
   .EXT_shader_subgroup_vote              = true,
   .EXT_shader_viewport_index_layer       = true,
   .EXT_subgroup_size_control             = true,
#ifdef LVP_USE_WSI_PLATFORM
   .EXT_swapchain_maintenance1            = true,
#endif
   .EXT_texel_buffer_alignment            = true,
   .EXT_tooling_info                      = true,
   .EXT_transform_feedback                = true,
   .EXT_vertex_attribute_divisor          = true,
   .EXT_vertex_input_dynamic_state        = true,
   .EXT_ycbcr_image_arrays                = true,
   .EXT_ycbcr_2plane_444_formats          = true,
   .EXT_custom_border_color               = true,
   .EXT_provoking_vertex                  = true,
   .EXT_line_rasterization                = true,
   .EXT_robustness2                       = true,
   .EXT_zero_initialize_device_memory     = true,
   .AMDX_shader_enqueue                   = true,
#if DETECT_OS_ANDROID
   .ANDROID_native_buffer                 = true,
#endif
   .GOOGLE_decorate_string                = true,
   .GOOGLE_hlsl_functionality1            = true,
};

static bool
assert_memhandle_type(VkExternalMemoryHandleTypeFlags types)
{
   unsigned valid[] = {
      VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT,
      VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT,
   };
   for (unsigned i = 0; i < ARRAY_SIZE(valid); i++) {
      if (types & valid[i])
         types &= ~valid[i];
   }
   u_foreach_bit(type, types)
      mesa_loge("lavapipe: unimplemented external memory type %u", 1<<type);
   return types == 0;
}

static enum lvp_device_memory_type
lvp_device_memory_type_for_handle_types(const struct lvp_physical_device *pdevice,
                                        VkExternalMemoryHandleTypeFlags types)
{
   if (types == 0)
      return LVP_DEVICE_MEMORY_TYPE_DEFAULT;

#ifdef PIPE_MEMORY_FD
   if (types & (VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT |
                VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT)) {
      assert(!(types & ~(VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT |
                         VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT)));

#ifdef HAVE_LIBDRM
      int dmabuf_bits = DRM_PRIME_CAP_EXPORT | DRM_PRIME_CAP_IMPORT;
      if ((pdevice->pscreen->caps.dmabuf & dmabuf_bits) == dmabuf_bits) {
         /* If we have full dma-buf support, everything is a dma-buf */
         return LVP_DEVICE_MEMORY_TYPE_DMA_BUF;
      }

      if (types & VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT) {
         /* dma-buf is only supported for import so if we see dma-buf it has
          * to come by itself.
          */
         assert(types == VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT);
         return LVP_DEVICE_MEMORY_TYPE_DMA_BUF;
      }
#endif

      assert(types == VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT);
      return LVP_DEVICE_MEMORY_TYPE_OPAQUE_FD;
   }
#endif

   if (types & VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT) {
      /* These can only be used for import so it's a single bit */
      assert(util_bitcount(types) == 1);
      return LVP_DEVICE_MEMORY_TYPE_USER_PTR;
   }

   UNREACHABLE("Unsupported import/export type");
}

static unsigned min_shader_cap(struct pipe_screen *pscreen,
                               mesa_shader_stage shader,
                               unsigned cap_offset)
{
   unsigned val = UINT_MAX;
   for (int i = 0; i <= shader; ++i) {
      if (!pscreen->shader_caps[i].max_instructions)
         continue;
      val = MIN2(val, *(unsigned *)((char *)&pscreen->shader_caps[i] + cap_offset));
   }
   return val;
}

static bool and_shader_cap(struct pipe_screen *pscreen,
                           unsigned cap_offset)
{
   bool val = true;
   for (int i = 0; i <= MESA_SHADER_COMPUTE; ++i) {
      if (!pscreen->shader_caps[i].max_instructions)
         continue;
      val &= *(bool *)((char *)&pscreen->shader_caps[i] + cap_offset);
   }
   return val;
}

#define MIN_VERTEX_PIPELINE_CAP(pscreen, cap) \
   min_shader_cap(pscreen, MESA_SHADER_GEOMETRY, offsetof(struct pipe_shader_caps, cap))

#define MIN_SHADER_CAP(pscreen, cap) \
   min_shader_cap(pscreen, MESA_SHADER_COMPUTE, offsetof(struct pipe_shader_caps, cap))

#define AND_SHADER_CAP(pscreen, cap) \
   and_shader_cap(pscreen, offsetof(struct pipe_shader_caps, cap))

static void
lvp_get_features(const struct lvp_physical_device *pdevice,
                 struct vk_features *features)
{
   bool instance_divisor = pdevice->pscreen->caps.vertex_element_instance_divisor != 0;

   *features = (struct vk_features){
      /* Vulkan 1.0 */
      .robustBufferAccess                       = true,
      .fullDrawIndexUint32                      = true,
      .imageCubeArray                           = (pdevice->pscreen->caps.cube_map_array != 0),
      .independentBlend                         = true,
      .geometryShader                           = (pdevice->pscreen->shader_caps[MESA_SHADER_GEOMETRY].max_instructions != 0),
      .tessellationShader                       = (pdevice->pscreen->shader_caps[MESA_SHADER_TESS_EVAL].max_instructions != 0),
      .sampleRateShading                        = (pdevice->pscreen->caps.sample_shading != 0),
      .dualSrcBlend                             = (pdevice->pscreen->caps.max_dual_source_render_targets != 0),
      .logicOp                                  = true,
      .multiDrawIndirect                        = (pdevice->pscreen->caps.multi_draw_indirect != 0),
      .drawIndirectFirstInstance                = true,
      .depthClamp                               = (pdevice->pscreen->caps.depth_clip_disable != 0),
      .depthBiasClamp                           = true,
      .fillModeNonSolid                         = true,
      .depthBounds                              = (pdevice->pscreen->caps.depth_bounds_test != 0),
      .wideLines                                = true,
      .largePoints                              = true,
      .alphaToOne                               = true,
      .multiViewport                            = true,
      .samplerAnisotropy                        = true,
      .textureCompressionETC2                   = false,
      .textureCompressionASTC_LDR               = false,
      .textureCompressionBC                     = true,
      .occlusionQueryPrecise                    = true,
      .pipelineStatisticsQuery                  = true,
      .vertexPipelineStoresAndAtomics           = (MIN_VERTEX_PIPELINE_CAP(pdevice->pscreen, max_shader_buffers) != 0),
      .fragmentStoresAndAtomics                 = (pdevice->pscreen->shader_caps[MESA_SHADER_FRAGMENT].max_shader_buffers != 0),
      .shaderTessellationAndGeometryPointSize   = true,
      .shaderImageGatherExtended                = true,
      .shaderStorageImageExtendedFormats        = (MIN_SHADER_CAP(pdevice->pscreen, max_shader_images) != 0),
      .shaderStorageImageMultisample            = (pdevice->pscreen->caps.texture_multisample != 0),
      .shaderUniformBufferArrayDynamicIndexing  = true,
      .shaderSampledImageArrayDynamicIndexing   = true,
      .shaderStorageBufferArrayDynamicIndexing  = true,
      .shaderStorageImageArrayDynamicIndexing   = true,
      .shaderStorageImageReadWithoutFormat      = true,
      .shaderStorageImageWriteWithoutFormat     = true,
      .shaderClipDistance                       = true,
      .shaderCullDistance                       = (pdevice->pscreen->caps.cull_distance == 1),
      .shaderFloat64                            = (pdevice->pscreen->caps.doubles == 1),
      .shaderInt64                              = (pdevice->pscreen->caps.int64 == 1),
      .shaderInt16                              = AND_SHADER_CAP(pdevice->pscreen, int16),
      .variableMultisampleRate                  = false,
      .inheritedQueries                         = false,
      .shaderResourceMinLod                     = true,
      .sparseBinding                            = DETECT_OS_LINUX,
      .sparseResidencyBuffer                    = DETECT_OS_LINUX,
      .sparseResidencyImage2D                   = DETECT_OS_LINUX,
      .sparseResidencyImage3D                   = DETECT_OS_LINUX,
      .sparseResidencyAliased                   = DETECT_OS_LINUX,
      .shaderResourceResidency                  = DETECT_OS_LINUX,

      /* Vulkan 1.1 */
      .storageBuffer16BitAccess            = true,
      .uniformAndStorageBuffer16BitAccess  = true,
      .storagePushConstant16               = true,
      .storageInputOutput16                = false,
      .multiview                           = true,
      .multiviewGeometryShader             = true,
      .multiviewTessellationShader         = true,
      .variablePointersStorageBuffer       = true,
      .variablePointers                    = true,
      .protectedMemory                     = false,
      .samplerYcbcrConversion              = true,
      .shaderDrawParameters                = true,

      /* Vulkan 1.2 */
      .samplerMirrorClampToEdge = true,
      .drawIndirectCount = true,
      .storageBuffer8BitAccess = true,
      .uniformAndStorageBuffer8BitAccess = true,
      .storagePushConstant8 = true,
      .shaderBufferInt64Atomics = true,
      .shaderSharedInt64Atomics = true,
      .shaderFloat16 = pdevice->pscreen->shader_caps[MESA_SHADER_FRAGMENT].fp16,
      .shaderInt8 = true,

      .descriptorIndexing = true,
      .shaderInputAttachmentArrayDynamicIndexing = true,
      .shaderUniformTexelBufferArrayDynamicIndexing = true,
      .shaderStorageTexelBufferArrayDynamicIndexing = true,
      .shaderUniformBufferArrayNonUniformIndexing = true,
      .shaderSampledImageArrayNonUniformIndexing = true,
      .shaderStorageBufferArrayNonUniformIndexing = true,
      .shaderStorageImageArrayNonUniformIndexing = true,
      .shaderInputAttachmentArrayNonUniformIndexing = true,
      .shaderUniformTexelBufferArrayNonUniformIndexing = true,
      .shaderStorageTexelBufferArrayNonUniformIndexing = true,
      .descriptorBindingUniformBufferUpdateAfterBind = true,
      .descriptorBindingSampledImageUpdateAfterBind = true,
      .descriptorBindingStorageImageUpdateAfterBind = true,
      .descriptorBindingStorageBufferUpdateAfterBind = true,
      .descriptorBindingUniformTexelBufferUpdateAfterBind = true,
      .descriptorBindingStorageTexelBufferUpdateAfterBind = true,
      .descriptorBindingUpdateUnusedWhilePending = true,
      .descriptorBindingPartiallyBound = true,
      .descriptorBindingVariableDescriptorCount = true,
      .runtimeDescriptorArray = true,

      .samplerFilterMinmax = true,
      .scalarBlockLayout = true,
      .imagelessFramebuffer = true,
      .uniformBufferStandardLayout = true,
      .shaderSubgroupExtendedTypes = true,
      .separateDepthStencilLayouts = true,
      .hostQueryReset = true,
      .timelineSemaphore = true,
      .bufferDeviceAddress = true,
      .bufferDeviceAddressCaptureReplay = false,
      .bufferDeviceAddressMultiDevice = false,
      .vulkanMemoryModel = true,
      .vulkanMemoryModelDeviceScope = true,
      .vulkanMemoryModelAvailabilityVisibilityChains = true,
      .shaderOutputViewportIndex = true,
      .shaderOutputLayer = true,
      .subgroupBroadcastDynamicId = true,

      /* Vulkan 1.3 */
      .robustImageAccess = true,
      .inlineUniformBlock = true,
      .descriptorBindingInlineUniformBlockUpdateAfterBind = true,
      .pipelineCreationCacheControl = true,
      .privateData = true,
      .shaderDemoteToHelperInvocation = true,
      .shaderTerminateInvocation = true,
      .subgroupSizeControl = true,
      .computeFullSubgroups = true,
      .synchronization2 = true,
      .textureCompressionASTC_HDR = VK_FALSE,
      .shaderZeroInitializeWorkgroupMemory = true,
      .dynamicRendering = true,
      .shaderIntegerDotProduct = true,
      .maintenance4 = true,

      /* Vulkan 1.4 */
      .globalPriorityQuery = true,
      .shaderSubgroupRotate = true,
      .shaderSubgroupRotateClustered = true,
      .shaderFloatControls2 = true,
      .shaderExpectAssume = true,
      .rectangularLines = true,
      .bresenhamLines = true,
      .smoothLines = true,
      .stippledRectangularLines = true,
      .stippledBresenhamLines = true,
      .stippledSmoothLines = true,
      .vertexAttributeInstanceRateDivisor = instance_divisor,
      .vertexAttributeInstanceRateZeroDivisor = instance_divisor,
      .indexTypeUint8 = true,
      .dynamicRenderingLocalRead = true,
      .maintenance5 = true,
      .maintenance6 = true,
      .pipelineRobustness = true,
      .hostImageCopy = true,
      .pushDescriptor = true,

      /* VK_KHR_acceleration_structure */
      .accelerationStructure = true,
      .accelerationStructureCaptureReplay = false,
      .accelerationStructureIndirectBuild = false,
      .accelerationStructureHostCommands = false,
      .descriptorBindingAccelerationStructureUpdateAfterBind = true,

      /* VK_EXT_descriptor_buffer */
      .descriptorBuffer = true,
      .descriptorBufferCaptureReplay = false,
      .descriptorBufferPushDescriptors = true,
      .descriptorBufferImageLayoutIgnored = true,

      /* VK_EXT_primitives_generated_query */
      .primitivesGeneratedQuery = true,
      .primitivesGeneratedQueryWithRasterizerDiscard = true,
      .primitivesGeneratedQueryWithNonZeroStreams = true,

      /* VK_EXT_border_color_swizzle */
      .borderColorSwizzle = true,
      .borderColorSwizzleFromImage = true,

      /* VK_EXT_non_seamless_cube_map */
      .nonSeamlessCubeMap = true,

      /* VK_EXT_attachment_feedback_loop_layout */
      .attachmentFeedbackLoopLayout = true,

      /* VK_EXT_pipeline_protected_access */
      .pipelineProtectedAccess = true,

      /* VK_EXT_rasterization_order_attachment_access */
      .rasterizationOrderColorAttachmentAccess = true,
      .rasterizationOrderDepthAttachmentAccess = true,
      .rasterizationOrderStencilAttachmentAccess = true,

      /* VK_EXT_multisampled_render_to_single_sampled */
      .multisampledRenderToSingleSampled = true,

      /* VK_EXT_mutable_descriptor_type */
      .mutableDescriptorType = true,

      /* VK_EXT_vertex_input_dynamic_state */
      .vertexInputDynamicState = true,

      /* VK_EXT_image_sliced_view_of_3d */
      .imageSlicedViewOf3D = true,

      /* VK_EXT_depth_bias_control */
      .depthBiasControl = true,
      .leastRepresentableValueForceUnormRepresentation = true,
      .floatRepresentation = true,
      .depthBiasExact = true,

      /* VK_EXT_depth_clip_control */
      .depthClipControl = true,

      /* VK_EXT_attachment_feedback_loop_layout_dynamic_state */
      .attachmentFeedbackLoopDynamicState = true,

      /* VK_KHR_ray_query */
      .rayQuery = true,

      /* VK_KHR_ray_tracing_maintenance1 */
      .rayTracingMaintenance1 = true,
      .rayTracingPipelineTraceRaysIndirect2 = true,

      /* VK_KHR_ray_tracing_pipeline */
      .rayTracingPipeline = true,
      .rayTracingPipelineShaderGroupHandleCaptureReplay = false,
      .rayTracingPipelineShaderGroupHandleCaptureReplayMixed = false,
      .rayTracingPipelineTraceRaysIndirect = true,
      .rayTraversalPrimitiveCulling = true,

      /* VK_EXT_pipeline_library_group_handles */
      .pipelineLibraryGroupHandles = true,

      /* VK_KHR_ray_tracing_position_fetch */
      .rayTracingPositionFetch = true,

      /* VK_EXT_shader_object */
      .shaderObject = true,

      /* VK_EXT_shader_replicated_composites */
      .shaderReplicatedComposites = true,

      /* VK_KHR_shader_clock */
      .shaderSubgroupClock = true,
      .shaderDeviceClock = true,

      /* VK_EXT_texel_buffer_alignment */
      .texelBufferAlignment = true,

      /* VK_EXT_transform_feedback */
      .transformFeedback = true,
      .geometryStreams = true,

      /* VK_EXT_conditional_rendering */
      .conditionalRendering = true,
      .inheritedConditionalRendering = false,

      /* VK_EXT_extended_dynamic_state */
      .extendedDynamicState = true,

      /* VK_EXT_4444_formats */
      .formatA4R4G4B4 = true,
      .formatA4B4G4R4 = true,

      /* VK_EXT_custom_border_color */
      .customBorderColors = true,
      .customBorderColorWithoutFormat = true,

      /* VK_EXT_color_write_enable */
      .colorWriteEnable = true,

      /* VK_EXT_image_2d_view_of_3d  */
      .image2DViewOf3D = true,
      .sampler2DViewOf3D = true,

      /* VK_EXT_provoking_vertex */
      .provokingVertexLast = true,
      .transformFeedbackPreservesProvokingVertex = true,

      /* VK_EXT_multi_draw */
      .multiDraw = true,

      /* VK_EXT_zero_initialize_device_memory */
      .zeroInitializeDeviceMemory = true,

      /* VK_EXT_depth_clip_enable */
      .depthClipEnable = (pdevice->pscreen->caps.depth_clamp_enable != 0),

      /* VK_EXT_extended_dynamic_state2 */
      .extendedDynamicState2 = true,
      .extendedDynamicState2LogicOp = true,
      .extendedDynamicState2PatchControlPoints = true,

      /* VK_EXT_extended_dynamic_state3 */
      .extendedDynamicState3PolygonMode = true,
      .extendedDynamicState3TessellationDomainOrigin = true,
      .extendedDynamicState3DepthClampEnable = true,
      .extendedDynamicState3DepthClipEnable = true,
      .extendedDynamicState3LogicOpEnable = true,
      .extendedDynamicState3SampleMask = true,
      .extendedDynamicState3RasterizationSamples = true,
      .extendedDynamicState3AlphaToCoverageEnable = true,
      .extendedDynamicState3AlphaToOneEnable = true,
      .extendedDynamicState3DepthClipNegativeOneToOne = true,
      .extendedDynamicState3RasterizationStream = false,
      .extendedDynamicState3ConservativeRasterizationMode = false,
      .extendedDynamicState3ExtraPrimitiveOverestimationSize = false,
      .extendedDynamicState3LineRasterizationMode = true,
      .extendedDynamicState3LineStippleEnable = true,
      .extendedDynamicState3ProvokingVertexMode = true,
      .extendedDynamicState3SampleLocationsEnable = false,
      .extendedDynamicState3ColorBlendEnable = true,
      .extendedDynamicState3ColorBlendEquation = true,
      .extendedDynamicState3ColorWriteMask = true,
      .extendedDynamicState3ViewportWScalingEnable = false,
      .extendedDynamicState3ViewportSwizzle = false,
      .extendedDynamicState3ShadingRateImageEnable = false,
      .extendedDynamicState3CoverageToColorEnable = false,
      .extendedDynamicState3CoverageToColorLocation = false,
      .extendedDynamicState3CoverageModulationMode = false,
      .extendedDynamicState3CoverageModulationTableEnable = false,
      .extendedDynamicState3CoverageModulationTable = false,
      .extendedDynamicState3CoverageReductionMode = false,
      .extendedDynamicState3RepresentativeFragmentTestEnable = false,
      .extendedDynamicState3ColorBlendAdvanced = false,

      /* VK_EXT_dynamic_rendering_unused_attachments */
      .dynamicRenderingUnusedAttachments = true,

      /* VK_EXT_robustness2 */
      .robustBufferAccess2 = true,
      .robustImageAccess2 = true,
      .nullDescriptor = true,

      /* VK_EXT_device_generated_commands */
      .deviceGeneratedCommands = true,
      .dynamicGeneratedPipelineLayout = true,

      /* VK_EXT_primitive_topology_list_restart */
      .primitiveTopologyListRestart = true,
      .primitiveTopologyPatchListRestart = true,

      /* VK_EXT_graphics_pipeline_library */
      .graphicsPipelineLibrary = true,

      /* VK_EXT_shader_atomic_float */
      .shaderBufferFloat32Atomics =    true,
      .shaderBufferFloat32AtomicAdd =  true,
      .shaderBufferFloat64Atomics =    false,
      .shaderBufferFloat64AtomicAdd =  false,
      .shaderSharedFloat32Atomics =    true,
      .shaderSharedFloat32AtomicAdd =  true,
      .shaderSharedFloat64Atomics =    false,
      .shaderSharedFloat64AtomicAdd =  false,
      .shaderImageFloat32Atomics =     true,
      .shaderImageFloat32AtomicAdd =   true,
      .sparseImageFloat32Atomics =     DETECT_OS_LINUX,
      .sparseImageFloat32AtomicAdd =   DETECT_OS_LINUX,

      /* VK_EXT_shader_atomic_float2 */
      .shaderBufferFloat16Atomics      = false,
      .shaderBufferFloat16AtomicAdd    = false,
      .shaderBufferFloat16AtomicMinMax = false,
      .shaderBufferFloat32AtomicMinMax = LLVM_VERSION_MAJOR >= 15,
      .shaderBufferFloat64AtomicMinMax = false,
      .shaderSharedFloat16Atomics      = false,
      .shaderSharedFloat16AtomicAdd    = false,
      .shaderSharedFloat16AtomicMinMax = false,
      .shaderSharedFloat32AtomicMinMax = LLVM_VERSION_MAJOR >= 15,
      .shaderSharedFloat64AtomicMinMax = false,
      .shaderImageFloat32AtomicMinMax  = LLVM_VERSION_MAJOR >= 15,
      .sparseImageFloat32AtomicMinMax  = false,

      /* VK_EXT_shader_image_atomic_int64 */
      .shaderImageInt64Atomics = true,
      .sparseImageInt64Atomics = true,

      /* VK_KHR_copy_memory_indirect */
      .indirectMemoryCopy = true,
      .indirectMemoryToImageCopy = true,

      /* VK_EXT_memory_priority */
      .memoryPriority = true,

      /* VK_EXT_legacy_vertex_attributes */
      .legacyVertexAttributes = true,

      /* VK_EXT_pageable_device_local_memory */
      .pageableDeviceLocalMemory = true,

      /* VK_EXT_nested_command_buffer */
      .nestedCommandBuffer = true,
      .nestedCommandBufferRendering = true,
      .nestedCommandBufferSimultaneousUse = true,

      /* VK_EXT_mesh_shader */
      .taskShader = true,
      .meshShader = true,
      .multiviewMeshShader = false,
      .primitiveFragmentShadingRateMeshShader = false,
      .meshShaderQueries = true,

      /* VK_EXT_ycbcr_2plane_444_formats */
      .ycbcr2plane444Formats = true,

      /* VK_EXT_ycbcr_image_arrays */
      .ycbcrImageArrays = true,

      /* maintenance7 */
      .maintenance7 = true,
      /* maintenance8 */
      .maintenance8 = true,
      /* maintenance9 */
      .maintenance9 = true,
      /* maintenance10 */
      .maintenance10 = true,

      /* VK_KHR_shader_maximal_reconvergence */
      .shaderMaximalReconvergence = true,

      /* VK_AMDX_shader_enqueue */
#ifdef VK_ENABLE_BETA_EXTENSIONS
      .shaderEnqueue = true,
#endif

#ifdef LVP_USE_WSI_PLATFORM
      /* VK_EXT_swapchain_maintenance1 */
      .swapchainMaintenance1 = true,
#endif

      /* VK_KHR_shader_relaxed_extended_instruction */
      .shaderRelaxedExtendedInstruction = true,

      /* VK_KHR_compute_shader_derivatives */
      .computeDerivativeGroupQuads = true,
      .computeDerivativeGroupLinear = true,

      /* VK_KHR_shader_quad_control */
      .shaderQuadControl = true,

      /* VK_EXT_fragment_shader_interlock */
      .fragmentShaderSampleInterlock = true,
      .fragmentShaderPixelInterlock = true,
      .fragmentShaderShadingRateInterlock = false,

      /* VK_KHR_workgroup_memory_explicit_layout */
      .workgroupMemoryExplicitLayout = true,
      .workgroupMemoryExplicitLayoutScalarBlockLayout = true,
      .workgroupMemoryExplicitLayout8BitAccess = true,
      .workgroupMemoryExplicitLayout16BitAccess = true,

      /* VK_KHR_unified_image_layouts */
      .unifiedImageLayouts = true,
      .unifiedImageLayoutsVideo = true,
   };
}

extern unsigned lp_native_vector_width;

static VkImageLayout lvp_host_copy_image_layouts[] = {
   VK_IMAGE_LAYOUT_GENERAL,
   VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
   VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
   VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL,
   VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
   VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
   VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
   VK_IMAGE_LAYOUT_PREINITIALIZED,
   VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_STENCIL_ATTACHMENT_OPTIMAL,
   VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_STENCIL_READ_ONLY_OPTIMAL,
   VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL,
   VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_OPTIMAL,
   VK_IMAGE_LAYOUT_STENCIL_ATTACHMENT_OPTIMAL,
   VK_IMAGE_LAYOUT_STENCIL_READ_ONLY_OPTIMAL,
   VK_IMAGE_LAYOUT_READ_ONLY_OPTIMAL,
   VK_IMAGE_LAYOUT_ATTACHMENT_OPTIMAL,
   VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
   VK_IMAGE_LAYOUT_VIDEO_DECODE_DST_KHR,
   VK_IMAGE_LAYOUT_VIDEO_DECODE_SRC_KHR,
   VK_IMAGE_LAYOUT_VIDEO_DECODE_DPB_KHR,
   VK_IMAGE_LAYOUT_SHARED_PRESENT_KHR,
   VK_IMAGE_LAYOUT_FRAGMENT_DENSITY_MAP_OPTIMAL_EXT,
   VK_IMAGE_LAYOUT_FRAGMENT_SHADING_RATE_ATTACHMENT_OPTIMAL_KHR,
};

static void
lvp_get_properties(const struct lvp_physical_device *device, struct vk_properties *p)
{
   const unsigned *grid_size = device->pscreen->compute_caps.max_grid_size;
   const unsigned *block_size = device->pscreen->compute_caps.max_block_size;

   const uint64_t max_render_targets = device->pscreen->caps.max_render_targets;

   int texel_buffer_alignment = device->pscreen->caps.texture_buffer_offset_alignment;

   STATIC_ASSERT(sizeof(struct lp_descriptor) <= 256);
   *p = (struct vk_properties) {
      /* Vulkan 1.0 */
      .apiVersion = LVP_API_VERSION,
      .driverVersion = vk_get_driver_version(),
      .vendorID = VK_VENDOR_ID_MESA,
      .deviceID = 0,
      .deviceType = VK_PHYSICAL_DEVICE_TYPE_CPU,
      .maxImageDimension1D                      = device->pscreen->caps.max_texture_2d_size,
      .maxImageDimension2D                      = device->pscreen->caps.max_texture_2d_size,
      .maxImageDimension3D                      = (1 << device->pscreen->caps.max_texture_3d_levels),
      .maxImageDimensionCube                    = (1 << device->pscreen->caps.max_texture_cube_levels),
      .maxImageArrayLayers                      = device->pscreen->caps.max_texture_array_layers,
      .maxTexelBufferElements                   = device->pscreen->caps.max_texel_buffer_elements,
      .maxUniformBufferRange                    = MIN_SHADER_CAP(device->pscreen, max_const_buffer0_size),
      .maxStorageBufferRange                    = device->pscreen->caps.max_shader_buffer_size,
      .maxPushConstantsSize                     = MAX_PUSH_CONSTANTS_SIZE,
      .maxMemoryAllocationCount                 = UINT32_MAX,
      .maxSamplerAllocationCount                = 32 * 1024,
      .bufferImageGranularity                   = 64, /* A cache line */
      .sparseAddressSpaceSize                   = 2UL*1024*1024*1024,
      .maxBoundDescriptorSets                   = MAX_SETS,
      .maxPerStageDescriptorSamplers            = MAX_DESCRIPTORS,
      .maxPerStageDescriptorUniformBuffers      = MAX_DESCRIPTORS,
      .maxPerStageDescriptorStorageBuffers      = MAX_DESCRIPTORS,
      .maxPerStageDescriptorSampledImages       = MAX_DESCRIPTORS,
      .maxPerStageDescriptorStorageImages       = MAX_DESCRIPTORS,
      .maxPerStageDescriptorInputAttachments    = MAX_DESCRIPTORS,
      .maxPerStageResources                     = MAX_DESCRIPTORS,
      .maxDescriptorSetSamplers                 = MAX_DESCRIPTORS,
      .maxDescriptorSetUniformBuffers           = MAX_DESCRIPTORS,
      .maxDescriptorSetUniformBuffersDynamic    = MAX_DESCRIPTORS / 2,
      .maxDescriptorSetStorageBuffers           = MAX_DESCRIPTORS,
      .maxDescriptorSetStorageBuffersDynamic    = MAX_DESCRIPTORS / 2,
      .maxDescriptorSetSampledImages            = MAX_DESCRIPTORS,
      .maxDescriptorSetStorageImages            = MAX_DESCRIPTORS,
      .maxDescriptorSetInputAttachments         = MAX_DESCRIPTORS,
      .maxVertexInputAttributes                 = 32,
      .maxVertexInputBindings                   = 32,
      .maxVertexInputAttributeOffset            = 2047,
      .maxVertexInputBindingStride              = 2048,
      .maxVertexOutputComponents                = 128,
      .maxTessellationGenerationLevel           = 64,
      .maxTessellationPatchSize                 = 32,
      .maxTessellationControlPerVertexInputComponents = 128,
      .maxTessellationControlPerVertexOutputComponents = 128,
      .maxTessellationControlPerPatchOutputComponents = 128,
      .maxTessellationControlTotalOutputComponents = 4096,
      .maxTessellationEvaluationInputComponents = 128,
      .maxTessellationEvaluationOutputComponents = 128,
      .maxGeometryShaderInvocations             = device->pscreen->caps.max_gs_invocations,
      .maxGeometryInputComponents               = 64,
      .maxGeometryOutputComponents              = 128,
      .maxGeometryOutputVertices                = device->pscreen->caps.max_geometry_output_vertices,
      .maxGeometryTotalOutputComponents         = device->pscreen->caps.max_geometry_total_output_components,
      .maxFragmentInputComponents               = 128,
      .maxFragmentOutputAttachments             = 8,
      .maxFragmentDualSrcAttachments            = 2,
      .maxFragmentCombinedOutputResources       = max_render_targets +
                                                  device->pscreen->shader_caps[MESA_SHADER_FRAGMENT].max_shader_buffers +
                                                  device->pscreen->shader_caps[MESA_SHADER_FRAGMENT].max_shader_images,
      .maxComputeSharedMemorySize               = device->pscreen->compute_caps.max_local_size,
      .maxComputeWorkGroupCount                 = { grid_size[0], grid_size[1], grid_size[2] },
      .maxComputeWorkGroupInvocations           = device->pscreen->compute_caps.max_threads_per_block,
      .maxComputeWorkGroupSize                  = { block_size[0], block_size[1], block_size[2] },
      .subPixelPrecisionBits                    = device->pscreen->caps.rasterizer_subpixel_bits,
      .subTexelPrecisionBits                    = 8,
      .mipmapPrecisionBits                      = 6,
      .maxDrawIndexedIndexValue                 = UINT32_MAX,
      .maxDrawIndirectCount                     = UINT32_MAX,
      .maxSamplerLodBias                        = 16,
      .maxSamplerAnisotropy                     = 16,
      .maxViewports                             = device->pscreen->caps.max_viewports,
      .maxViewportDimensions                    = { (1 << 14), (1 << 14) },
      .viewportBoundsRange                      = { -32768.0, 32768.0 },
      .viewportSubPixelBits                     = device->pscreen->caps.viewport_subpixel_bits,
      .minMemoryMapAlignment                    = device->pscreen->caps.min_map_buffer_alignment,
      .minTexelBufferOffsetAlignment            = device->pscreen->caps.texture_buffer_offset_alignment,
      .minUniformBufferOffsetAlignment          = device->pscreen->caps.constant_buffer_offset_alignment,
      .minStorageBufferOffsetAlignment          = device->pscreen->caps.shader_buffer_offset_alignment,
      .minTexelOffset                           = device->pscreen->caps.min_texel_offset,
      .maxTexelOffset                           = device->pscreen->caps.max_texel_offset,
      .minTexelGatherOffset                     = device->pscreen->caps.min_texture_gather_offset,
      .maxTexelGatherOffset                     = device->pscreen->caps.max_texture_gather_offset,
      .minInterpolationOffset                   = -2, /* FIXME */
      .maxInterpolationOffset                   = 2, /* FIXME */
      .subPixelInterpolationOffsetBits          = 8, /* FIXME */
      .maxFramebufferWidth                      = device->pscreen->caps.max_texture_2d_size,
      .maxFramebufferHeight                     = device->pscreen->caps.max_texture_2d_size,
      .maxFramebufferLayers                     = device->pscreen->caps.max_texture_array_layers,
      .framebufferColorSampleCounts             = LVP_SAMPLE_COUNTS,
      .framebufferDepthSampleCounts             = LVP_SAMPLE_COUNTS,
      .framebufferStencilSampleCounts           = LVP_SAMPLE_COUNTS,
      .framebufferNoAttachmentsSampleCounts     = LVP_SAMPLE_COUNTS,
      .maxColorAttachments                      = max_render_targets,
      .sampledImageColorSampleCounts            = LVP_SAMPLE_COUNTS,
      .sampledImageIntegerSampleCounts          = LVP_SAMPLE_COUNTS,
      .sampledImageDepthSampleCounts            = LVP_SAMPLE_COUNTS,
      .sampledImageStencilSampleCounts          = LVP_SAMPLE_COUNTS,
      .storageImageSampleCounts                 = LVP_SAMPLE_COUNTS,
      .maxSampleMaskWords                       = 1,
      .timestampComputeAndGraphics              = true,
      .timestampPeriod                          = 1,
      .maxClipDistances                         = 8,
      .maxCullDistances                         = 8,
      .maxCombinedClipAndCullDistances          = 8,
      .discreteQueuePriorities                  = 2,
      .pointSizeRange                           = { 0.0, device->pscreen->caps.max_point_size },
      .lineWidthRange                           = { 1.0, device->pscreen->caps.max_line_width },
      .pointSizeGranularity                     = (1.0 / 8.0),
      .lineWidthGranularity                     = 1.0 / 128.0,
      .strictLines                              = true,
      .standardSampleLocations                  = true,
      .optimalBufferCopyOffsetAlignment         = 128,
      .optimalBufferCopyRowPitchAlignment       = 128,
      .nonCoherentAtomSize                      = 64,
      .sparseResidencyStandard2DBlockShape      = true,
      .sparseResidencyStandard2DMultisampleBlockShape = true,
      .sparseResidencyStandard3DBlockShape      = true,

      /* Vulkan 1.1 */
      /* The LUID is for Windows. */
      .deviceLUIDValid = false,
      .deviceNodeMask = 0,

      .subgroupSize = lp_native_vector_width / 32,
      .subgroupSupportedStages = VK_SHADER_STAGE_FRAGMENT_BIT | VK_SHADER_STAGE_COMPUTE_BIT | VK_SHADER_STAGE_TASK_BIT_EXT | VK_SHADER_STAGE_MESH_BIT_EXT,
      .subgroupSupportedOperations = VK_SUBGROUP_FEATURE_BASIC_BIT | VK_SUBGROUP_FEATURE_VOTE_BIT | VK_SUBGROUP_FEATURE_ARITHMETIC_BIT | VK_SUBGROUP_FEATURE_BALLOT_BIT,
      .subgroupQuadOperationsInAllStages = true,

      .pointClippingBehavior = VK_POINT_CLIPPING_BEHAVIOR_ALL_CLIP_PLANES,
      .maxMultiviewViewCount = 6,
      .maxMultiviewInstanceIndex = INT_MAX,
      .protectedNoFault = false,
      .maxPerSetDescriptors = MAX_DESCRIPTORS,
      .maxMemoryAllocationSize = (1u << 31),

      /* Vulkan 1.2 */
      .driverID = VK_DRIVER_ID_MESA_LLVMPIPE,

      .conformanceVersion = (VkConformanceVersion){
         .major = 1,
         .minor = 3,
         .subminor = 1,
         .patch = 1,
      },

      .denormBehaviorIndependence = VK_SHADER_FLOAT_CONTROLS_INDEPENDENCE_ALL,
      .roundingModeIndependence = VK_SHADER_FLOAT_CONTROLS_INDEPENDENCE_ALL,
      .shaderDenormFlushToZeroFloat16 = false,
      .shaderDenormPreserveFloat16 = false,
      .shaderRoundingModeRTEFloat16 = true,
      .shaderRoundingModeRTZFloat16 = false,
      .shaderSignedZeroInfNanPreserveFloat16 = true,

      .shaderDenormFlushToZeroFloat32 = false,
      .shaderDenormPreserveFloat32 = false,
      .shaderRoundingModeRTEFloat32 = true,
      .shaderRoundingModeRTZFloat32 = false,
      .shaderSignedZeroInfNanPreserveFloat32 = true,

      .shaderDenormFlushToZeroFloat64 = false,
      .shaderDenormPreserveFloat64 = false,
      .shaderRoundingModeRTEFloat64 = true,
      .shaderRoundingModeRTZFloat64 = false,
      .shaderSignedZeroInfNanPreserveFloat64 = true,

      .maxUpdateAfterBindDescriptorsInAllPools = UINT32_MAX,
      .shaderUniformBufferArrayNonUniformIndexingNative = true,
      .shaderSampledImageArrayNonUniformIndexingNative = true,
      .shaderStorageBufferArrayNonUniformIndexingNative = true,
      .shaderStorageImageArrayNonUniformIndexingNative = true,
      .shaderInputAttachmentArrayNonUniformIndexingNative = true,
      .robustBufferAccessUpdateAfterBind = true,
      .quadDivergentImplicitLod = true,
      .maxPerStageDescriptorUpdateAfterBindSamplers = MAX_DESCRIPTORS,
      .maxPerStageDescriptorUpdateAfterBindUniformBuffers = MAX_DESCRIPTORS,
      .maxPerStageDescriptorUpdateAfterBindStorageBuffers = MAX_DESCRIPTORS,
      .maxPerStageDescriptorUpdateAfterBindSampledImages = MAX_DESCRIPTORS,
      .maxPerStageDescriptorUpdateAfterBindStorageImages = MAX_DESCRIPTORS,
      .maxPerStageDescriptorUpdateAfterBindInputAttachments = MAX_DESCRIPTORS,
      .maxPerStageUpdateAfterBindResources = MAX_DESCRIPTORS,
      .maxDescriptorSetUpdateAfterBindSamplers = MAX_DESCRIPTORS,
      .maxDescriptorSetUpdateAfterBindUniformBuffers = MAX_DESCRIPTORS,
      .maxDescriptorSetUpdateAfterBindUniformBuffersDynamic = MAX_DESCRIPTORS / 2,
      .maxDescriptorSetUpdateAfterBindStorageBuffers = MAX_DESCRIPTORS,
      .maxDescriptorSetUpdateAfterBindStorageBuffersDynamic = MAX_DESCRIPTORS / 2,
      .maxDescriptorSetUpdateAfterBindSampledImages = MAX_DESCRIPTORS,
      .maxDescriptorSetUpdateAfterBindStorageImages = MAX_DESCRIPTORS,
      .maxDescriptorSetUpdateAfterBindInputAttachments = MAX_DESCRIPTORS,

      .supportedDepthResolveModes = VK_RESOLVE_MODE_SAMPLE_ZERO_BIT,
      .supportedStencilResolveModes = VK_RESOLVE_MODE_SAMPLE_ZERO_BIT,
      .independentResolveNone = false,
      .independentResolve = false,

      .filterMinmaxImageComponentMapping = true,
      .filterMinmaxSingleComponentFormats = true,

      .maxTimelineSemaphoreValueDifference = UINT64_MAX,
      .framebufferIntegerColorSampleCounts = VK_SAMPLE_COUNT_1_BIT, /* LVP_SAMPLE_COUNTS? */ 

      /* Vulkan 1.3 */
      .minSubgroupSize = lp_native_vector_width / 32,
      .maxSubgroupSize = lp_native_vector_width / 32,
      .maxComputeWorkgroupSubgroups = 32,
      .requiredSubgroupSizeStages = VK_SHADER_STAGE_FRAGMENT_BIT | VK_SHADER_STAGE_COMPUTE_BIT,
      .maxInlineUniformTotalSize = MAX_DESCRIPTOR_UNIFORM_BLOCK_SIZE * MAX_PER_STAGE_DESCRIPTOR_UNIFORM_BLOCKS * MAX_SETS,
      .maxInlineUniformBlockSize = MAX_DESCRIPTOR_UNIFORM_BLOCK_SIZE,
      .maxPerStageDescriptorInlineUniformBlocks = MAX_PER_STAGE_DESCRIPTOR_UNIFORM_BLOCKS,
      .maxPerStageDescriptorUpdateAfterBindInlineUniformBlocks = MAX_PER_STAGE_DESCRIPTOR_UNIFORM_BLOCKS,
      .maxDescriptorSetInlineUniformBlocks = MAX_PER_STAGE_DESCRIPTOR_UNIFORM_BLOCKS,
      .maxDescriptorSetUpdateAfterBindInlineUniformBlocks = MAX_PER_STAGE_DESCRIPTOR_UNIFORM_BLOCKS,
      .storageTexelBufferOffsetAlignmentBytes = texel_buffer_alignment,
      .storageTexelBufferOffsetSingleTexelAlignment = true,
      .uniformTexelBufferOffsetAlignmentBytes = texel_buffer_alignment,
      .uniformTexelBufferOffsetSingleTexelAlignment = true,
      .maxBufferSize = UINT32_MAX,

      /* Vulkan 1.4 */
      .lineSubPixelPrecisionBits = device->pscreen->caps.rasterizer_subpixel_bits,
      .maxPushDescriptors = MAX_PUSH_DESCRIPTORS,
      /* FIXME No idea about most of these ones. */
      .earlyFragmentMultisampleCoverageAfterSampleCounting = true,
      .earlyFragmentSampleMaskTestBeforeSampleCounting = false,
      .depthStencilSwizzleOneSupport = false,
      .polygonModePointSize = true, /* This one is correct. */
      .nonStrictSinglePixelWideLinesUseParallelogram = false,
      .nonStrictWideLinesUseParallelogram = false,
      .blockTexelViewCompatibleMultipleLayers = true,
      .maxCombinedImageSamplerDescriptorCount = 3,
      .defaultRobustnessStorageBuffers = VK_PIPELINE_ROBUSTNESS_BUFFER_BEHAVIOR_ROBUST_BUFFER_ACCESS_2_EXT,
      .defaultRobustnessUniformBuffers = VK_PIPELINE_ROBUSTNESS_BUFFER_BEHAVIOR_ROBUST_BUFFER_ACCESS_2_EXT,
      .defaultRobustnessVertexInputs = VK_PIPELINE_ROBUSTNESS_BUFFER_BEHAVIOR_ROBUST_BUFFER_ACCESS_2_EXT,
      .defaultRobustnessImages = VK_PIPELINE_ROBUSTNESS_IMAGE_BEHAVIOR_ROBUST_IMAGE_ACCESS_2_EXT,
      .pCopySrcLayouts = lvp_host_copy_image_layouts,
      .copySrcLayoutCount = ARRAY_SIZE(lvp_host_copy_image_layouts),
      .pCopyDstLayouts = lvp_host_copy_image_layouts,
      .copyDstLayoutCount = ARRAY_SIZE(lvp_host_copy_image_layouts),
      .identicalMemoryTypeRequirements = VK_FALSE,

      /* VK_EXT_transform_feedback */
      .maxTransformFeedbackStreams = device->pscreen->caps.max_vertex_streams,
      .maxTransformFeedbackBuffers = device->pscreen->caps.max_stream_output_buffers,
      .maxTransformFeedbackBufferSize = UINT32_MAX,
      .maxTransformFeedbackStreamDataSize = 512,
      .maxTransformFeedbackBufferDataSize = 512,
      .maxTransformFeedbackBufferDataStride = 2048,
      .transformFeedbackQueries = true,
      .transformFeedbackStreamsLinesTriangles = false,
      .transformFeedbackRasterizationStreamSelect = false,
      .transformFeedbackDraw = true,

      /* VK_EXT_extended_dynamic_state3 */
      .dynamicPrimitiveTopologyUnrestricted = VK_TRUE,

      /* VK_EXT_device_generated_commands */
      .maxIndirectPipelineCount = 1<<12,
      .maxIndirectShaderObjectCount = 1<<12,
      .maxIndirectSequenceCount = 1<<20,
      .maxIndirectCommandsTokenCount = MAX_DGC_TOKENS,
      .maxIndirectCommandsTokenOffset = 2047,
      .maxIndirectCommandsIndirectStride = 2048,
      .supportedIndirectCommandsInputModes = VK_INDIRECT_COMMANDS_INPUT_MODE_VULKAN_INDEX_BUFFER_EXT | VK_INDIRECT_COMMANDS_INPUT_MODE_DXGI_INDEX_BUFFER_EXT,
      .supportedIndirectCommandsShaderStages = VK_SHADER_STAGE_ALL,
      .supportedIndirectCommandsShaderStagesPipelineBinding = VK_SHADER_STAGE_ALL,
      .supportedIndirectCommandsShaderStagesShaderBinding = VK_SHADER_STAGE_ALL,
      .deviceGeneratedCommandsTransformFeedback = true,
      .deviceGeneratedCommandsMultiDrawIndirectCount = true,

      /* VK_EXT_external_memory_host */
      .minImportedHostPointerAlignment = 4096,

      /* VK_EXT_custom_border_color */
      .maxCustomBorderColorSamplers = 32 * 1024,

      /* VK_EXT_provoking_vertex */
      .provokingVertexModePerPipeline = true,
      .transformFeedbackPreservesTriangleFanProvokingVertex = true,

      /* VK_EXT_multi_draw */
      .maxMultiDrawCount = 2048,

      /* VK_EXT_descriptor_buffer */
      .combinedImageSamplerDescriptorSingleArray = VK_TRUE,
      .bufferlessPushDescriptors = VK_TRUE,
      .descriptorBufferOffsetAlignment = 4,
      .maxDescriptorBufferBindings = MAX_SETS,
      .maxResourceDescriptorBufferBindings = MAX_SETS,
      .maxSamplerDescriptorBufferBindings = MAX_SETS,
      .maxEmbeddedImmutableSamplerBindings = MAX_SETS,
      .maxEmbeddedImmutableSamplers = 2032,
      .bufferCaptureReplayDescriptorDataSize = 0,
      .imageCaptureReplayDescriptorDataSize = 0,
      .imageViewCaptureReplayDescriptorDataSize = 0,
      .samplerCaptureReplayDescriptorDataSize = 0,
      .accelerationStructureCaptureReplayDescriptorDataSize = 0,
      .samplerDescriptorSize = sizeof(struct lp_descriptor),
      .combinedImageSamplerDescriptorSize = sizeof(struct lp_descriptor),
      .sampledImageDescriptorSize = sizeof(struct lp_descriptor),
      .storageImageDescriptorSize = sizeof(struct lp_descriptor),
      .uniformTexelBufferDescriptorSize = sizeof(struct lp_descriptor),
      .robustUniformTexelBufferDescriptorSize = sizeof(struct lp_descriptor),
      .storageTexelBufferDescriptorSize = sizeof(struct lp_descriptor),
      .robustStorageTexelBufferDescriptorSize = sizeof(struct lp_descriptor),
      .uniformBufferDescriptorSize = sizeof(struct lp_descriptor),
      .robustUniformBufferDescriptorSize = sizeof(struct lp_descriptor),
      .storageBufferDescriptorSize = sizeof(struct lp_descriptor),
      .robustStorageBufferDescriptorSize = sizeof(struct lp_descriptor),
      .inputAttachmentDescriptorSize = sizeof(struct lp_descriptor),
      .accelerationStructureDescriptorSize = sizeof(struct lp_descriptor),
      .maxSamplerDescriptorBufferRange = UINT32_MAX,
      .maxResourceDescriptorBufferRange = UINT32_MAX,
      .resourceDescriptorBufferAddressSpaceSize = UINT32_MAX,
      .samplerDescriptorBufferAddressSpaceSize = UINT32_MAX,
      .descriptorBufferAddressSpaceSize = UINT32_MAX,

      /* VK_EXT_graphics_pipeline_library */
      .graphicsPipelineLibraryFastLinking = VK_TRUE,
      .graphicsPipelineLibraryIndependentInterpolationDecoration = VK_TRUE,

      /* VK_EXT_robustness2 */
      .robustStorageBufferAccessSizeAlignment = 1,
      .robustUniformBufferAccessSizeAlignment = 1,

      /* VK_EXT_mesh_shader */
      .maxTaskWorkGroupTotalCount = 4194304,
      .maxTaskWorkGroupCount[0] = 65536,
      .maxTaskWorkGroupCount[1] = 65536,
      .maxTaskWorkGroupCount[2] = 65536,
      .maxTaskWorkGroupInvocations = 1024,
      .maxTaskWorkGroupSize[0] = 1024,
      .maxTaskWorkGroupSize[1] = 1024,
      .maxTaskWorkGroupSize[2] = 1024,
      .maxTaskPayloadSize = 16384,
      .maxTaskSharedMemorySize = 32768,
      .maxTaskPayloadAndSharedMemorySize = 32768,

      .maxMeshWorkGroupTotalCount = 4194304,
      .maxMeshWorkGroupCount[0] = 65536,
      .maxMeshWorkGroupCount[1] = 65536,
      .maxMeshWorkGroupCount[2] = 65536,
      .maxMeshWorkGroupInvocations = 1024,
      .maxMeshWorkGroupSize[0] = 1024,
      .maxMeshWorkGroupSize[1] = 1024,
      .maxMeshWorkGroupSize[2] = 1024,
      .maxMeshOutputMemorySize = 32768, /* 32K min required */
      .maxMeshSharedMemorySize = 28672,     /* 28K min required */
      .maxMeshOutputComponents = 128, /* 32x vec4 min required */
      .maxMeshOutputVertices = 256,
      .maxMeshOutputPrimitives = 256,
      .maxMeshOutputLayers = 8,
      .meshOutputPerVertexGranularity = 1,
      .meshOutputPerPrimitiveGranularity = 1,
      .maxPreferredTaskWorkGroupInvocations = 64,
      .maxPreferredMeshWorkGroupInvocations = 128,
      .prefersLocalInvocationVertexOutput = true,
      .prefersLocalInvocationPrimitiveOutput = true,
      .prefersCompactVertexOutput = true,
      .prefersCompactPrimitiveOutput = false,

      /* VK_EXT_sample_locations */
      .sampleLocationSampleCounts = ~VK_SAMPLE_COUNT_1_BIT & LVP_SAMPLE_COUNTS,
      .maxSampleLocationGridSize.width = 1,
      .maxSampleLocationGridSize.height = 1,
      .sampleLocationCoordinateRange[0] = 0.0f,
      .sampleLocationCoordinateRange[1] = 0.9375f,
      .sampleLocationSubPixelBits = 4,
      .variableSampleLocations = true,

      /* VK_AMDX_shader_enqueue */
#ifdef VK_ENABLE_BETA_EXTENSIONS
      .maxExecutionGraphDepth = 32,
      .maxExecutionGraphShaderOutputNodes = LVP_MAX_EXEC_GRAPH_PAYLOADS,
      .maxExecutionGraphShaderPayloadSize = 0xFFFF,
      .maxExecutionGraphShaderPayloadCount = LVP_MAX_EXEC_GRAPH_PAYLOADS,
      .executionGraphDispatchAddressAlignment = 4,
#endif

      /* VK_KHR_acceleration_structure */
      .maxGeometryCount = (1 << 24) - 1,
      .maxInstanceCount = (1 << 24) - 1,
      .maxPrimitiveCount = (1 << 24) - 1,
      .maxPerStageDescriptorAccelerationStructures = MAX_DESCRIPTORS,
      .maxPerStageDescriptorUpdateAfterBindAccelerationStructures = MAX_DESCRIPTORS,
      .maxDescriptorSetAccelerationStructures = MAX_DESCRIPTORS,
      .maxDescriptorSetUpdateAfterBindAccelerationStructures = MAX_DESCRIPTORS,
      .minAccelerationStructureScratchOffsetAlignment = 8,

      /* VK_EXT_legacy_vertex_attributes */
      .nativeUnalignedPerformance = true,

      /* VK_KHR_ray_tracing_pipeline */
      .shaderGroupHandleSize = LVP_RAY_TRACING_GROUP_HANDLE_SIZE,
      .maxRayRecursionDepth = 31,    /* Minimum allowed for DXR. */
      .maxShaderGroupStride = 16384, /* dummy */
      /* This isn't strictly necessary, but Doom Eternal breaks if the
       * alignment is any lower. */
      .shaderGroupBaseAlignment = 32,
      .shaderGroupHandleCaptureReplaySize = 0,
      .maxRayDispatchInvocationCount = 1024 * 1024 * 64,
      .shaderGroupHandleAlignment = 16,
      .maxRayHitAttributeSize = LVP_RAY_HIT_ATTRIBS_SIZE,

      /* VK_KHR_compute_shader_derivatives */
      .meshAndTaskShaderDerivatives = true,
   };

   /* Vulkan 1.0 */
   strcpy(p->deviceName, device->pscreen->get_name(device->pscreen));
   lvp_device_get_cache_uuid(p->pipelineCacheUUID);

   /* Vulkan 1.1 */
   device->pscreen->get_device_uuid(device->pscreen, (char*)(p->deviceUUID));
   device->pscreen->get_driver_uuid(device->pscreen, (char*)(p->driverUUID));
   memset(p->deviceLUID, 0, VK_LUID_SIZE);

#if LLVM_VERSION_MAJOR >= 10
   p->subgroupSupportedOperations |= VK_SUBGROUP_FEATURE_SHUFFLE_BIT | VK_SUBGROUP_FEATURE_SHUFFLE_RELATIVE_BIT | VK_SUBGROUP_FEATURE_QUAD_BIT |
      VK_SUBGROUP_FEATURE_CLUSTERED_BIT | VK_SUBGROUP_FEATURE_ROTATE_BIT_KHR | VK_SUBGROUP_FEATURE_ROTATE_CLUSTERED_BIT_KHR;
#endif

   /* Vulkan 1.2 */
   snprintf(p->driverName, VK_MAX_DRIVER_NAME_SIZE, "llvmpipe");
   snprintf(p->driverInfo, VK_MAX_DRIVER_INFO_SIZE, "Mesa " PACKAGE_VERSION MESA_GIT_SHA1
#ifdef MESA_LLVM_VERSION_STRING
            " (LLVM " MESA_LLVM_VERSION_STRING ")"
#endif
           );

   /* Vulkan 1.4 */
   if (device->pscreen->caps.vertex_element_instance_divisor)
      p->maxVertexAttribDivisor = UINT32_MAX;
   else
      p->maxVertexAttribDivisor = 1;

   /* VK_EXT_nested_command_buffer */
   p->maxCommandBufferNestingLevel = UINT32_MAX;

   /* VK_EXT_host_image_copy */
   lvp_device_get_cache_uuid(p->optimalTilingLayoutUUID);

   /* VK_KHR_copy_memory_indirect */
   p->supportedQueues = 0xffffffff;

   /* maintenance7 */
   p->robustFragmentShadingRateAttachmentAccess = false;
   p->separateDepthStencilAttachmentAccess = true;
   p->maxDescriptorSetTotalUniformBuffersDynamic = MAX_DESCRIPTORS;
   p->maxDescriptorSetTotalStorageBuffersDynamic = MAX_DESCRIPTORS;
   p->maxDescriptorSetTotalBuffersDynamic = MAX_DESCRIPTORS;
   p->maxDescriptorSetUpdateAfterBindTotalUniformBuffersDynamic = MAX_DESCRIPTORS / 2;
   p->maxDescriptorSetUpdateAfterBindTotalStorageBuffersDynamic = MAX_DESCRIPTORS / 2;
   p->maxDescriptorSetUpdateAfterBindTotalBuffersDynamic = MAX_DESCRIPTORS;

   /* maintenance9 */
   p->image2DViewOf3DSparse = true;
   p->defaultVertexAttributeValue = VK_DEFAULT_VERTEX_ATTRIBUTE_VALUE_ZERO_ZERO_ZERO_ZERO_KHR;

   /* maintenance10 */
   p->rgba4OpaqueBlackSwizzled = true;
   p->resolveSrgbFormatAppliesTransferFunction = true;

   /* VK_EXT_shader_object */
   /* this is basically unsupported */
   lvp_device_get_cache_uuid(p->shaderBinaryUUID);
   p->shaderBinaryVersion = 1;

   /* VK_EXT_mesh_shader */
   p->maxMeshPayloadAndSharedMemorySize = p->maxTaskPayloadSize + p->maxMeshSharedMemorySize; /* 28K min required */
   p->maxMeshPayloadAndOutputMemorySize = p->maxTaskPayloadSize + p->maxMeshOutputMemorySize; /* 47K min required */
}

static VkResult VKAPI_CALL
lvp_physical_device_init(struct lvp_physical_device *device,
                         struct lvp_instance *instance,
                         struct pipe_loader_device *pld)
{
   VkResult result;

   struct vk_physical_device_dispatch_table dispatch_table;
   vk_physical_device_dispatch_table_from_entrypoints(
      &dispatch_table, &lvp_physical_device_entrypoints, true);
   vk_physical_device_dispatch_table_from_entrypoints(
      &dispatch_table, &wsi_physical_device_entrypoints, false);
   result = vk_physical_device_init(&device->vk, &instance->vk,
                                    NULL, NULL, NULL, &dispatch_table);
   if (result != VK_SUCCESS) {
      vk_error(instance, result);
      goto fail;
   }
   device->pld = pld;

   device->pscreen = pipe_loader_create_screen_vk(device->pld, true, false);
   if (!device->pscreen)
      return vk_error(instance, VK_ERROR_OUT_OF_HOST_MEMORY);
   for (unsigned i = 0; i < ARRAY_SIZE(device->drv_options); i++)
      device->drv_options[i] = device->pscreen->nir_options[MIN2(i, MESA_SHADER_COMPUTE)];

   device->sync_timeline_type = vk_sync_timeline_get_type(&lvp_pipe_sync_type);
   device->sync_types[0] = &lvp_pipe_sync_type;
   device->sync_types[1] = &device->sync_timeline_type.sync;
   device->sync_types[2] = NULL;
   device->vk.supported_sync_types = device->sync_types;

   device->max_images = device->pscreen->shader_caps[MESA_SHADER_FRAGMENT].max_shader_images;
   device->vk.supported_extensions = lvp_device_extensions_supported;
#ifdef HAVE_LIBDRM
   int dmabuf_bits = DRM_PRIME_CAP_EXPORT | DRM_PRIME_CAP_IMPORT;
   int supported_dmabuf_bits = device->pscreen->caps.dmabuf;
   /* if import or export is supported then EXT_external_memory_dma_buf is supported */
   if (supported_dmabuf_bits)
      device->vk.supported_extensions.EXT_external_memory_dma_buf = true;
   if ((supported_dmabuf_bits & dmabuf_bits) == dmabuf_bits)
      device->vk.supported_extensions.EXT_image_drm_format_modifier = true;
   if (device->pscreen->caps.native_fence_fd) {
      device->vk.supported_extensions.KHR_external_semaphore_fd = true;
      device->vk.supported_extensions.KHR_external_fence_fd = true;
   }
   if (supported_dmabuf_bits & DRM_PRIME_CAP_IMPORT)
      device->vk.supported_extensions.ANDROID_external_memory_android_hardware_buffer = true;
#endif

   /* SNORM blending on llvmpipe fails CTS - disable by default */
   device->snorm_blend = debug_get_bool_option("LVP_SNORM_BLEND", false);

   lvp_get_features(device, &device->vk.supported_features);
   lvp_get_properties(device, &device->vk.properties);

#ifdef LVP_USE_WSI_PLATFORM
   result = lvp_init_wsi(device);
   if (result != VK_SUCCESS) {
      vk_physical_device_finish(&device->vk);
      vk_error(instance, result);
      goto fail;
   }
#endif

   return VK_SUCCESS;
 fail:
   return result;
}

static void VKAPI_CALL
lvp_physical_device_finish(struct lvp_physical_device *device)
{
#ifdef LVP_USE_WSI_PLATFORM
   lvp_finish_wsi(device);
#endif
   device->pscreen->destroy(device->pscreen);
   vk_physical_device_finish(&device->vk);
}

static void
lvp_destroy_physical_device(struct vk_physical_device *device)
{
   lvp_physical_device_finish((struct lvp_physical_device *)device);
   vk_free(&device->instance->alloc, device);
}

static VkResult
lvp_enumerate_physical_devices(struct vk_instance *vk_instance);

VKAPI_ATTR VkResult VKAPI_CALL lvp_CreateInstance(
   const VkInstanceCreateInfo*                 pCreateInfo,
   const VkAllocationCallbacks*                pAllocator,
   VkInstance*                                 pInstance)
{
   struct lvp_instance *instance;
   VkResult result;

   assert(pCreateInfo->sType == VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO);

   if (pAllocator == NULL)
      pAllocator = vk_default_allocator();

   instance = vk_zalloc(pAllocator, sizeof(*instance), 8,
                        VK_SYSTEM_ALLOCATION_SCOPE_INSTANCE);
   if (!instance)
      return vk_error(NULL, VK_ERROR_OUT_OF_HOST_MEMORY);

   struct vk_instance_dispatch_table dispatch_table;
   vk_instance_dispatch_table_from_entrypoints(
      &dispatch_table, &lvp_instance_entrypoints, true);
   vk_instance_dispatch_table_from_entrypoints(
      &dispatch_table, &wsi_instance_entrypoints, false);

   result = vk_instance_init(&instance->vk,
                             &lvp_instance_extensions_supported,
                             &dispatch_table,
                             pCreateInfo,
                             pAllocator);
   if (result != VK_SUCCESS) {
      vk_free(pAllocator, instance);
      return vk_error(NULL, result);
   }

   instance->apiVersion = LVP_API_VERSION;

   instance->vk.physical_devices.enumerate = lvp_enumerate_physical_devices;
   instance->vk.physical_devices.destroy = lvp_destroy_physical_device;

   //   VG(VALGRIND_CREATE_MEMPOOL(instance, 0, false));

   *pInstance = lvp_instance_to_handle(instance);

   return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL lvp_DestroyInstance(
   VkInstance                                  _instance,
   const VkAllocationCallbacks*                pAllocator)
{
   VK_FROM_HANDLE(lvp_instance, instance, _instance);

   if (!instance)
      return;

   pipe_loader_release(&instance->devs, instance->num_devices);

   vk_instance_finish(&instance->vk);
   vk_free(&instance->vk.alloc, instance);
}

#if defined(HAVE_DRI)
static void lvp_get_image(struct dri_drawable *dri_drawable,
                          int x, int y, unsigned width, unsigned height, unsigned stride,
                          void *data)
{

}

static void lvp_put_image(struct dri_drawable *dri_drawable,
                          void *data, unsigned width, unsigned height)
{
   fprintf(stderr, "put image %dx%d\n", width, height);
}

static void lvp_put_image2(struct dri_drawable *dri_drawable,
                           void *data, int x, int y, unsigned width, unsigned height,
                           unsigned stride)
{
   fprintf(stderr, "put image 2 %d,%d %dx%d\n", x, y, width, height);
}

static struct drisw_loader_funcs lvp_sw_lf = {
   .get_image = lvp_get_image,
   .put_image = lvp_put_image,
   .put_image2 = lvp_put_image2,
};
#endif

static VkResult
lvp_enumerate_physical_devices(struct vk_instance *vk_instance)
{
   if (!draw_get_option_use_llvm())
      return VK_SUCCESS;

   struct lvp_instance *instance =
      container_of(vk_instance, struct lvp_instance, vk);

   /* sw only for now */
   instance->num_devices = pipe_loader_sw_probe(NULL, 0);

   assert(instance->num_devices == 1);

#if defined(HAVE_DRI)
   pipe_loader_sw_probe_dri(&instance->devs, &lvp_sw_lf);
#else
   pipe_loader_sw_probe_null(&instance->devs);
#endif

   struct lvp_physical_device *device =
      vk_zalloc2(&instance->vk.alloc, NULL, sizeof(*device), 8,
                 VK_SYSTEM_ALLOCATION_SCOPE_INSTANCE);
   if (!device)
      return vk_error(instance, VK_ERROR_OUT_OF_HOST_MEMORY);

   VkResult result = lvp_physical_device_init(device, instance, &instance->devs[0]);
   if (result == VK_SUCCESS)
      list_addtail(&device->vk.link, &instance->vk.physical_devices.list);
   else
      vk_free(&vk_instance->alloc, device);

   return result;
}

void
lvp_device_get_cache_uuid(void *uuid)
{
   memset(uuid, 'a', VK_UUID_SIZE);
   if (MESA_GIT_SHA1[0])
      /* debug build */
      memcpy(uuid, &MESA_GIT_SHA1[4], MIN2(strlen(MESA_GIT_SHA1) - 4, VK_UUID_SIZE));
   else
      /* release build */
      memcpy(uuid, PACKAGE_VERSION, MIN2(strlen(PACKAGE_VERSION), VK_UUID_SIZE));
}

VKAPI_ATTR void VKAPI_CALL lvp_GetPhysicalDeviceQueueFamilyProperties2(
   VkPhysicalDevice                            physicalDevice,
   uint32_t*                                   pCount,
   VkQueueFamilyProperties2                   *pQueueFamilyProperties)
{
   VK_OUTARRAY_MAKE_TYPED(VkQueueFamilyProperties2, out, pQueueFamilyProperties, pCount);

   VkQueueFamilyGlobalPriorityPropertiesKHR *prio = vk_find_struct(pQueueFamilyProperties, QUEUE_FAMILY_GLOBAL_PRIORITY_PROPERTIES_KHR);
   if (prio) {
      prio->priorityCount = 4;
      prio->priorities[0] = VK_QUEUE_GLOBAL_PRIORITY_LOW_KHR;
      prio->priorities[1] = VK_QUEUE_GLOBAL_PRIORITY_MEDIUM_KHR;
      prio->priorities[2] = VK_QUEUE_GLOBAL_PRIORITY_HIGH_KHR;
      prio->priorities[3] = VK_QUEUE_GLOBAL_PRIORITY_REALTIME_KHR;
   }
   VkQueueFamilyOwnershipTransferPropertiesKHR *prop = vk_find_struct(pQueueFamilyProperties, QUEUE_FAMILY_OWNERSHIP_TRANSFER_PROPERTIES_KHR);
   if (prop)
      prop->optimalImageTransferToQueueFamilies = ~0;

   vk_outarray_append_typed(VkQueueFamilyProperties2, &out, p) {
      p->queueFamilyProperties = (VkQueueFamilyProperties) {
         .queueFlags = VK_QUEUE_GRAPHICS_BIT |
         VK_QUEUE_COMPUTE_BIT |
         VK_QUEUE_TRANSFER_BIT |
         (DETECT_OS_LINUX ? VK_QUEUE_SPARSE_BINDING_BIT : 0),
         .queueCount = 1,
         .timestampValidBits = 64,
         .minImageTransferGranularity = (VkExtent3D) { 1, 1, 1 },
      };
   }
}

VKAPI_ATTR void VKAPI_CALL lvp_GetPhysicalDeviceMemoryProperties(
   VkPhysicalDevice                            physicalDevice,
   VkPhysicalDeviceMemoryProperties*           pMemoryProperties)
{
   pMemoryProperties->memoryTypeCount = 1;
   pMemoryProperties->memoryTypes[0] = (VkMemoryType) {
      .propertyFlags = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT |
      VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
      VK_MEMORY_PROPERTY_HOST_COHERENT_BIT |
      VK_MEMORY_PROPERTY_HOST_CACHED_BIT,
      .heapIndex = 0,
   };

   VkDeviceSize low_size = 3ULL*1024*1024*1024;
   VkDeviceSize total_size;
   os_get_total_physical_memory(&total_size);
   pMemoryProperties->memoryHeapCount = 1;
   pMemoryProperties->memoryHeaps[0] = (VkMemoryHeap) {
      .size = low_size,
      .flags = VK_MEMORY_HEAP_DEVICE_LOCAL_BIT,
   };
   if (sizeof(void*) > sizeof(uint32_t))
      pMemoryProperties->memoryHeaps[0].size = total_size;
}

VKAPI_ATTR void VKAPI_CALL lvp_GetPhysicalDeviceMemoryProperties2(
   VkPhysicalDevice                            physicalDevice,
   VkPhysicalDeviceMemoryProperties2          *pMemoryProperties)
{
   lvp_GetPhysicalDeviceMemoryProperties(physicalDevice,
                                         &pMemoryProperties->memoryProperties);
   VkPhysicalDeviceMemoryBudgetPropertiesEXT *props = vk_find_struct(pMemoryProperties, PHYSICAL_DEVICE_MEMORY_BUDGET_PROPERTIES_EXT);
   if (props) {
      props->heapBudget[0] = pMemoryProperties->memoryProperties.memoryHeaps[0].size;
      if (os_get_available_system_memory(&props->heapUsage[0])) {
         props->heapUsage[0] = props->heapBudget[0] - props->heapUsage[0];
      } else {
         props->heapUsage[0] = 0;
      }
      memset(&props->heapBudget[1], 0, sizeof(props->heapBudget[0]) * (VK_MAX_MEMORY_HEAPS - 1));
      memset(&props->heapUsage[1], 0, sizeof(props->heapUsage[0]) * (VK_MAX_MEMORY_HEAPS - 1));
   }
}

VKAPI_ATTR VkResult VKAPI_CALL
lvp_GetMemoryHostPointerPropertiesEXT(
   VkDevice _device,
   VkExternalMemoryHandleTypeFlagBits handleType,
   const void *pHostPointer,
   VkMemoryHostPointerPropertiesEXT *pMemoryHostPointerProperties)
{
   switch (handleType) {
   case VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT: {
      pMemoryHostPointerProperties->memoryTypeBits = 1;
      return VK_SUCCESS;
   }
   default:
      return VK_ERROR_INVALID_EXTERNAL_HANDLE;
   }
}

VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL lvp_GetInstanceProcAddr(
   VkInstance                                  _instance,
   const char*                                 pName)
{
   VK_FROM_HANDLE(vk_instance, instance, _instance);
   return vk_instance_get_proc_addr(instance,
                                    &lvp_instance_entrypoints,
                                    pName);
}

/* The loader wants us to expose a second GetInstanceProcAddr function
 * to work around certain LD_PRELOAD issues seen in apps.
 */
PUBLIC
VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL vk_icdGetInstanceProcAddr(
   VkInstance                                  instance,
   const char*                                 pName)
{
   return lvp_GetInstanceProcAddr(instance, pName);
}

static void
destroy_pipelines(struct lvp_queue *queue)
{
   struct lvp_device *device = lvp_queue_device(queue);
   simple_mtx_lock(&queue->lock);
   while (util_dynarray_contains(&queue->pipeline_destroys, struct lvp_pipeline*)) {
      lvp_pipeline_destroy(device, util_dynarray_pop(&queue->pipeline_destroys, struct lvp_pipeline*), true);
   }
   simple_mtx_unlock(&queue->lock);
}

static VkResult
lvp_queue_submit(struct vk_queue *vk_queue,
                 struct vk_queue_submit *submit)
{
   struct lvp_queue *queue = container_of(vk_queue, struct lvp_queue, vk);
   struct lvp_device *device = lvp_queue_device(queue);

   VkResult result = vk_sync_wait_many(&device->vk,
                                       submit->wait_count, submit->waits,
                                       VK_SYNC_WAIT_COMPLETE, UINT64_MAX);
   if (result != VK_SUCCESS)
      return result;

   simple_mtx_lock(&queue->lock);

   for (uint32_t i = 0; i < submit->buffer_bind_count; i++) {
      VkSparseBufferMemoryBindInfo *bind = &submit->buffer_binds[i];

      lvp_buffer_bind_sparse(device, queue, bind);
   }

   for (uint32_t i = 0; i < submit->image_opaque_bind_count; i++) {
      VkSparseImageOpaqueMemoryBindInfo *bind = &submit->image_opaque_binds[i];

      lvp_image_bind_opaque_sparse(device, queue, bind);
   }

   for (uint32_t i = 0; i < submit->image_bind_count; i++) {
      VkSparseImageMemoryBindInfo *bind = &submit->image_binds[i];

      lvp_image_bind_sparse(device, queue, bind);
   }

   for (uint32_t i = 0; i < submit->command_buffer_count; i++) {
      struct lvp_cmd_buffer *cmd_buffer =
         container_of(submit->command_buffers[i], struct lvp_cmd_buffer, vk);

      lvp_execute_cmds(device, queue, cmd_buffer);
   }

   simple_mtx_unlock(&queue->lock);

   if (submit->command_buffer_count > 0)
      queue->ctx->flush(queue->ctx, &queue->last_fence, 0);

   for (uint32_t i = 0; i < submit->signal_count; i++) {
      struct lvp_pipe_sync *sync =
         vk_sync_as_lvp_pipe_sync(submit->signals[i].sync);
      lvp_pipe_sync_signal_with_fence(device, sync, queue->last_fence);
   }
   destroy_pipelines(queue);

   return VK_SUCCESS;
}

static VkResult
lvp_queue_init(struct lvp_device *device, struct lvp_queue *queue,
               const VkDeviceQueueCreateInfo *create_info,
               uint32_t index_in_family)
{
   VkResult result = vk_queue_init(&queue->vk, &device->vk, create_info,
                                   index_in_family);
   if (result != VK_SUCCESS)
      return result;

   result = vk_queue_enable_submit_thread(&queue->vk);
   if (result != VK_SUCCESS) {
      vk_queue_finish(&queue->vk);
      return result;
   }

   queue->ctx = device->pscreen->context_create(device->pscreen, NULL, PIPE_CONTEXT_ROBUST_BUFFER_ACCESS);
   queue->cso = cso_create_context(queue->ctx, CSO_NO_VBUF);
   queue->uploader = u_upload_create(queue->ctx, 1024 * 1024, PIPE_BIND_CONSTANT_BUFFER, PIPE_USAGE_STREAM, 0);

   queue->vk.driver_submit = lvp_queue_submit;

   simple_mtx_init(&queue->lock, mtx_plain);
   util_dynarray_init(&queue->pipeline_destroys, NULL);

   return VK_SUCCESS;
}

static void
lvp_queue_finish(struct lvp_queue *queue)
{
   vk_queue_finish(&queue->vk);

   destroy_pipelines(queue);
   simple_mtx_destroy(&queue->lock);
   util_dynarray_fini(&queue->pipeline_destroys);

   u_upload_destroy(queue->uploader);
   cso_destroy_context(queue->cso);
   queue->ctx->destroy(queue->ctx);
}

VKAPI_ATTR VkResult VKAPI_CALL lvp_CreateDevice(
   VkPhysicalDevice                            physicalDevice,
   const VkDeviceCreateInfo*                   pCreateInfo,
   const VkAllocationCallbacks*                pAllocator,
   VkDevice*                                   pDevice)
{
   VK_FROM_HANDLE(lvp_physical_device, physical_device, physicalDevice);
   struct lvp_device *device;
   struct lvp_instance *instance = (struct lvp_instance *)physical_device->vk.instance;

   assert(pCreateInfo->sType == VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO);

   size_t state_size = lvp_get_rendering_state_size();
   device = vk_zalloc2(&physical_device->vk.instance->alloc, pAllocator,
                       sizeof(*device) + state_size, 8,
                       VK_SYSTEM_ALLOCATION_SCOPE_DEVICE);
   if (!device)
      return vk_error(instance, VK_ERROR_OUT_OF_HOST_MEMORY);

   device->queue.state = device + 1;
   device->poison_mem = debug_get_bool_option("LVP_POISON_MEMORY", false);
   device->print_cmds = debug_get_bool_option("LVP_CMD_DEBUG", false);

   struct vk_device_dispatch_table dispatch_table;
   vk_device_dispatch_table_from_entrypoints(&dispatch_table,
      &lvp_device_entrypoints, true);
   lvp_add_enqueue_cmd_entrypoints(&dispatch_table);
   vk_device_dispatch_table_from_entrypoints(&dispatch_table,
      &wsi_device_entrypoints, false);
   VkResult result = vk_device_init(&device->vk,
                                    &physical_device->vk,
                                    &dispatch_table, pCreateInfo,
                                    pAllocator);
   if (result != VK_SUCCESS) {
      vk_free(&device->vk.alloc, device);
      return result;
   }

   vk_device_enable_threaded_submit(&device->vk);
   device->vk.command_buffer_ops = &lvp_cmd_buffer_ops;

   device->pscreen = physical_device->pscreen;

   assert(pCreateInfo->queueCreateInfoCount <= LVP_NUM_QUEUES);
   if (pCreateInfo->queueCreateInfoCount) {
      assert(pCreateInfo->pQueueCreateInfos[0].queueFamilyIndex == 0);
      assert(pCreateInfo->pQueueCreateInfos[0].queueCount == 1);
   }
   result = lvp_queue_init(device, &device->queue, pCreateInfo->pQueueCreateInfos, 0);
   if (result != VK_SUCCESS) {
      vk_free(&device->vk.alloc, device);
      return result;
   }

   nir_builder b = nir_builder_init_simple_shader(MESA_SHADER_FRAGMENT, physical_device->drv_options[MESA_SHADER_FRAGMENT], "dummy_frag");
   struct pipe_shader_state shstate = {0};
   shstate.type = PIPE_SHADER_IR_NIR;
   shstate.ir.nir = b.shader;
   device->noop_fs = device->queue.ctx->create_fs_state(device->queue.ctx, &shstate);
   _mesa_hash_table_init(&device->bda, NULL, _mesa_hash_pointer, _mesa_key_pointer_equal);
   simple_mtx_init(&device->bda_lock, mtx_plain);

   uint32_t zero = 0;
   device->zero_buffer = pipe_buffer_create_with_data(device->queue.ctx, 0, PIPE_USAGE_IMMUTABLE, sizeof(uint32_t), &zero);

   device->null_texture_handle = (void *)(uintptr_t)device->queue.ctx->create_texture_handle(device->queue.ctx,
      &(struct pipe_sampler_view){ 0 }, NULL);
   device->null_image_handle = (void *)(uintptr_t)device->queue.ctx->create_image_handle(device->queue.ctx,
      &(struct pipe_image_view){ 0 });

   util_dynarray_init(&device->bda_texture_handles, NULL);
   util_dynarray_init(&device->bda_image_handles, NULL);

   device->group_handle_alloc = 1;

   result = vk_meta_device_init(&device->vk, &device->meta);
   if (result != VK_SUCCESS) {
      lvp_DestroyDevice(lvp_device_to_handle(device), pAllocator);
      return result;
   }

   lvp_device_init_accel_struct_state(device);

   *pDevice = lvp_device_to_handle(device);

   return VK_SUCCESS;

}

VKAPI_ATTR void VKAPI_CALL lvp_DestroyDevice(
   VkDevice                                    _device,
   const VkAllocationCallbacks*                pAllocator)
{
   VK_FROM_HANDLE(lvp_device, device, _device);

   lvp_device_finish_accel_struct_state(device);

   vk_meta_device_finish(&device->vk, &device->meta);

   util_dynarray_foreach(&device->bda_texture_handles, struct lp_texture_handle *, handle)
      device->queue.ctx->delete_texture_handle(device->queue.ctx, (uint64_t)(uintptr_t)*handle);

   util_dynarray_fini(&device->bda_texture_handles);

   util_dynarray_foreach(&device->bda_image_handles, struct lp_texture_handle *, handle)
      device->queue.ctx->delete_image_handle(device->queue.ctx, (uint64_t)(uintptr_t)*handle);

   util_dynarray_fini(&device->bda_image_handles);

   device->queue.ctx->delete_texture_handle(device->queue.ctx, (uint64_t)(uintptr_t)device->null_texture_handle);
   device->queue.ctx->delete_image_handle(device->queue.ctx, (uint64_t)(uintptr_t)device->null_image_handle);

   device->queue.ctx->delete_fs_state(device->queue.ctx, device->noop_fs);

   if (device->queue.last_fence)
      device->pscreen->fence_reference(device->pscreen, &device->queue.last_fence, NULL);
   _mesa_hash_table_fini(&device->bda, NULL);
   simple_mtx_destroy(&device->bda_lock);
   pipe_resource_reference(&device->zero_buffer, NULL);

   lvp_queue_finish(&device->queue);
   vk_device_finish(&device->vk);
   vk_free(&device->vk.alloc, device);
}

VKAPI_ATTR VkResult VKAPI_CALL lvp_EnumerateInstanceExtensionProperties(
   const char*                                 pLayerName,
   uint32_t*                                   pPropertyCount,
   VkExtensionProperties*                      pProperties)
{
   if (pLayerName)
      return vk_error(NULL, VK_ERROR_LAYER_NOT_PRESENT);

   return vk_enumerate_instance_extension_properties(
      &lvp_instance_extensions_supported, pPropertyCount, pProperties);
}

VKAPI_ATTR VkResult VKAPI_CALL lvp_EnumerateInstanceLayerProperties(
   uint32_t*                                   pPropertyCount,
   VkLayerProperties*                          pProperties)
{
   if (pProperties == NULL) {
      *pPropertyCount = 0;
      return VK_SUCCESS;
   }

   /* None supported at this time */
   return vk_error(NULL, VK_ERROR_LAYER_NOT_PRESENT);
}

VKAPI_ATTR VkResult VKAPI_CALL lvp_EnumerateDeviceLayerProperties(
   VkPhysicalDevice                            physicalDevice,
   uint32_t*                                   pPropertyCount,
   VkLayerProperties*                          pProperties)
{
   if (pProperties == NULL) {
      *pPropertyCount = 0;
      return VK_SUCCESS;
   }

   /* None supported at this time */
   return vk_error(NULL, VK_ERROR_LAYER_NOT_PRESENT);
}

static void
set_mem_priority(struct lvp_device_memory *mem, int priority)
{
#if DETECT_OS_LINUX
   if (priority) {
      int advice = 0;
#ifdef MADV_COLD
      if (priority < 0)
         advice |= MADV_COLD;
#endif
      if (priority > 0)
         advice |= MADV_WILLNEED;
      if (advice)
         madvise(mem->map, mem->vk.size, advice);
   }
#endif
}

static int
get_mem_priority(float priority)
{
   if (priority < 0.3)
      return -1;
   if (priority < 0.6)
      return 0;
   return priority = 1;
}

VKAPI_ATTR VkResult VKAPI_CALL lvp_AllocateMemory(
   VkDevice                                    _device,
   const VkMemoryAllocateInfo*                 pAllocateInfo,
   const VkAllocationCallbacks*                pAllocator,
   VkDeviceMemory*                             pMem)
{
   VK_FROM_HANDLE(lvp_device, device, _device);
   struct lvp_device_memory *mem;
   ASSERTED const VkImportMemoryFdInfoKHR *import_info = NULL;
   const VkMemoryAllocateFlagsInfo *mem_flags = NULL;
   VkResult error = VK_ERROR_OUT_OF_DEVICE_MEMORY;
   assert(pAllocateInfo->sType == VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO);
   int priority = 0;

   vk_foreach_struct_const(ext, pAllocateInfo->pNext) {
      switch ((unsigned)ext->sType) {
      case VK_STRUCTURE_TYPE_IMPORT_MEMORY_FD_INFO_KHR:
         import_info = (VkImportMemoryFdInfoKHR*)ext;
         assert_memhandle_type(import_info->handleType);
         break;
      case VK_STRUCTURE_TYPE_MEMORY_PRIORITY_ALLOCATE_INFO_EXT: {
         VkMemoryPriorityAllocateInfoEXT *prio = (VkMemoryPriorityAllocateInfoEXT*)ext;
         priority = get_mem_priority(prio->priority);
         break;
      }
      case VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO:
         mem_flags = (void*)ext;
         break;
      default:
         break;
      }
   }

#ifdef PIPE_MEMORY_FD
   if (import_info != NULL && import_info->fd < 0) {
      const struct lvp_physical_device *pdev = lvp_device_physical(device);
      return vk_error(pdev->vk.instance, VK_ERROR_INVALID_EXTERNAL_HANDLE);
   }
#endif

   mem = vk_device_memory_create(&device->vk, pAllocateInfo, pAllocator,
                                 sizeof(*mem));
   if (mem == NULL)
      return vk_error(device, VK_ERROR_OUT_OF_HOST_MEMORY);

   mem->memory_type = LVP_DEVICE_MEMORY_TYPE_DEFAULT;
   mem->backed_fd = -1;

   if (mem->vk.host_ptr) {
      mem->mem_alloc = (struct llvmpipe_memory_allocation) {
         .cpu_addr = mem->vk.host_ptr,
      };
      mem->pmem = (void *)&mem->mem_alloc;
      mem->map = mem->vk.host_ptr;
      mem->memory_type = LVP_DEVICE_MEMORY_TYPE_USER_PTR;
   }
#if DETECT_OS_ANDROID
   else if (mem->vk.ahardware_buffer) {
      error = lvp_import_ahb_memory(device, mem);
      if (error != VK_SUCCESS)
         goto fail;
   }
#endif
#ifdef PIPE_MEMORY_FD
   else if (mem->vk.import_handle_type) {
      assert(import_info &&
             import_info->handleType == mem->vk.import_handle_type);
      const enum lvp_device_memory_type memory_type =
         lvp_device_memory_type_for_handle_types(lvp_device_physical(device), mem->vk.import_handle_type);
      const bool dmabuf = memory_type == LVP_DEVICE_MEMORY_TYPE_DMA_BUF;
      uint64_t size;
      if(!device->pscreen->import_memory_fd(device->pscreen, import_info->fd, &mem->pmem, &size, dmabuf)) {
         error = VK_ERROR_INVALID_EXTERNAL_HANDLE;
         goto fail;
      }
      if(size < pAllocateInfo->allocationSize) {
         device->pscreen->free_memory_fd(device->pscreen, mem->pmem);
         error = VK_ERROR_INVALID_EXTERNAL_HANDLE;
         goto fail;
      }
      if (mem->vk.export_handle_types == mem->vk.import_handle_type) {
         mem->backed_fd = import_info->fd;
      }
      else {
         close(import_info->fd);
      }

      mem->vk.size = size;
      mem->map = device->pscreen->map_memory(device->pscreen, mem->pmem);
      mem->memory_type = memory_type;
   }
   else if (mem->vk.export_handle_types) {
      const enum lvp_device_memory_type memory_type =
         lvp_device_memory_type_for_handle_types(lvp_device_physical(device), mem->vk.export_handle_types);
      const bool dmabuf = memory_type == LVP_DEVICE_MEMORY_TYPE_DMA_BUF;
      mem->pmem = device->pscreen->allocate_memory_fd(device->pscreen, pAllocateInfo->allocationSize, &mem->backed_fd, dmabuf);
      if (!mem->pmem || mem->backed_fd < 0) {
          goto fail;
      }

      mem->map = device->pscreen->map_memory(device->pscreen, mem->pmem);
      mem->memory_type = memory_type;
      /* XXX: this should be memset_s or memset_explicit but they are not supported */
      if (mem_flags && mem_flags->flags & VK_MEMORY_ALLOCATE_ZERO_INITIALIZE_BIT_EXT)
         memset(mem->map, 0, pAllocateInfo->allocationSize);
   }
#endif
   else {
      mem->pmem = device->pscreen->allocate_memory(device->pscreen, pAllocateInfo->allocationSize);
      if (!mem->pmem) {
         goto fail;
      }
      mem->map = device->pscreen->map_memory(device->pscreen, mem->pmem);
      if (device->poison_mem) {
         /* this is a value that will definitely break things */
         memset(mem->map, UINT8_MAX / 2 + 1, pAllocateInfo->allocationSize);
      }
      set_mem_priority(mem, priority);
      /* XXX: this should be memset_s or memset_explicit but they are not supported */
      if (mem_flags && mem_flags->flags & VK_MEMORY_ALLOCATE_ZERO_INITIALIZE_BIT_EXT)
         memset(mem->map, 0, pAllocateInfo->allocationSize);
   }

   *pMem = lvp_device_memory_to_handle(mem);

   return VK_SUCCESS;

fail:
   vk_device_memory_destroy(&device->vk, pAllocator, &mem->vk);
   return vk_error(device, error);
}

VKAPI_ATTR void VKAPI_CALL lvp_FreeMemory(
   VkDevice                                    _device,
   VkDeviceMemory                              _mem,
   const VkAllocationCallbacks*                pAllocator)
{
   VK_FROM_HANDLE(lvp_device, device, _device);
   VK_FROM_HANDLE(lvp_device_memory, mem, _mem);

   if (mem == NULL)
      return;

   if (mem->memory_type != LVP_DEVICE_MEMORY_TYPE_USER_PTR)
      device->pscreen->unmap_memory(device->pscreen, mem->pmem);

   switch(mem->memory_type) {
   case LVP_DEVICE_MEMORY_TYPE_DEFAULT:
      device->pscreen->free_memory(device->pscreen, mem->pmem);
      break;
#ifdef PIPE_MEMORY_FD
   case LVP_DEVICE_MEMORY_TYPE_DMA_BUF:
   case LVP_DEVICE_MEMORY_TYPE_OPAQUE_FD:
      device->pscreen->free_memory_fd(device->pscreen, mem->pmem);
      if(mem->backed_fd >= 0)
         close(mem->backed_fd);
      break;
#endif
   case LVP_DEVICE_MEMORY_TYPE_USER_PTR:
   default:
      break;
   }

   vk_device_memory_destroy(&device->vk, pAllocator, &mem->vk);
}

VKAPI_ATTR VkResult VKAPI_CALL lvp_MapMemory2KHR(
    VkDevice                                    _device,
    const VkMemoryMapInfoKHR*                   pMemoryMapInfo,
    void**                                      ppData)
{
   VK_FROM_HANDLE(lvp_device_memory, mem, pMemoryMapInfo->memory);

   if (mem == NULL) {
      *ppData = NULL;
      return VK_SUCCESS;
   }

   *ppData = (char *)mem->map + pMemoryMapInfo->offset;
   return VK_SUCCESS;
}

VKAPI_ATTR VkResult VKAPI_CALL lvp_UnmapMemory2KHR(
    VkDevice                                    _device,
    const VkMemoryUnmapInfoKHR*                 pMemoryUnmapInfo)
{
   return VK_SUCCESS;
}

VKAPI_ATTR VkResult VKAPI_CALL lvp_FlushMappedMemoryRanges(
   VkDevice                                    _device,
   uint32_t                                    memoryRangeCount,
   const VkMappedMemoryRange*                  pMemoryRanges)
{
   return VK_SUCCESS;
}

VKAPI_ATTR VkResult VKAPI_CALL lvp_InvalidateMappedMemoryRanges(
   VkDevice                                    _device,
   uint32_t                                    memoryRangeCount,
   const VkMappedMemoryRange*                  pMemoryRanges)
{
   return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL lvp_GetDeviceBufferMemoryRequirements(
    VkDevice                                    _device,
    const VkDeviceBufferMemoryRequirements*     pInfo,
    VkMemoryRequirements2*                      pMemoryRequirements)
{
   pMemoryRequirements->memoryRequirements.memoryTypeBits = 1;
   pMemoryRequirements->memoryRequirements.alignment = 64;

   if (pInfo->pCreateInfo->flags & VK_BUFFER_CREATE_SPARSE_BINDING_BIT) {
      uint64_t alignment;
      os_get_page_size(&alignment);
      pMemoryRequirements->memoryRequirements.alignment = alignment;
   }
   pMemoryRequirements->memoryRequirements.size = 0;

   VkBuffer _buffer;
   if (lvp_CreateBuffer(_device, pInfo->pCreateInfo, NULL, &_buffer) != VK_SUCCESS)
      return;

   assert(pInfo->pNext == NULL);
   const VkBufferMemoryRequirementsInfo2 info = {
      .sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_REQUIREMENTS_INFO_2,
      .buffer = _buffer,
   };
   lvp_GetBufferMemoryRequirements2(_device, &info, pMemoryRequirements);
   lvp_DestroyBuffer(_device, _buffer, NULL);
}

VKAPI_ATTR void VKAPI_CALL lvp_GetDeviceImageMemoryRequirements(
    VkDevice                                    _device,
    const VkDeviceImageMemoryRequirements*     pInfo,
    VkMemoryRequirements2*                      pMemoryRequirements)
{
   pMemoryRequirements->memoryRequirements.memoryTypeBits = 1;
   pMemoryRequirements->memoryRequirements.alignment = 0;
   pMemoryRequirements->memoryRequirements.size = 0;

   VkImage _image;
   if (lvp_CreateImage(_device, pInfo->pCreateInfo, NULL, &_image) != VK_SUCCESS)
      return;
   VK_FROM_HANDLE(lvp_image, image, _image);

   /* Per spec VUs of VkImageMemoryRequirementsInfo2 */
   const bool need_plane_info =
      image->vk.create_flags & VK_IMAGE_CREATE_DISJOINT_BIT &&
      (image->plane_count > 1 ||
       image->vk.tiling == VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT);
   const VkImagePlaneMemoryRequirementsInfo plane_info = {
      .sType = VK_STRUCTURE_TYPE_IMAGE_PLANE_MEMORY_REQUIREMENTS_INFO,
      .planeAspect = pInfo->planeAspect,
   };
   const VkImageMemoryRequirementsInfo2 base_info = {
      .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_REQUIREMENTS_INFO_2,
      .pNext = need_plane_info ? &plane_info : NULL,
      .image = _image,
   };
   lvp_GetImageMemoryRequirements2(_device, &base_info, pMemoryRequirements);
   lvp_DestroyImage(_device, _image, NULL);
}

VKAPI_ATTR void VKAPI_CALL lvp_GetBufferMemoryRequirements(
   VkDevice                                    device,
   VkBuffer                                    _buffer,
   VkMemoryRequirements*                       pMemoryRequirements)
{
   VK_FROM_HANDLE(lvp_buffer, buffer, _buffer);

   pMemoryRequirements->alignment = 64;
   if (buffer->vk.create_flags & VK_BUFFER_CREATE_SPARSE_BINDING_BIT) {
      uint64_t alignment;
      os_get_page_size(&alignment);
      pMemoryRequirements->alignment = alignment;
   }
   /* The Vulkan spec (git aaed022) says:
    *
    *    memoryTypeBits is a bitfield and contains one bit set for every
    *    supported memory type for the resource. The bit `1<<i` is set if and
    *    only if the memory type `i` in the VkPhysicalDeviceMemoryProperties
    *    structure for the physical device is supported.
    *
    * We support exactly one memory type.
    */
   pMemoryRequirements->memoryTypeBits = 1;

   pMemoryRequirements->size = buffer->total_size;
}

VKAPI_ATTR void VKAPI_CALL lvp_GetBufferMemoryRequirements2(
   VkDevice                                     device,
   const VkBufferMemoryRequirementsInfo2       *pInfo,
   VkMemoryRequirements2                       *pMemoryRequirements)
{
   lvp_GetBufferMemoryRequirements(device, pInfo->buffer,
                                   &pMemoryRequirements->memoryRequirements);
   vk_foreach_struct(ext, pMemoryRequirements->pNext) {
      switch (ext->sType) {
      case VK_STRUCTURE_TYPE_MEMORY_DEDICATED_REQUIREMENTS: {
         VkMemoryDedicatedRequirements *req =
            (VkMemoryDedicatedRequirements *) ext;
         req->requiresDedicatedAllocation = false;
         req->prefersDedicatedAllocation = req->requiresDedicatedAllocation;
         break;
      }
      default:
         break;
      }
   }
}

VKAPI_ATTR void VKAPI_CALL lvp_GetImageMemoryRequirements(
   VkDevice                                    device,
   VkImage                                     _image,
   VkMemoryRequirements*                       pMemoryRequirements)
{
   VK_FROM_HANDLE(lvp_image, image, _image);
   pMemoryRequirements->memoryTypeBits = 1;

   pMemoryRequirements->size = image->size;
   pMemoryRequirements->alignment = image->alignment;
}

VKAPI_ATTR void VKAPI_CALL lvp_GetImageMemoryRequirements2(
   VkDevice                                    device,
   const VkImageMemoryRequirementsInfo2       *pInfo,
   VkMemoryRequirements2                      *pMemoryRequirements)
{
   lvp_GetImageMemoryRequirements(device, pInfo->image,
                                  &pMemoryRequirements->memoryRequirements);

   vk_foreach_struct(ext, pMemoryRequirements->pNext) {
      switch (ext->sType) {
      case VK_STRUCTURE_TYPE_MEMORY_DEDICATED_REQUIREMENTS: {
         VkMemoryDedicatedRequirements *req =
            (VkMemoryDedicatedRequirements *) ext;
         req->requiresDedicatedAllocation = false;
         req->prefersDedicatedAllocation = req->requiresDedicatedAllocation;
         break;
      }
      default:
         break;
      }
   }
}

VKAPI_ATTR void VKAPI_CALL lvp_GetDeviceMemoryCommitment(
   VkDevice                                    device,
   VkDeviceMemory                              memory,
   VkDeviceSize*                               pCommittedMemoryInBytes)
{
   *pCommittedMemoryInBytes = 0;
}

VKAPI_ATTR VkResult VKAPI_CALL lvp_BindBufferMemory2(VkDevice _device,
                               uint32_t bindInfoCount,
                               const VkBindBufferMemoryInfo *pBindInfos)
{
   VK_FROM_HANDLE(lvp_device, device, _device);
   for (uint32_t i = 0; i < bindInfoCount; ++i) {
      VK_FROM_HANDLE(lvp_device_memory, mem, pBindInfos[i].memory);
      VK_FROM_HANDLE(lvp_buffer, buffer, pBindInfos[i].buffer);
      VkBindMemoryStatusKHR *status = (void*)vk_find_struct_const(&pBindInfos[i], BIND_MEMORY_STATUS_KHR);

      buffer->mem = mem;
      buffer->map = (char*)mem->map + pBindInfos[i].memoryOffset;
      buffer->offset = pBindInfos[i].memoryOffset;
      device->pscreen->resource_bind_backing(device->pscreen,
                                             buffer->bo,
                                             mem->pmem,
                                             0, 0,
                                             pBindInfos[i].memoryOffset);
      buffer->vk.device_address = (VkDeviceAddress)(uintptr_t)buffer->map;
      if (status)
         *status->pResult = VK_SUCCESS;
   }
   return VK_SUCCESS;
}

static VkResult
lvp_image_plane_bind(struct lvp_device *device,
                     struct lvp_image_plane *plane,
                     struct lvp_device_memory *mem,
                     VkDeviceSize memory_offset,
                     VkDeviceSize *plane_offset)
{
   if (!device->pscreen->resource_bind_backing(device->pscreen,
                                               plane->bo,
                                               mem->pmem,
                                               0, 0,
                                               memory_offset + *plane_offset)) {
      /* This is probably caused by the texture being too large, so let's
       * report this as the *closest* allowed error-code. It's not ideal,
       * but it's unlikely that anyone will care too much.
       */
      return vk_error(device, VK_ERROR_OUT_OF_DEVICE_MEMORY);
   }
   plane->pmem = mem->pmem;
   plane->memory_offset = memory_offset;
   plane->plane_offset = *plane_offset;
   *plane_offset += plane->size;
   return VK_SUCCESS;
}

static VkResult
lvp_image_bind(struct lvp_device *device,
               const VkBindImageMemoryInfo *bind_info)
{
   VK_FROM_HANDLE(lvp_device_memory, mem, bind_info->memory);
   VK_FROM_HANDLE(lvp_image, image, bind_info->image);
   uint64_t mem_offset = bind_info->memoryOffset;
   VkResult result;

   if (!mem) {
#if DETECT_OS_ANDROID
      /* TODO handle VkNativeBufferANDROID */
      UNREACHABLE("VkBindImageMemoryInfo with no memory");
#else
      const VkBindImageMemorySwapchainInfoKHR *swapchain_info =
         vk_find_struct_const(bind_info->pNext,
                              BIND_IMAGE_MEMORY_SWAPCHAIN_INFO_KHR);
      assert(swapchain_info && swapchain_info->swapchain != VK_NULL_HANDLE);
      mem = lvp_device_memory_from_handle(wsi_common_get_memory(
         swapchain_info->swapchain, swapchain_info->imageIndex));
      mem_offset = 0;
#endif
   }

   assert(mem);
   uint64_t offset_B = 0;
   if (image->disjoint) {
      const VkBindImagePlaneMemoryInfo *plane_info =
         vk_find_struct_const(bind_info->pNext, BIND_IMAGE_PLANE_MEMORY_INFO);
      const uint8_t plane =
         lvp_image_aspects_to_plane(image, plane_info->planeAspect);
      result = lvp_image_plane_bind(device, &image->planes[plane], mem,
                                    mem_offset, &offset_B);
      if (result != VK_SUCCESS)
         return result;
   } else {
      for (unsigned plane = 0; plane < image->plane_count; plane++) {
         result = lvp_image_plane_bind(device, &image->planes[plane], mem,
                                       mem_offset + image->offset, &offset_B);
         if (result != VK_SUCCESS)
            return result;
      }
   }

   return VK_SUCCESS;
}

VKAPI_ATTR VkResult VKAPI_CALL
lvp_BindImageMemory2(VkDevice _device,
                     uint32_t bindInfoCount,
                     const VkBindImageMemoryInfo *pBindInfos)
{
   VK_FROM_HANDLE(lvp_device, device, _device);
   VkResult result = VK_SUCCESS;

   for (uint32_t i = 0; i < bindInfoCount; i++) {
      const VkBindMemoryStatus *bind_status =
         vk_find_struct_const(&pBindInfos[i], BIND_MEMORY_STATUS);
      VkResult bind_result = lvp_image_bind(device, &pBindInfos[i]);
      if (bind_status)
         *bind_status->pResult = bind_result;
      if (bind_result != VK_SUCCESS)
         result = bind_result;
   }

   return result;
}

#ifdef PIPE_MEMORY_FD

VkResult
lvp_GetMemoryFdKHR(VkDevice _device, const VkMemoryGetFdInfoKHR *pGetFdInfo, int *pFD)
{
   VK_FROM_HANDLE(lvp_device_memory, memory, pGetFdInfo->memory);

   assert(pGetFdInfo->sType == VK_STRUCTURE_TYPE_MEMORY_GET_FD_INFO_KHR);
   assert_memhandle_type(pGetFdInfo->handleType);

   *pFD = os_dupfd_cloexec(memory->backed_fd);
   assert(*pFD >= 0);
   return VK_SUCCESS;
}

VkResult
lvp_GetMemoryFdPropertiesKHR(VkDevice _device,
                             VkExternalMemoryHandleTypeFlagBits handleType,
                             int fd,
                             VkMemoryFdPropertiesKHR *pMemoryFdProperties)
{
   VK_FROM_HANDLE(lvp_device, device, _device);

   assert(pMemoryFdProperties->sType == VK_STRUCTURE_TYPE_MEMORY_FD_PROPERTIES_KHR);

   if (assert_memhandle_type(handleType)) {
      // There is only one memoryType so select this one
      pMemoryFdProperties->memoryTypeBits = 1;
   }
   else {
      const struct lvp_physical_device *pdev = lvp_device_physical(device);
      return vk_error(pdev->vk.instance, VK_ERROR_INVALID_EXTERNAL_HANDLE);
   }
   return VK_SUCCESS;
}

#endif

VKAPI_ATTR VkResult VKAPI_CALL lvp_CreateEvent(
   VkDevice                                    _device,
   const VkEventCreateInfo*                    pCreateInfo,
   const VkAllocationCallbacks*                pAllocator,
   VkEvent*                                    pEvent)
{
   VK_FROM_HANDLE(lvp_device, device, _device);
   struct lvp_event *event = vk_alloc2(&device->vk.alloc, pAllocator,
                                       sizeof(*event), 8,
                                       VK_SYSTEM_ALLOCATION_SCOPE_OBJECT);

   if (!event)
      return vk_error(device, VK_ERROR_OUT_OF_HOST_MEMORY);

   vk_object_base_init(&device->vk, &event->base, VK_OBJECT_TYPE_EVENT);
   *pEvent = lvp_event_to_handle(event);
   event->event_storage = 0;

   return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL lvp_DestroyEvent(
   VkDevice                                    _device,
   VkEvent                                     _event,
   const VkAllocationCallbacks*                pAllocator)
{
   VK_FROM_HANDLE(lvp_device, device, _device);
   VK_FROM_HANDLE(lvp_event, event, _event);

   if (!event)
      return;

   vk_object_base_finish(&event->base);
   vk_free2(&device->vk.alloc, pAllocator, event);
}

VKAPI_ATTR VkResult VKAPI_CALL lvp_GetEventStatus(
   VkDevice                                    _device,
   VkEvent                                     _event)
{
   VK_FROM_HANDLE(lvp_event, event, _event);
   if (event->event_storage == 1)
      return VK_EVENT_SET;
   return VK_EVENT_RESET;
}

VKAPI_ATTR VkResult VKAPI_CALL lvp_SetEvent(
   VkDevice                                    _device,
   VkEvent                                     _event)
{
   VK_FROM_HANDLE(lvp_event, event, _event);
   event->event_storage = 1;

   return VK_SUCCESS;
}

VKAPI_ATTR VkResult VKAPI_CALL lvp_ResetEvent(
   VkDevice                                    _device,
   VkEvent                                     _event)
{
   VK_FROM_HANDLE(lvp_event, event, _event);
   event->event_storage = 0;

   return VK_SUCCESS;
}

void
lvp_sampler_init(struct lvp_device *device, struct lp_descriptor *desc, const VkSamplerCreateInfo *pCreateInfo, const struct vk_sampler *sampler)
{
   struct pipe_sampler_state state = {0};
   VkClearColorValue border_color =
      vk_sampler_border_color_value(pCreateInfo, NULL);
   STATIC_ASSERT(sizeof(state.border_color) == sizeof(border_color));

   state.wrap_s = vk_conv_wrap_mode(pCreateInfo->addressModeU);
   state.wrap_t = vk_conv_wrap_mode(pCreateInfo->addressModeV);
   state.wrap_r = vk_conv_wrap_mode(pCreateInfo->addressModeW);
   state.min_img_filter = pCreateInfo->minFilter == VK_FILTER_LINEAR ? PIPE_TEX_FILTER_LINEAR : PIPE_TEX_FILTER_NEAREST;
   state.min_mip_filter = pCreateInfo->mipmapMode == VK_SAMPLER_MIPMAP_MODE_LINEAR ? PIPE_TEX_MIPFILTER_LINEAR : PIPE_TEX_MIPFILTER_NEAREST;
   state.mag_img_filter = pCreateInfo->magFilter == VK_FILTER_LINEAR ? PIPE_TEX_FILTER_LINEAR : PIPE_TEX_FILTER_NEAREST;
   state.min_lod = pCreateInfo->minLod;
   state.max_lod = pCreateInfo->maxLod;
   state.lod_bias = pCreateInfo->mipLodBias;
   if (pCreateInfo->anisotropyEnable)
      state.max_anisotropy = pCreateInfo->maxAnisotropy;
   else
      state.max_anisotropy = 1;
   state.unnormalized_coords = pCreateInfo->unnormalizedCoordinates;
   state.compare_mode = pCreateInfo->compareEnable ? PIPE_TEX_COMPARE_R_TO_TEXTURE : PIPE_TEX_COMPARE_NONE;
   state.compare_func = pCreateInfo->compareOp;
   state.seamless_cube_map = !(pCreateInfo->flags & VK_SAMPLER_CREATE_NON_SEAMLESS_CUBE_MAP_BIT_EXT);
   STATIC_ASSERT((unsigned)VK_SAMPLER_REDUCTION_MODE_WEIGHTED_AVERAGE == (unsigned)PIPE_TEX_REDUCTION_WEIGHTED_AVERAGE);
   STATIC_ASSERT((unsigned)VK_SAMPLER_REDUCTION_MODE_MIN == (unsigned)PIPE_TEX_REDUCTION_MIN);
   STATIC_ASSERT((unsigned)VK_SAMPLER_REDUCTION_MODE_MAX == (unsigned)PIPE_TEX_REDUCTION_MAX);
   state.reduction_mode = (enum pipe_tex_reduction_mode)sampler->reduction_mode;
   memcpy(&state.border_color, &border_color, sizeof(border_color));

   simple_mtx_lock(&device->queue.lock);
   struct lp_texture_handle *texture_handle = (void *)(uintptr_t)device->queue.ctx->create_texture_handle(device->queue.ctx, NULL, &state);
   desc->texture.sampler_index = texture_handle->sampler_index;
   device->queue.ctx->delete_texture_handle(device->queue.ctx, (uint64_t)(uintptr_t)texture_handle);
   simple_mtx_unlock(&device->queue.lock);

   lp_jit_sampler_from_pipe(&desc->sampler, &state);
}

VKAPI_ATTR VkResult VKAPI_CALL lvp_CreateSampler(
   VkDevice                                    _device,
   const VkSamplerCreateInfo*                  pCreateInfo,
   const VkAllocationCallbacks*                pAllocator,
   VkSampler*                                  pSampler)
{
   VK_FROM_HANDLE(lvp_device, device, _device);
   struct lvp_sampler *sampler;

   sampler = vk_sampler_create(&device->vk, pCreateInfo,
                               pAllocator, sizeof(*sampler));
   if (!sampler)
      return vk_error(device, VK_ERROR_OUT_OF_HOST_MEMORY);

   lvp_sampler_init(device, &sampler->desc, pCreateInfo, &sampler->vk);

   *pSampler = lvp_sampler_to_handle(sampler);

   return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL lvp_DestroySampler(
   VkDevice                                    _device,
   VkSampler                                   _sampler,
   const VkAllocationCallbacks*                pAllocator)
{
   VK_FROM_HANDLE(lvp_device, device, _device);
   VK_FROM_HANDLE(lvp_sampler, sampler, _sampler);

   if (!_sampler)
      return;

   vk_sampler_destroy(&device->vk, pAllocator, &sampler->vk);
}

VKAPI_ATTR VkResult VKAPI_CALL lvp_CreatePrivateDataSlot(
   VkDevice                                    _device,
   const VkPrivateDataSlotCreateInfo*          pCreateInfo,
   const VkAllocationCallbacks*                pAllocator,
   VkPrivateDataSlot*                          pPrivateDataSlot)
{
   VK_FROM_HANDLE(lvp_device, device, _device);
   return vk_private_data_slot_create(&device->vk, pCreateInfo, pAllocator,
                                      pPrivateDataSlot);
}

VKAPI_ATTR void VKAPI_CALL lvp_DestroyPrivateDataSlot(
   VkDevice                                    _device,
   VkPrivateDataSlot                           privateDataSlot,
   const VkAllocationCallbacks*                pAllocator)
{
   VK_FROM_HANDLE(lvp_device, device, _device);
   vk_private_data_slot_destroy(&device->vk, privateDataSlot, pAllocator);
}

VKAPI_ATTR VkResult VKAPI_CALL lvp_SetPrivateData(
   VkDevice                                    _device,
   VkObjectType                                objectType,
   uint64_t                                    objectHandle,
   VkPrivateDataSlot                           privateDataSlot,
   uint64_t                                    data)
{
   VK_FROM_HANDLE(lvp_device, device, _device);
   return vk_object_base_set_private_data(&device->vk, objectType,
                                          objectHandle, privateDataSlot,
                                          data);
}

VKAPI_ATTR void VKAPI_CALL lvp_GetPrivateData(
   VkDevice                                    _device,
   VkObjectType                                objectType,
   uint64_t                                    objectHandle,
   VkPrivateDataSlot                           privateDataSlot,
   uint64_t*                                   pData)
{
   VK_FROM_HANDLE(lvp_device, device, _device);
   vk_object_base_get_private_data(&device->vk, objectType, objectHandle,
                                   privateDataSlot, pData);
}

VKAPI_ATTR void VKAPI_CALL lvp_GetPhysicalDeviceExternalFenceProperties(
   VkPhysicalDevice                           physicalDevice,
   const VkPhysicalDeviceExternalFenceInfo    *pExternalFenceInfo,
   VkExternalFenceProperties                  *pExternalFenceProperties)
{
   VK_FROM_HANDLE(lvp_physical_device, physical_device, physicalDevice);
   const VkExternalFenceHandleTypeFlagBits handle_type = pExternalFenceInfo->handleType;

   if (handle_type == VK_EXTERNAL_FENCE_HANDLE_TYPE_SYNC_FD_BIT &&
       physical_device->pscreen->caps.native_fence_fd) {
      pExternalFenceProperties->exportFromImportedHandleTypes =
         VK_EXTERNAL_FENCE_HANDLE_TYPE_SYNC_FD_BIT;
      pExternalFenceProperties->compatibleHandleTypes =
         VK_EXTERNAL_FENCE_HANDLE_TYPE_SYNC_FD_BIT;
       pExternalFenceProperties->externalFenceFeatures =
          VK_EXTERNAL_FENCE_FEATURE_EXPORTABLE_BIT |
          VK_EXTERNAL_FENCE_FEATURE_IMPORTABLE_BIT;
   } else {
      pExternalFenceProperties->exportFromImportedHandleTypes = 0;
      pExternalFenceProperties->compatibleHandleTypes = 0;
      pExternalFenceProperties->externalFenceFeatures = 0;
   }
}

VKAPI_ATTR void VKAPI_CALL lvp_GetPhysicalDeviceExternalSemaphoreProperties(
   VkPhysicalDevice                            physicalDevice,
   const VkPhysicalDeviceExternalSemaphoreInfo *pExternalSemaphoreInfo,
   VkExternalSemaphoreProperties               *pExternalSemaphoreProperties)
{
   VK_FROM_HANDLE(lvp_physical_device, physical_device, physicalDevice);
   const VkSemaphoreTypeCreateInfo *type_info =
      vk_find_struct_const(pExternalSemaphoreInfo->pNext, SEMAPHORE_TYPE_CREATE_INFO);
   const VkSemaphoreType type = !type_info ? VK_SEMAPHORE_TYPE_BINARY : type_info->semaphoreType;
   const VkExternalSemaphoreHandleTypeFlagBits handle_type = pExternalSemaphoreInfo->handleType;

   if (type == VK_SEMAPHORE_TYPE_BINARY &&
       handle_type == VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_SYNC_FD_BIT &&
       physical_device->pscreen->caps.native_fence_fd) {
      pExternalSemaphoreProperties->exportFromImportedHandleTypes =
         VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_SYNC_FD_BIT;
      pExternalSemaphoreProperties->compatibleHandleTypes =
         VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_SYNC_FD_BIT;
      pExternalSemaphoreProperties->externalSemaphoreFeatures =
         VK_EXTERNAL_SEMAPHORE_FEATURE_EXPORTABLE_BIT |
         VK_EXTERNAL_SEMAPHORE_FEATURE_IMPORTABLE_BIT;
   } else {
      pExternalSemaphoreProperties->exportFromImportedHandleTypes = 0;
      pExternalSemaphoreProperties->compatibleHandleTypes = 0;
      pExternalSemaphoreProperties->externalSemaphoreFeatures = 0;
   }
}

static const VkTimeDomainEXT lvp_time_domains[] = {
        VK_TIME_DOMAIN_DEVICE_EXT,
        VK_TIME_DOMAIN_CLOCK_MONOTONIC_EXT,
};

VKAPI_ATTR VkResult VKAPI_CALL lvp_GetPhysicalDeviceCalibrateableTimeDomainsEXT(
   VkPhysicalDevice physicalDevice,
   uint32_t *pTimeDomainCount,
   VkTimeDomainEXT *pTimeDomains)
{
   int d;
   VK_OUTARRAY_MAKE_TYPED(VkTimeDomainEXT, out, pTimeDomains,
                          pTimeDomainCount);

   for (d = 0; d < ARRAY_SIZE(lvp_time_domains); d++) {
      vk_outarray_append_typed(VkTimeDomainEXT, &out, i) {
         *i = lvp_time_domains[d];
      }
    }

    return vk_outarray_status(&out);
}

VKAPI_ATTR void VKAPI_CALL lvp_GetPhysicalDeviceMultisamplePropertiesEXT(
   VkPhysicalDevice                            physicalDevice,
   VkSampleCountFlagBits                       samples,
   VkMultisamplePropertiesEXT*                 pMultisampleProperties)
{
   assert(pMultisampleProperties->sType ==
          VK_STRUCTURE_TYPE_MULTISAMPLE_PROPERTIES_EXT);

   VkSampleCountFlags sample_counts =
      ~VK_SAMPLE_COUNT_1_BIT & LVP_SAMPLE_COUNTS;

   VkExtent2D grid_size;
   if (samples & sample_counts) {
      grid_size.width = 1;
      grid_size.height = 1;
   } else {
      grid_size.width = 0;
      grid_size.height = 0;
   }
   pMultisampleProperties->maxSampleLocationGridSize = grid_size;
}


VKAPI_ATTR VkResult VKAPI_CALL lvp_GetCalibratedTimestampsEXT(
   VkDevice device,
   uint32_t timestampCount,
   const VkCalibratedTimestampInfoEXT *pTimestampInfos,
   uint64_t *pTimestamps,
   uint64_t *pMaxDeviation)
{
   *pMaxDeviation = 1;

   uint64_t now = os_time_get_nano();
   for (unsigned i = 0; i < timestampCount; i++) {
      pTimestamps[i] = now;
   }
   return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL lvp_GetDeviceGroupPeerMemoryFeatures(
    VkDevice device,
    uint32_t heapIndex,
    uint32_t localDeviceIndex,
    uint32_t remoteDeviceIndex,
    VkPeerMemoryFeatureFlags *pPeerMemoryFeatures)
{
   *pPeerMemoryFeatures = 0;
}

VKAPI_ATTR void VKAPI_CALL lvp_SetDeviceMemoryPriorityEXT(
    VkDevice                                    _device,
    VkDeviceMemory                              _memory,
    float                                       priority)
{
   VK_FROM_HANDLE(lvp_device_memory, mem, _memory);
   set_mem_priority(mem, get_mem_priority(priority));
}

VKAPI_ATTR void VKAPI_CALL lvp_GetRenderingAreaGranularityKHR(
    VkDevice                                    device,
    const VkRenderingAreaInfoKHR*               pRenderingAreaInfo,
    VkExtent2D*                                 pGranularity)
{
   VkExtent2D tile_size = {64, 64};
   *pGranularity = tile_size;
}
