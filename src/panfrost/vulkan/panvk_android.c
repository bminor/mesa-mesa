/*
 * Copyright 2025 Google LLC
 * SPDX-License-Identifier: MIT
 */

#include "panvk_android.h"

#include "panvk_device.h"

#include "vulkan/vk_android_native_buffer.h"

#include "vk_alloc.h"
#include "vk_android.h"
#include "vk_util.h"

bool
panvk_android_is_gralloc_image(const VkImageCreateInfo *pCreateInfo)
{
   vk_foreach_struct_const(ext, pCreateInfo->pNext) {
      switch ((uint32_t)ext->sType) {
      case VK_STRUCTURE_TYPE_NATIVE_BUFFER_ANDROID:
         return true;
      case VK_STRUCTURE_TYPE_IMAGE_SWAPCHAIN_CREATE_INFO_KHR: {
         const VkImageSwapchainCreateInfoKHR *swapchain_info = (void *)ext;
         if (swapchain_info->swapchain != VK_NULL_HANDLE)
            return true;
         break;
      }
      case VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO: {
         const VkExternalMemoryImageCreateInfo *external_info = (void *)ext;
         if (external_info->handleTypes &
             VK_EXTERNAL_MEMORY_HANDLE_TYPE_ANDROID_HARDWARE_BUFFER_BIT_ANDROID)
            return true;
         break;
      }
      default:
         break;
      }
   }
   return false;
}

struct panvk_android_deferred_image {
   struct panvk_image base;

   VkImageCreateInfo *create_info;
   bool initialized;
};

static VkResult
panvk_android_create_deferred_image(VkDevice device,
                                    const VkImageCreateInfo *pCreateInfo,
                                    const VkAllocationCallbacks *pAllocator,
                                    VkImage *pImage)
{
   VK_FROM_HANDLE(panvk_device, dev, device);

   /* collect all dynamic array infos */
   uint32_t queue_family_count = 0;
   uint32_t view_format_count = 0;

   if (pCreateInfo->sharingMode == VK_SHARING_MODE_CONCURRENT)
      queue_family_count = pCreateInfo->queueFamilyIndexCount;

   const VkImageFormatListCreateInfo *raw_list =
      vk_find_struct_const(pCreateInfo->pNext, IMAGE_FORMAT_LIST_CREATE_INFO);
   if (raw_list)
      view_format_count = raw_list->viewFormatCount;

   /* Extend below when panvk supports more extensions that interact with ANB or
    * AHB. e.g. VK_EXT_image_compression_control
    */
   VK_MULTIALLOC(ma);
   VK_MULTIALLOC_DECL(&ma, struct panvk_android_deferred_image, deferred, 1);
   VK_MULTIALLOC_DECL(&ma, VkImageCreateInfo, create_info, 1);
   VK_MULTIALLOC_DECL(&ma, VkImageFormatListCreateInfo, list_info, 1);
   VK_MULTIALLOC_DECL(&ma, VkImageStencilUsageCreateInfo, stencil_info, 1);
   VK_MULTIALLOC_DECL(&ma, uint32_t, queue_families, queue_family_count);
   VK_MULTIALLOC_DECL(&ma, uint32_t, view_formats, view_format_count);

   if (!vk_multialloc_zalloc2(&ma, &dev->vk.alloc, pAllocator,
                              VK_SYSTEM_ALLOCATION_SCOPE_OBJECT))
      return panvk_error(dev, VK_ERROR_OUT_OF_HOST_MEMORY);

   vk_image_init(&dev->vk, &deferred->base.vk, pCreateInfo);

   /* prepare the deferred VkImageCreateInfo chain */
   *create_info = *pCreateInfo;
   create_info->pNext = NULL;
   create_info->tiling = deferred->base.vk.tiling =
      VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT;
   if (pCreateInfo->sharingMode == VK_SHARING_MODE_CONCURRENT) {
      typed_memcpy(queue_families, pCreateInfo->pQueueFamilyIndices,
                   pCreateInfo->queueFamilyIndexCount);
      create_info->pQueueFamilyIndices = queue_families;
   }

   /* Per spec section 12.3. Images
    *
    * - If tiling is VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT and flags contains
    *   VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT, then the pNext chain must include a
    *   VkImageFormatListCreateInfo structure with non-zero viewFormatCount.
    *
    * ANB and aliased ANB always chain proper format list for mutable swapchain
    * image support, but AHB is allowed to mutate without an explicit format
    * list due to legacy spec issue. So we chain a view format of the create
    * format itself to satisfy VK_EXT_image_drm_format_modifier VUs.
    */
   if (view_format_count ||
       deferred->base.vk.create_flags & VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT) {
      if (view_format_count) {
         typed_memcpy(view_formats, raw_list->pViewFormats, view_format_count);
      } else {
         view_format_count = 1;
         view_formats = &create_info->format;
      }
      *list_info = (VkImageFormatListCreateInfo){
         .sType = VK_STRUCTURE_TYPE_IMAGE_FORMAT_LIST_CREATE_INFO,
         .viewFormatCount = view_format_count,
         .pViewFormats = view_formats,
      };
      __vk_append_struct(create_info, list_info);
   }

   if (deferred->base.vk.stencil_usage) {
      *stencil_info = (VkImageStencilUsageCreateInfo){
         .sType = VK_STRUCTURE_TYPE_IMAGE_STENCIL_USAGE_CREATE_INFO,
         .stencilUsage = deferred->base.vk.stencil_usage,
      };
      __vk_append_struct(create_info, stencil_info);
   }

   deferred->create_info = create_info;
   *pImage = panvk_image_to_handle(&deferred->base);

   return VK_SUCCESS;
}

