/*
 * Copyright © 2022 Imagination Technologies Ltd.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the next
 * paragraph) shall be included in all copies or substantial portions of the
 * Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */

#include "pvr_image.h"

#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "pvr_buffer.h"
#include "pvr_device.h"
#include "pvr_device_info.h"
#include "pvr_entrypoints.h"
#include "pvr_formats.h"
#include "pvr_macros.h"
#include "pvr_physical_device.h"
#include "pvr_tex_state.h"
#include "util/macros.h"
#include "util/u_math.h"
#include "vk_format.h"
#include "vk_image.h"
#include "vk_log.h"
#include "vk_object.h"
#include "vk_util.h"
#include "wsi_common.h"

static void pvr_image_init_memlayout(struct pvr_image *image)
{
   switch (image->vk.tiling) {
   default:
      UNREACHABLE("bad VkImageTiling");
   case VK_IMAGE_TILING_OPTIMAL:
      if (image->vk.wsi_legacy_scanout)
         image->memlayout = PVR_MEMLAYOUT_LINEAR;
      else if (image->vk.image_type == VK_IMAGE_TYPE_3D)
         image->memlayout = PVR_MEMLAYOUT_3DTWIDDLED;
      else
         image->memlayout = PVR_MEMLAYOUT_TWIDDLED;
      break;
   case VK_IMAGE_TILING_LINEAR:
      image->memlayout = PVR_MEMLAYOUT_LINEAR;
      break;
   }
}

static void pvr_image_init_physical_extent(struct pvr_image *image,
                                           unsigned pbe_stride_align)
{
   assert(image->memlayout != PVR_MEMLAYOUT_UNDEFINED);

   /* clang-format off */
   if (image->vk.mip_levels > 1 ||
      image->memlayout == PVR_MEMLAYOUT_TWIDDLED ||
      image->memlayout == PVR_MEMLAYOUT_3DTWIDDLED) {
      /* clang-format on */
      image->physical_extent.width =
         util_next_power_of_two(image->vk.extent.width);
      image->physical_extent.height =
         util_next_power_of_two(image->vk.extent.height);
      image->physical_extent.depth =
         util_next_power_of_two(image->vk.extent.depth);
   } else {
      assert(image->memlayout == PVR_MEMLAYOUT_LINEAR);
      image->physical_extent = image->vk.extent;

      /* If the image is being rendered to (written by the PBE) make sure the
       * width is aligned correctly.
       */
      if (image->vk.usage & (VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
                             VK_IMAGE_USAGE_TRANSFER_DST_BIT)) {
         image->physical_extent.width =
            align(image->physical_extent.width, pbe_stride_align);
      }
   }
}

static void pvr_image_setup_mip_levels(struct pvr_image *image)
{
   const uint32_t extent_alignment =
      image->vk.image_type == VK_IMAGE_TYPE_3D ? 4 : 1;
   const unsigned int cpp = vk_format_get_blocksize(image->vk.format);
   VkExtent3D extent =
      vk_image_extent_to_elements(&image->vk, image->physical_extent);

   assert(image->vk.mip_levels <= ARRAY_SIZE(image->mip_levels));

   image->layer_size = 0;

   for (uint32_t i = 0; i < image->vk.mip_levels; i++) {
      struct pvr_mip_level *mip_level = &image->mip_levels[i];

      mip_level->pitch = cpp * align(extent.width, extent_alignment);
      mip_level->height_pitch = align(extent.height, extent_alignment);
      mip_level->size = image->vk.samples * mip_level->pitch *
                        mip_level->height_pitch *
                        align(extent.depth, extent_alignment);
      mip_level->offset = image->layer_size;

      image->layer_size += mip_level->size;

      extent.height = u_minify(extent.height, 1);
      extent.width = u_minify(extent.width, 1);
      extent.depth = u_minify(extent.depth, 1);
   }

   if (image->vk.mip_levels > 1) {
      /* The hw calculates layer strides as if a full mip chain up until 1x1x1
       * were present so we need to account for that in the `layer_size`.
       */
      while (extent.height != 1 || extent.width != 1 || extent.depth != 1) {
         const uint32_t height_pitch = align(extent.height, extent_alignment);
         const uint32_t pitch = cpp * align(extent.width, extent_alignment);

         image->layer_size += image->vk.samples * pitch * height_pitch *
                              align(extent.depth, extent_alignment);

         extent.height = u_minify(extent.height, 1);
         extent.width = u_minify(extent.width, 1);
         extent.depth = u_minify(extent.depth, 1);
      }
   }

   /* TODO: It might be useful to store the alignment in the image so it can be
    * checked (via an assert?) when setting
    * RGX_CR_TPU_TAG_CEM_4K_FACE_PACKING_EN, assuming this is where the
    * requirement comes from.
    */
   if (image->vk.array_layers > 1)
      image->layer_size = align64(image->layer_size, image->alignment);

   image->size = image->layer_size * image->vk.array_layers;
}

static unsigned get_pbe_stride_align(const struct pvr_device_info *dev_info);

