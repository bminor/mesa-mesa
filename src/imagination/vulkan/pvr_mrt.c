/*
 * Copyright © 2025 Imagination Technologies Ltd.
 *
 * SPDX-License-Identifier: MIT
 */

#include "pvr_mrt.h"

#include "vk_log.h"

#include "pvr_device.h"
#include "pvr_formats.h"
#include "pvr_physical_device.h"

#include "hwdef/rogue_hw_utils.h"
#include "util/macros.h"

#include "pvr_mrt.h"

/* Which parts of the output registers/a tile buffer are currently allocated. */
struct pvr_mrt_alloc_mask {
   /* Bit array. A bit is set if the corresponding dword is allocated. */
   BITSET_DECLARE(allocs, 8U);
};

struct pvr_mrt_alloc_ctx {
   /* Which pixel output registers are allocated. */
   struct pvr_mrt_alloc_mask output_reg;

   /* Range of allocated output registers. */
   uint32_t output_regs_count;

   /* Number of tile buffers allocated. */
   uint32_t tile_buffers_count;

   /* Which parts of each tile buffer are allocated. Length is
    * tile_buffers_count.
    */
   struct pvr_mrt_alloc_mask tile_buffers[PVR_MAX_TILE_BUFFER_COUNT];
};

static uint32_t pvr_get_accum_format_bitsize(VkFormat vk_format)
{
   return pvr_get_pbe_accum_format_size_in_bytes(vk_format) * 8;
}

/**
 * Check if there is space in a buffer for storing a render target of a
 * specified size.
 */
static int32_t
pvr_mrt_alloc_from_buffer(const struct pvr_device_info *dev_info,
                          struct pvr_mrt_alloc_mask *buffer,
                          uint32_t pixel_size)
{
   const uint32_t max_out_regs = rogue_get_max_output_regs_per_pixel(dev_info);
   uint32_t alignment = 1U;

   if (PVR_HAS_FEATURE(dev_info, pbe2_in_xe)) {
      /* For a 64-bit/128-bit source format: the start offset must be even. */
      if (pixel_size == 2U || pixel_size == 4U)
         alignment = 2U;
   }

   assert(pixel_size <= max_out_regs);

   for (uint32_t i = 0U; i <= (max_out_regs - pixel_size); i += alignment) {
      if (!BITSET_TEST_RANGE(buffer->allocs, i, i + pixel_size - 1U)) {
         BITSET_SET_RANGE(buffer->allocs, i, i + pixel_size - 1U);
         return i;
      }
   }

   return -1;
}

void
pvr_init_mrt_desc(VkFormat format, struct usc_mrt_desc *desc)
{
   uint32_t pixel_size_in_chunks;
   uint32_t pixel_size_in_bits;

   /* TODO: Add support for packing multiple attachments into the same
    * register
    */
   const uint32_t part_bits = 0;
   if (vk_format_is_color(format) &&
       pvr_get_pbe_accum_format(format) == PVR_PBE_ACCUM_FORMAT_INVALID) {
      /* The VkFormat is not supported as a color attachment so `0`.
       * vulkan doesn't seem to restrict vkCreateRenderPass() to supported
       * formats only.
       */
      pixel_size_in_bits = 0;
   } else {
      /* TODO: handle IMG_PIXFMT_A8_UNORM
       *  For alpha only formats alpha is still placed in channel 3, so channels
       *  0-2 need to be allocated but are left unused
       */
      pixel_size_in_bits = pvr_get_accum_format_bitsize(format);
   }

   desc->intermediate_size = DIV_ROUND_UP(pixel_size_in_bits, CHAR_BIT);

   pixel_size_in_chunks = DIV_ROUND_UP(pixel_size_in_bits, 32U);
   for (uint32_t j = 0U; j < pixel_size_in_chunks; j++)
      desc->valid_mask[j] = ~0;

   if (part_bits > 0U)
      desc->valid_mask[pixel_size_in_chunks] = BITFIELD_MASK(part_bits);
}

