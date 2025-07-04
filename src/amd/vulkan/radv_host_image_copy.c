/*
 * Copyright © 2025 Valve Corporation
 *
 * SPDX-License-Identifier: MIT
 */

#include "radv_device.h"
#include "radv_entrypoints.h"
#include "radv_image.h"
#include "radv_physical_device.h"

static uint32_t
radv_get_texel_scale(VkFormat format)
{
   return vk_format_get_blocksize(format);
}

static const struct ac_surface_copy_region
radv_get_surface_copy_region(struct radv_device *device, const struct radv_image *image, const void *host_ptr,
                             uint32_t memory_row_length, uint32_t memory_image_height,
                             const VkImageSubresourceLayers *subresource, VkOffset3D image_offset,
                             VkExtent3D image_extent)
{
   const void *surf_ptr = image->bindings[0].host_ptr;
   const uint32_t texel_scale = radv_get_texel_scale(image->vk.format);
   const VkOffset3D img_offset_el = vk_image_offset_to_elements(&image->vk, image_offset);
   const VkExtent3D img_extent_el = vk_image_extent_to_elements(&image->vk, image_extent);
   const uint32_t mem_row_pitch = memory_row_length ? memory_row_length : img_extent_el.width;
   const uint32_t mem_slice_pitch = (memory_image_height ? memory_image_height : img_extent_el.height) * mem_row_pitch;

   const struct ac_surface_copy_region surf_copy_region = {
      .surf_ptr = surf_ptr,
      .host_ptr = host_ptr,
      .offset =
         {
            .x = img_offset_el.x,
            .y = img_offset_el.y,
            .z = img_offset_el.z,
         },
      .extent =
         {
            .width = img_extent_el.width,
            .height = img_extent_el.height,
            .depth = img_extent_el.depth,
         },
      .level = subresource->mipLevel,
      .base_layer = subresource->baseArrayLayer,
      .num_layers = vk_image_subresource_layer_count(&image->vk, subresource),
      .mem_row_pitch = mem_row_pitch * texel_scale,
      .mem_slice_pitch = mem_slice_pitch * texel_scale,
   };

   return surf_copy_region;
}

VKAPI_ATTR VkResult VKAPI_CALL
radv_CopyImageToMemoryEXT(VkDevice _device, const VkCopyImageToMemoryInfo *pCopyImageToMemoryInfo)
{
   VK_FROM_HANDLE(radv_device, device, _device);
   VK_FROM_HANDLE(radv_image, image, pCopyImageToMemoryInfo->srcImage);
   const struct radv_physical_device *pdev = radv_device_physical(device);
   const struct radeon_surf *surf = &image->planes[0].surface;
   const void *surf_ptr = image->bindings[0].host_ptr;
   const struct ac_surf_info surf_info = radv_get_ac_surf_info(device, image);

   if (!surf_ptr)
      return VK_ERROR_MEMORY_MAP_FAILED;

   for (uint32_t i = 0; i < pCopyImageToMemoryInfo->regionCount; i++) {
      const VkImageToMemoryCopy *copy = &pCopyImageToMemoryInfo->pRegions[i];

      const struct ac_surface_copy_region surf_copy_region =
         radv_get_surface_copy_region(device, image, copy->pHostPointer, copy->memoryRowLength, copy->memoryImageHeight,
                                      &copy->imageSubresource, copy->imageOffset, copy->imageExtent);

      if (!ac_surface_copy_surface_to_mem(pdev->addrlib, &pdev->info, surf, &surf_info, &surf_copy_region))
         return VK_ERROR_INITIALIZATION_FAILED;
   }

   return VK_SUCCESS;
}

VKAPI_ATTR VkResult VKAPI_CALL
radv_CopyMemoryToImageEXT(VkDevice _device, const VkCopyMemoryToImageInfo *pCopyMemoryToImageInfo)
{
   VK_FROM_HANDLE(radv_device, device, _device);
   VK_FROM_HANDLE(radv_image, image, pCopyMemoryToImageInfo->dstImage);
   const struct radv_physical_device *pdev = radv_device_physical(device);
   const struct radeon_surf *surf = &image->planes[0].surface;
   const void *surf_ptr = image->bindings[0].host_ptr;
   const struct ac_surf_info surf_info = radv_get_ac_surf_info(device, image);

   if (!surf_ptr)
      return VK_ERROR_MEMORY_MAP_FAILED;

   for (uint32_t i = 0; i < pCopyMemoryToImageInfo->regionCount; i++) {
      const VkMemoryToImageCopy *copy = &pCopyMemoryToImageInfo->pRegions[i];

      const struct ac_surface_copy_region surf_copy_region =
         radv_get_surface_copy_region(device, image, copy->pHostPointer, copy->memoryRowLength, copy->memoryImageHeight,
                                      &copy->imageSubresource, copy->imageOffset, copy->imageExtent);

      if (!ac_surface_copy_mem_to_surface(pdev->addrlib, &pdev->info, surf, &surf_info, &surf_copy_region))
         return VK_ERROR_INITIALIZATION_FAILED;
   }

   return VK_SUCCESS;
}
