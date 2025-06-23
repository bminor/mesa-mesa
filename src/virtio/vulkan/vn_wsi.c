/*
 * Copyright 2019 Google LLC
 * SPDX-License-Identifier: MIT
 *
 * based in part on anv and radv which are:
 * Copyright © 2015 Intel Corporation
 * Copyright © 2016 Red Hat.
 * Copyright © 2016 Bas Nieuwenhuizen
 */

#include "vn_wsi.h"

#include <xf86drm.h>

#include "drm-uapi/dma-buf.h"
#include "vk_enum_to_str.h"
#include "wsi_common_entrypoints.h"

#include "vn_device.h"
#include "vn_image.h"
#include "vn_instance.h"
#include "vn_physical_device.h"
#include "vn_queue.h"

/* The common WSI support makes some assumptions about the driver.
 *
 * In wsi_device_init, it assumes VK_EXT_pci_bus_info is available.  In
 * wsi_create_native_image and wsi_create_prime_image, it assumes
 * VK_KHR_external_memory_fd and VK_EXT_external_memory_dma_buf are enabled.
 *
 * In wsi_create_native_image, if wsi_device::supports_modifiers is set and
 * the window system supports modifiers, it assumes
 * VK_EXT_image_drm_format_modifier is enabled.  Otherwise, it assumes that
 * wsi_image_create_info can be chained to VkImageCreateInfo and
 * vkGetImageSubresourceLayout can be called even the tiling is
 * VK_IMAGE_TILING_OPTIMAL.
 *
 * Together, it knows how to share dma-bufs, with explicit or implicit
 * modifiers, to the window system.
 *
 * For venus, we use explicit modifiers when the renderer and the window
 * system support them.  Otherwise, we have to fall back to
 * VK_IMAGE_TILING_LINEAR (or trigger the prime blit path).  But the fallback
 * can be problematic when the memory is scanned out directly and special
 * requirements (e.g., alignments) must be met.
 *
 * The common WSI support makes other assumptions about the driver to support
 * implicit fencing.  In wsi_create_native_image and wsi_create_prime_image,
 * it assumes wsi_memory_allocate_info can be chained to VkMemoryAllocateInfo.
 * In wsi_common_queue_present, it assumes wsi_memory_signal_submit_info can
 * be chained to VkSubmitInfo.  Finally, in wsi_common_acquire_next_image2, it
 * calls wsi_device::signal_semaphore_for_memory, and
 * wsi_device::signal_fence_for_memory if the driver provides them.
 *
 * Some drivers use wsi_memory_allocate_info to set up implicit fencing.
 * Others use wsi_memory_signal_submit_info to set up implicit IN-fences and
 * use wsi_device::signal_*_for_memory to set up implicit OUT-fences.
 *
 * For venus, implicit fencing is broken (and there is no explicit fencing
 * support yet).  The kernel driver assumes everything is in the same fence
 * context and no synchronization is needed.  It should be fixed for
 * correctness, but it is still not ideal.  venus requires explicit fencing
 * (and renderer-side synchronization) to work well.
 */

/* cast a WSI object to a pointer for logging */
#define VN_WSI_PTR(obj) ((const void *)(uintptr_t)(obj))

static PFN_vkVoidFunction
vn_wsi_proc_addr(VkPhysicalDevice physicalDevice, const char *pName)
{
   struct vn_physical_device *physical_dev =
      vn_physical_device_from_handle(physicalDevice);
   return vk_instance_get_proc_addr_unchecked(
      &physical_dev->instance->base.vk, pName);
}

VkResult
vn_wsi_init(struct vn_physical_device *physical_dev)
{
   /* TODO Drop the workaround for NVIDIA_PROPRIETARY once hw prime buffer
    * blit path works there.
    */
   const bool use_sw_device =
      !physical_dev->base.vk.supported_extensions
          .EXT_external_memory_dma_buf ||
      physical_dev->renderer_driver_id == VK_DRIVER_ID_NVIDIA_PROPRIETARY;

   const VkAllocationCallbacks *alloc =
      &physical_dev->instance->base.vk.alloc;
   VkResult result = wsi_device_init(
      &physical_dev->wsi_device, vn_physical_device_to_handle(physical_dev),
      vn_wsi_proc_addr, alloc, -1, &physical_dev->instance->dri_options,
      &(struct wsi_device_options){
         .sw_device = use_sw_device,
         .extra_xwayland_image = true,
      });
   if (result != VK_SUCCESS)
      return result;

   physical_dev->wsi_device.supports_scanout = false;
   physical_dev->wsi_device.supports_modifiers =
      physical_dev->base.vk.supported_extensions.EXT_image_drm_format_modifier;
   physical_dev->base.vk.wsi_device = &physical_dev->wsi_device;

   return VK_SUCCESS;
}