static VkResult pvr_alloc_mrt(const struct pvr_device_info *dev_info,
                              struct pvr_mrt_alloc_ctx *alloc,
                              struct usc_mrt_setup *setup,
                              unsigned rt,
                              VkFormat format)
{
   struct usc_mrt_resource *resource = &setup->mrt_resources[rt];

   const uint32_t pixel_size =
      DIV_ROUND_UP(pvr_get_accum_format_bitsize(format), 32U);

   const int32_t output_reg =
      pvr_mrt_alloc_from_buffer(dev_info, &alloc->output_reg, pixel_size);

   if (output_reg != -1) {
      resource->type = USC_MRT_RESOURCE_TYPE_OUTPUT_REG;
      resource->reg.output_reg = output_reg;
      resource->reg.offset = 0;

      alloc->output_regs_count =
         MAX2(alloc->output_regs_count, resource->reg.output_reg + pixel_size);
   } else {
      resource->type = USC_MRT_RESOURCE_TYPE_MEMORY;

      unsigned tib = 0;
      for (; tib < alloc->tile_buffers_count; tib++) {
         struct pvr_mrt_alloc_mask *tib_alloc =
            &alloc->tile_buffers[tib];

         const int32_t tile_buffer_offset =
            pvr_mrt_alloc_from_buffer(dev_info, tib_alloc, pixel_size);

         if (tile_buffer_offset != -1) {
            resource->mem.tile_buffer = tib;
            resource->mem.offset_dw = tile_buffer_offset;
            break;
         }
      }

      if (tib == alloc->tile_buffers_count) {
         if (alloc->tile_buffers_count == PVR_MAX_TILE_BUFFER_COUNT)
            return vk_error(NULL, VK_ERROR_TOO_MANY_OBJECTS);

         resource->mem.tile_buffer = alloc->tile_buffers_count;
         resource->mem.offset_dw = 0;
      }

      /* If needed a new tile buffer than those that were allocated, then wipe
       * it bump the global count.
       */
      if (resource->mem.tile_buffer >= alloc->tile_buffers_count) {
         memset(
            &alloc->tile_buffers[alloc->tile_buffers_count],
            0U,
            sizeof(alloc->tile_buffers[0U]) *
               (resource->mem.tile_buffer + 1U - alloc->tile_buffers_count));
         alloc->tile_buffers_count = resource->mem.tile_buffer + 1U;
      }

      /* The hardware makes the bit depth of the on-chip storage and memory
       * storage the same so make sure the memory storage is large enough to
       * accommodate the largest render target.
       */
      alloc->output_regs_count =
         MAX2(alloc->output_regs_count, resource->mem.offset_dw + pixel_size);
   }

   pvr_init_mrt_desc(format, &resource->mrt_desc);
   resource->intermediate_size = resource->mrt_desc.intermediate_size;

   setup->num_render_targets++;

   return VK_SUCCESS;
}

VkResult
pvr_init_usc_mrt_setup(struct pvr_device *device,
                       uint32_t attachment_count,
                       const VkFormat attachment_formats[attachment_count],
                       struct usc_mrt_setup *setup)
{
   const struct pvr_device_info *dev_info = &device->pdevice->dev_info;
   struct pvr_mrt_alloc_ctx alloc = { 0 };
   VkResult result;

   memset(setup, 0, sizeof(*setup));

   if (!attachment_count)
      goto early_exit;

   setup->mrt_resources =
      vk_alloc(&device->vk.alloc,
               sizeof(*setup->mrt_resources) * attachment_count, 8U,
               VK_SYSTEM_ALLOCATION_SCOPE_OBJECT);
   if (!setup->mrt_resources)
      return vk_error(NULL, VK_ERROR_OUT_OF_HOST_MEMORY);

   for (unsigned i = 0; i < attachment_count; i++) {
      VkFormat att_format = attachment_formats[i];
      assert(att_format != VK_FORMAT_UNDEFINED);

      result = pvr_alloc_mrt(dev_info, &alloc, setup, i, att_format);
      if (result != VK_SUCCESS) {
         result = vk_error(NULL, VK_ERROR_OUT_OF_DEVICE_MEMORY);
         goto fail;
      }
   }

early_exit:
   setup->num_output_regs = util_next_power_of_two(alloc.output_regs_count);
   setup->num_tile_buffers = alloc.tile_buffers_count;
   return VK_SUCCESS;
fail:
   vk_free(&device->vk.alloc, setup->mrt_resources);
   return result;
}

void
pvr_destroy_mrt_setup(const struct pvr_device *device,
                      struct usc_mrt_setup *setup)
{
   if (!setup)
      return;

   vk_free(&device->vk.alloc, setup->mrt_resources);
}
