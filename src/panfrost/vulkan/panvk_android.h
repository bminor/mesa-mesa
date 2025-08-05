/*
 * Copyright 2025 Google LLC
 * SPDX-License-Identifier: MIT
 */

#ifndef PANVK_ANDROID_H
#define PANVK_ANDROID_H

#include <stdbool.h>

#include "util/detect_os.h"
#include "vulkan/vulkan.h"

#ifdef VK_USE_PLATFORM_ANDROID_KHR

bool panvk_android_is_gralloc_image(const VkImageCreateInfo *pCreateInfo);

VkResult panvk_android_create_gralloc_image(
   VkDevice device, const VkImageCreateInfo *pCreateInfo,
   const VkAllocationCallbacks *pAllocator, VkImage *pImage);

#else /* VK_USE_PLATFORM_ANDROID_KHR */

static inline bool
panvk_android_is_gralloc_image(const VkImageCreateInfo *pCreateInfo)
{
   return false;
}

static inline VkResult
panvk_android_create_gralloc_image(VkDevice device,
                                   const VkImageCreateInfo *pCreateInfo,
                                   const VkAllocationCallbacks *pAllocator,
                                   VkImage *pImage)
{
   return VK_ERROR_FEATURE_NOT_PRESENT;
}

#endif /* VK_USE_PLATFORM_ANDROID_KHR */

#endif /* PANVK_ANDROID_H */
