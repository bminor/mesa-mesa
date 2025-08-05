/*
 * Copyright 2025 Google LLC
 * SPDX-License-Identifier: MIT
 */

#include "panvk_android.h"

#include "vulkan/vk_android_native_buffer.h"

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

static VkResult
panvk_android_create_deferred_image(VkDevice device,
                                    const VkImageCreateInfo *pCreateInfo,
                                    const VkAllocationCallbacks *pAllocator,
                                    VkImage *pImage)
{
   return VK_ERROR_FEATURE_NOT_PRESENT;
}

VkResult
panvk_android_create_gralloc_image(VkDevice device,
                                   const VkImageCreateInfo *pCreateInfo,
                                   const VkAllocationCallbacks *pAllocator,
                                   VkImage *pImage)
{
   const VkNativeBufferANDROID *anb =
      vk_find_struct_const(pCreateInfo->pNext, NATIVE_BUFFER_ANDROID);
   if (!anb) {
      return panvk_android_create_deferred_image(device, pCreateInfo,
                                                 pAllocator, pImage);
   }

   return VK_ERROR_FEATURE_NOT_PRESENT;
}