static inline uint32_t
panvk_android_get_fd_mem_type_bits(VkDevice dev_handle, int dma_buf_fd)
{
   VK_FROM_HANDLE(vk_device, dev, dev_handle);

   VkMemoryFdPropertiesKHR fd_props = {
      .sType = VK_STRUCTURE_TYPE_MEMORY_FD_PROPERTIES_KHR,
   };
   VkResult result = dev->dispatch_table.GetMemoryFdPropertiesKHR(
      dev_handle, VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT, dma_buf_fd,
      &fd_props);
   return result == VK_SUCCESS ? fd_props.memoryTypeBits : 0;
}

static VkResult
panvk_android_get_image_mem_reqs(VkDevice dev_handle, VkImage img_handle,
                                 int dma_buf_fd,
                                 VkMemoryRequirements *out_mem_reqs)
{
   VK_FROM_HANDLE(vk_device, dev, dev_handle);
   VkMemoryRequirements mem_reqs;

   dev->dispatch_table.GetImageMemoryRequirements(dev_handle, img_handle,
                                                  &mem_reqs);

   const uint32_t fd_mem_type_bits =
      panvk_android_get_fd_mem_type_bits(dev_handle, dma_buf_fd);

   if (!(mem_reqs.memoryTypeBits & fd_mem_type_bits)) {
      return panvk_errorf(dev_handle, VK_ERROR_INVALID_EXTERNAL_HANDLE,
                          "No compatible mem type: img req (%u), fd req (%u)",
                          mem_reqs.memoryTypeBits, fd_mem_type_bits);
   }

   mem_reqs.memoryTypeBits &= fd_mem_type_bits;
   *out_mem_reqs = mem_reqs;

   return VK_SUCCESS;
}