void
vn_wsi_fini(struct vn_physical_device *physical_dev)
{
   const VkAllocationCallbacks *alloc =
      &physical_dev->instance->base.vk.alloc;
   physical_dev->base.vk.wsi_device = NULL;
   wsi_device_finish(&physical_dev->wsi_device, alloc);
}

VkResult
vn_wsi_create_image(struct vn_device *dev,
                    const VkImageCreateInfo *create_info,
                    const struct wsi_image_create_info *wsi_info,
                    const VkAllocationCallbacks *alloc,
                    struct vn_image **out_img)
{
   VkImageCreateInfo local_create_info;
   if (dev->physical_device->renderer_driver_id ==
          VK_DRIVER_ID_INTEL_OPEN_SOURCE_MESA &&
       (create_info->flags & VK_IMAGE_CREATE_ALIAS_BIT)) {
      /* See explanation in vn_GetPhysicalDeviceImageFormatProperties2() */
      local_create_info = *create_info;
      local_create_info.flags &= ~VK_IMAGE_CREATE_ALIAS_BIT;
      create_info = &local_create_info;
   }

   struct vn_image *img;
   VkResult result = vn_image_create(dev, create_info, alloc, &img);
   if (result != VK_SUCCESS)
      return result;

   img->wsi.is_wsi = true;
   img->wsi.is_prime_blit_src = wsi_info->blit_src;
   img->wsi.tiling_override = create_info->tiling;

   if (create_info->tiling == VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT) {
      VkDevice dev_handle = vn_device_to_handle(dev);
      VkImage img_handle = vn_image_to_handle(img);

      VkImageDrmFormatModifierPropertiesEXT props = {
         .sType = VK_STRUCTURE_TYPE_IMAGE_DRM_FORMAT_MODIFIER_PROPERTIES_EXT,
      };
      result = vn_GetImageDrmFormatModifierPropertiesEXT(dev_handle,
                                                         img_handle, &props);
      if (result != VK_SUCCESS) {
         vn_DestroyImage(dev_handle, img_handle, alloc);
         return result;
      }

      img->wsi.drm_format_modifier = props.drmFormatModifier;
   }

   *out_img = img;
   return VK_SUCCESS;
}

VkResult
vn_wsi_create_image_from_swapchain(
   struct vn_device *dev,
   const VkImageCreateInfo *create_info,
   const VkImageSwapchainCreateInfoKHR *swapchain_info,
   const VkAllocationCallbacks *alloc,
   struct vn_image **out_img)
{
   const struct vn_image *swapchain_img = vn_image_from_handle(
      wsi_common_get_image(swapchain_info->swapchain, 0));
   assert(swapchain_img->wsi.is_wsi);

   /* must match what the common WSI and vn_wsi_create_image do */
   VkImageCreateInfo local_create_info = *create_info;

   /* match external memory */
   const VkExternalMemoryImageCreateInfo local_external_info = {
      .sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO,
      .pNext = local_create_info.pNext,
      .handleTypes =
         dev->physical_device->external_memory.renderer_handle_type,
   };
   local_create_info.pNext = &local_external_info;

   /* match image tiling */
   local_create_info.tiling = swapchain_img->wsi.tiling_override;

   VkImageDrmFormatModifierListCreateInfoEXT local_mod_info;
   if (local_create_info.tiling == VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT) {
      local_mod_info = (const VkImageDrmFormatModifierListCreateInfoEXT){
         .sType =
            VK_STRUCTURE_TYPE_IMAGE_DRM_FORMAT_MODIFIER_LIST_CREATE_INFO_EXT,
         .pNext = local_create_info.pNext,
         .drmFormatModifierCount = 1,
         .pDrmFormatModifiers = &swapchain_img->wsi.drm_format_modifier,
      };
      local_create_info.pNext = &local_mod_info;
   }

   /* match image usage */
   if (swapchain_img->wsi.is_prime_blit_src)
      local_create_info.usage |= VK_IMAGE_USAGE_TRANSFER_SRC_BIT;

   create_info = &local_create_info;

   struct vn_image *img;
   VkResult result = vn_image_create(dev, create_info, alloc, &img);
   if (result != VK_SUCCESS)
      return result;

   img->wsi.is_wsi = true;
   img->wsi.tiling_override = swapchain_img->wsi.tiling_override;
   img->wsi.drm_format_modifier = swapchain_img->wsi.drm_format_modifier;

   *out_img = img;
   return VK_SUCCESS;
}

/* swapchain commands */

