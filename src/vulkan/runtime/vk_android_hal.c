/*
 * Copyright 2025 Google LLC
 * SPDX-License-Identifier: MIT
 */

#include <hardware/hardware.h>
#include <hardware/hwvulkan.h>
#include <util/macros.h>
#include <vulkan/vk_icd.h>

#include <stdlib.h>
#include <string.h>

static_assert(HWVULKAN_DISPATCH_MAGIC == ICD_LOADER_MAGIC, "");

static int vk_android_hal_open(const struct hw_module_t *mod, const char *id,
                               struct hw_device_t **dev);

PUBLIC struct hwvulkan_module_t HAL_MODULE_INFO_SYM = {
   .common =
      {
         .tag = HARDWARE_MODULE_TAG,
         .module_api_version = HWVULKAN_MODULE_API_VERSION_0_1,
         .hal_api_version = HARDWARE_MAKE_API_VERSION(1, 0),
         .id = HWVULKAN_HARDWARE_MODULE_ID,
         .name = "Mesa 3D Vulkan HAL",
         .author = "Mesa 3D",
         .methods =
            &(hw_module_methods_t){
               .open = vk_android_hal_open,
            },
      },
};

static int
vk_android_hal_close(struct hw_device_t *dev)
{
   /* the hw_device_t::close() function is called upon driver unloading */
   assert(dev->version == HWVULKAN_DEVICE_API_VERSION_0_1);
   assert(dev->module == &HAL_MODULE_INFO_SYM.common);

   hwvulkan_device_t *hal_dev = container_of(dev, hwvulkan_device_t, common);
   free(hal_dev);
   return 0;
}

static int
vk_android_hal_open(const struct hw_module_t *mod, const char *id,
                    struct hw_device_t **dev)
{
   assert(mod == &HAL_MODULE_INFO_SYM.common);
   assert(strcmp(id, HWVULKAN_DEVICE_0) == 0);

   hwvulkan_device_t *hal_dev = malloc(sizeof(*hal_dev));
   if (!hal_dev)
      return -1;

   *hal_dev = (hwvulkan_device_t){
      .common =
         {
            .tag = HARDWARE_DEVICE_TAG,
            .version = HWVULKAN_DEVICE_API_VERSION_0_1,
            .module = &HAL_MODULE_INFO_SYM.common,
            .close = vk_android_hal_close,
         },
      .EnumerateInstanceExtensionProperties =
         (PFN_vkEnumerateInstanceExtensionProperties)vk_icdGetInstanceProcAddr(
            NULL, "vkEnumerateInstanceExtensionProperties"),
      .CreateInstance =
         (PFN_vkCreateInstance)vk_icdGetInstanceProcAddr(
            NULL, "vkCreateInstance"),
      .GetInstanceProcAddr =
         (PFN_vkGetInstanceProcAddr)vk_icdGetInstanceProcAddr(
            NULL, "vkGetInstanceProcAddr"),
   };

   *dev = &hal_dev->common;
   return 0;
}