static VkResult
panvk_android_import_anb_memory(VkDevice dev_handle, VkImage img_handle,
                                const VkNativeBufferANDROID *anb,
                                const VkAllocationCallbacks *alloc)
{
   VK_FROM_HANDLE(vk_device, dev, dev_handle);
   VK_FROM_HANDLE(panvk_image, img, img_handle);
   VkMemoryRequirements mem_reqs;
   VkResult result;

   assert(anb && anb->handle && anb->handle->numFds > 0);

   int dma_buf_fd = anb->handle->data[0];
   result = panvk_android_get_image_mem_reqs(dev_handle, img_handle, dma_buf_fd,
                                             &mem_reqs);
   if (result != VK_SUCCESS)
      return result;

   int dup_fd = os_dupfd_cloexec(dma_buf_fd);
   if (dup_fd < 0) {
      return (errno == EMFILE) ? VK_ERROR_TOO_MANY_OBJECTS
                               : VK_ERROR_OUT_OF_HOST_MEMORY;
   }

   const VkMemoryDedicatedAllocateInfo dedicated_info = {
      .sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO,
      .image = img_handle,
   };
   const VkImportMemoryFdInfoKHR fd_info = {
      .sType = VK_STRUCTURE_TYPE_IMPORT_MEMORY_FD_INFO_KHR,
      .pNext = &dedicated_info,
      .handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT,
      .fd = dup_fd,
   };
   const VkMemoryAllocateInfo alloc_info = {
      .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
      .pNext = &fd_info,
      .allocationSize = mem_reqs.size,
      .memoryTypeIndex = ffs(mem_reqs.memoryTypeBits) - 1,
   };
   result = dev->dispatch_table.AllocateMemory(dev_handle, &alloc_info, alloc,
                                               &img->vk.anb_memory);
   if (result != VK_SUCCESS) {
      close(dup_fd);
      return result;
   }

   return VK_SUCCESS;
}

static VkResult
panvk_android_anb_init(struct panvk_device *dev, VkImageCreateInfo *create_info,
                       const VkNativeBufferANDROID *anb,
                       const VkAllocationCallbacks *alloc,
                       struct panvk_image *img)
{
   VkResult result;

   VkImageDrmFormatModifierExplicitCreateInfoEXT mod_info;
   VkSubresourceLayout layouts[PANVK_MAX_PLANES];
   assert(vk_find_struct_const(create_info->pNext, NATIVE_BUFFER_ANDROID));
   result = vk_android_get_anb_layout(create_info, &mod_info, layouts,
                                      PANVK_MAX_PLANES);
   if (result != VK_SUCCESS)
      return result;

   mod_info.pNext = create_info->pNext;
   const VkExternalMemoryImageCreateInfo external_info = {
      .sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO,
      .pNext = &mod_info,
      .handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT,
   };

   /* create_info is a already local copy from the caller */
   create_info->pNext = &external_info;
   result = panvk_image_init(img, create_info);
   if (result != VK_SUCCESS)
      return result;

   result = panvk_android_import_anb_memory(
      panvk_device_to_handle(dev), panvk_image_to_handle(img), anb, alloc);
   if (result != VK_SUCCESS)
      return result;

   return VK_SUCCESS;
}

VkResult
panvk_android_create_gralloc_image(VkDevice device,
                                   const VkImageCreateInfo *pCreateInfo,
                                   const VkAllocationCallbacks *pAllocator,
                                   VkImage *pImage)
{
   VK_FROM_HANDLE(panvk_device, dev, device);
   VkResult result;

   const VkNativeBufferANDROID *anb =
      vk_find_struct_const(pCreateInfo->pNext, NATIVE_BUFFER_ANDROID);
   if (!anb) {
      return panvk_android_create_deferred_image(device, pCreateInfo,
                                                 pAllocator, pImage);
   }

   struct panvk_image *img =
      vk_image_create(&dev->vk, pCreateInfo, pAllocator, sizeof(*img));
   if (!img)
      return panvk_error(device, VK_ERROR_OUT_OF_HOST_MEMORY);

   VkImageCreateInfo create_info = *pCreateInfo;
   create_info.tiling = img->vk.tiling =
      VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT;

   result = panvk_android_anb_init(dev, &create_info, anb, pAllocator, img);
   if (result != VK_SUCCESS) {
      vk_image_destroy(&dev->vk, pAllocator, &img->vk);
      return panvk_error(device, result);
   }

   VkImage img_handle = panvk_image_to_handle(img);
   result = dev->vk.dispatch_table.BindImageMemory(device, img_handle,
                                                   img->vk.anb_memory, 0);
   if (result != VK_SUCCESS) {
      dev->vk.dispatch_table.DestroyImage(device, img_handle, pAllocator);
      return panvk_error(device, result);
   }

   *pImage = img_handle;

   return VK_SUCCESS;
}