static int
vn_wsi_export_sync_file(struct vn_device *dev, struct vn_renderer_bo *bo)
{
   /* Don't keep trying an IOCTL that doesn't exist. */
   static bool no_dma_buf_sync_file = false;
   if (no_dma_buf_sync_file)
      return -1;

   /* For simplicity, export dma-buf here and rely on the dma-buf sync file
    * export api. On legacy kernels without the new uapi, for the record, we
    * do have the fallback option to track the wsi bo in the sync payload and
    * do DRM_IOCTL_VIRTGPU_WAIT where we do sync_wait.
    */
   int dma_buf_fd = vn_renderer_bo_export_dma_buf(dev->renderer, bo);
   if (dma_buf_fd < 0)
      return -1;

   struct dma_buf_export_sync_file export = {
      .flags = DMA_BUF_SYNC_RW,
      .fd = -1,
   };
   int ret = drmIoctl(dma_buf_fd, DMA_BUF_IOCTL_EXPORT_SYNC_FILE, &export);

   close(dma_buf_fd);

   if (ret && (errno == ENOTTY || errno == EBADF || errno == ENOSYS)) {
      no_dma_buf_sync_file = true;
      return -1;
   }

   return export.fd;
}

VkResult
vn_AcquireNextImage2KHR(VkDevice device,
                        const VkAcquireNextImageInfoKHR *pAcquireInfo,
                        uint32_t *pImageIndex)
{
   VN_TRACE_FUNC();
   struct vn_device *dev = vn_device_from_handle(device);

   VkResult result = wsi_common_acquire_next_image2(
      &dev->physical_device->wsi_device, device, pAcquireInfo, pImageIndex);
   if (VN_DEBUG(WSI) && result != VK_SUCCESS) {
      const int idx = result >= VK_SUCCESS ? *pImageIndex : -1;
      vn_log(dev->instance, "swapchain %p: acquired image %d: %s",
             VN_WSI_PTR(pAcquireInfo->swapchain), idx,
             vk_Result_to_str(result));
   }

   if (result != VK_SUCCESS && result != VK_SUBOPTIMAL_KHR)
      return vn_error(dev->instance, result);

   /* Extract compositor implicit fence and resolve on the driver side upon
    * the acquire fence being submitted. Since we used to rely on renderer
    * side drivers being able to handle implicit in-fence, here we only opt-in
    * the new behavior for those known to be unable to handle it.
    */
   int sync_fd = -1;
   if (dev->physical_device->renderer_driver_id ==
       VK_DRIVER_ID_NVIDIA_PROPRIETARY) {
      struct vn_image *wsi_img = vn_image_from_handle(
         wsi_common_get_image(pAcquireInfo->swapchain, *pImageIndex));
      assert(wsi_img->wsi.is_wsi);

      struct vn_device_memory *wsi_mem = wsi_img->wsi.is_prime_blit_src
                                            ? wsi_img->wsi.blit_mem
                                            : wsi_img->wsi.memory;
      if (wsi_mem)
         sync_fd = vn_wsi_export_sync_file(dev, wsi_mem->base_bo);
   }

   int sem_fd = -1, fence_fd = -1;
   if (sync_fd >= 0) {
      if (pAcquireInfo->semaphore != VK_NULL_HANDLE &&
          pAcquireInfo->fence != VK_NULL_HANDLE) {
         sem_fd = sync_fd;
         fence_fd = dup(sync_fd);
         if (fence_fd < 0) {
            result = errno == EMFILE ? VK_ERROR_TOO_MANY_OBJECTS
                                     : VK_ERROR_OUT_OF_HOST_MEMORY;
            close(sync_fd);
            return vn_error(dev->instance, result);
         }
      } else if (pAcquireInfo->semaphore != VK_NULL_HANDLE) {
         sem_fd = sync_fd;
      } else {
         assert(pAcquireInfo->fence != VK_NULL_HANDLE);
         fence_fd = sync_fd;
      }
   }

   if (pAcquireInfo->semaphore != VK_NULL_HANDLE) {
      /* venus waits on the driver side when this semaphore is submitted */
      const VkImportSemaphoreFdInfoKHR info = {
         .sType = VK_STRUCTURE_TYPE_IMPORT_SEMAPHORE_FD_INFO_KHR,
         .semaphore = pAcquireInfo->semaphore,
         .flags = VK_SEMAPHORE_IMPORT_TEMPORARY_BIT,
         .handleType = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_SYNC_FD_BIT,
         .fd = sem_fd,
      };
      result = vn_ImportSemaphoreFdKHR(device, &info);
      if (result == VK_SUCCESS)
         sem_fd = -1;
   }

   if (result == VK_SUCCESS && pAcquireInfo->fence != VK_NULL_HANDLE) {
      const VkImportFenceFdInfoKHR info = {
         .sType = VK_STRUCTURE_TYPE_IMPORT_FENCE_FD_INFO_KHR,
         .fence = pAcquireInfo->fence,
         .flags = VK_FENCE_IMPORT_TEMPORARY_BIT,
         .handleType = VK_EXTERNAL_FENCE_HANDLE_TYPE_SYNC_FD_BIT,
         .fd = fence_fd,
      };
      result = vn_ImportFenceFdKHR(device, &info);
      if (result == VK_SUCCESS)
         fence_fd = -1;
   }

   if (sem_fd >= 0)
      close(sem_fd);
   if (fence_fd >= 0)
      close(fence_fd);

   return vn_result(dev->instance, result);
}