VkResult pvr_CreateImage(VkDevice _device,
                         const VkImageCreateInfo *pCreateInfo,
                         const VkAllocationCallbacks *pAllocator,
                         VkImage *pImage)
{
   VK_FROM_HANDLE(pvr_device, device, _device);
   struct pvr_image *image;

   if (wsi_common_is_swapchain_image(pCreateInfo)) {
      return wsi_common_create_swapchain_image(&device->pdevice->wsi_device,
                                               pCreateInfo,
                                               pImage);
   }

   image =
      vk_image_create(&device->vk, pCreateInfo, pAllocator, sizeof(*image));
   if (!image)
      return vk_error(device, VK_ERROR_OUT_OF_HOST_MEMORY);

   /* All images aligned to 4k, in case of arrays/CEM.
    * Refer: pvr_GetImageMemoryRequirements for further details.
    */
   image->alignment = 4096U;

   unsigned pbe_stride_align = get_pbe_stride_align(&device->pdevice->dev_info);

   /* Initialize the image using the saved information from pCreateInfo */
   pvr_image_init_memlayout(image);
   pvr_image_init_physical_extent(image, pbe_stride_align);
   pvr_image_setup_mip_levels(image);

   *pImage = pvr_image_to_handle(image);

   return VK_SUCCESS;
}

void pvr_DestroyImage(VkDevice _device,
                      VkImage _image,
                      const VkAllocationCallbacks *pAllocator)
{
   VK_FROM_HANDLE(pvr_device, device, _device);
   VK_FROM_HANDLE(pvr_image, image, _image);

   if (!image)
      return;

   if (image->vma)
      pvr_unbind_memory(device, image->vma);

   vk_image_destroy(&device->vk, pAllocator, &image->vk);
}

/* clang-format off */
/* Consider a 4 page buffer object.
 *   _________________________________________
 *  |         |          |         |          |
 *  |_________|__________|_________|__________|
 *                  |
 *                  \__ offset (0.5 page size)
 *
 *                  |___size(2 pages)____|
 *
 *            |__VMA size required (3 pages)__|
 *
 *                  |
 *                  \__ returned dev_addr = vma + offset % page_size
 *
 *   VMA size = align(size + offset % page_size, page_size);
 *
 *   Note: the above handling is currently divided between generic
 *   driver code and winsys layer. Given are the details of how this is
 *   being handled.
 *   * As winsys vma allocation interface does not have offset information,
 *     it can not calculate the extra size needed to adjust for the unaligned
 *     offset. So generic code is responsible for allocating a VMA that has
 *     extra space to deal with the above scenario.
 *   * Remaining work of mapping the vma to bo is done by vma_map interface,
 *     as it contains offset information, we don't need to do any adjustments
 *     in the generic code for this part.
 *
 *  TODO: Look into merging heap_alloc and vma_map into single interface.
 */
/* clang-format on */

VkResult pvr_BindImageMemory2(VkDevice _device,
                              uint32_t bindInfoCount,
                              const VkBindImageMemoryInfo *pBindInfos)
{
   VK_FROM_HANDLE(pvr_device, device, _device);
   uint32_t i;

   for (i = 0; i < bindInfoCount; i++) {
      VK_FROM_HANDLE(pvr_device_memory, mem, pBindInfos[i].memory);
      VK_FROM_HANDLE(pvr_image, image, pBindInfos[i].image);
      VkDeviceSize offset = pBindInfos[i].memoryOffset;
      VkResult result;

#if defined(PVR_USE_WSI_PLATFORM)
      const VkBindImageMemorySwapchainInfoKHR *swapchain_info =
         vk_find_struct_const(pBindInfos[i].pNext,
                              BIND_IMAGE_MEMORY_SWAPCHAIN_INFO_KHR);

      if (swapchain_info && swapchain_info->swapchain != VK_NULL_HANDLE) {
         VkDeviceMemory _swapchain_memory =
            wsi_common_get_memory(swapchain_info->swapchain,
                                  swapchain_info->imageIndex);
         VK_FROM_HANDLE(pvr_device_memory, swapchain_memory, _swapchain_memory);

         mem = swapchain_memory;
         offset = 0;
      }
#endif

      result = pvr_bind_memory(device,
                               mem,
                               offset,
                               image->size,
                               image->alignment,
                               &image->vma,
                               &image->dev_addr);
      if (result != VK_SUCCESS) {
         while (i--) {
            VK_FROM_HANDLE(pvr_image, image, pBindInfos[i].image);

            pvr_unbind_memory(device, image->vma);
         }

         return result;
      }
   }

   return VK_SUCCESS;
}

void pvr_get_image_subresource_layout(const struct pvr_image *image,
                                      const VkImageSubresource *subresource,
                                      VkSubresourceLayout *layout)
{
   const struct pvr_mip_level *mip_level =
      &image->mip_levels[subresource->mipLevel];

   pvr_assert(subresource->mipLevel < image->vk.mip_levels);
   pvr_assert(subresource->arrayLayer < image->vk.array_layers);

   layout->offset =
      subresource->arrayLayer * image->layer_size + mip_level->offset;
   layout->rowPitch = mip_level->pitch;
   layout->depthPitch = mip_level->pitch * mip_level->height_pitch;
   layout->arrayPitch = image->layer_size;
   layout->size = mip_level->size;
}

void pvr_GetImageSubresourceLayout(VkDevice device,
                                   VkImage _image,
                                   const VkImageSubresource *subresource,
                                   VkSubresourceLayout *layout)
{
   VK_FROM_HANDLE(pvr_image, image, _image);

   pvr_get_image_subresource_layout(image, subresource, layout);
}

/* Leave this at the very end, to avoid leakage of HW-defs here */
#define PVR_BUILD_ARCH_ROGUE
#include "pvr_csb.h"

static unsigned get_pbe_stride_align(const struct pvr_device_info *dev_info)
{
   return PVR_HAS_FEATURE(dev_info, pbe_stride_align_1pixel)
             ? 1
             : ROGUE_PBESTATE_REG_WORD0_LINESTRIDE_UNIT_SIZE;
}
