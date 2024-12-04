/*
 * Copyright © 2024 Collabora Ltd.
 * SPDX-License-Identifier: MIT
 */

#include "panvk_cmd_alloc.h"
#include "panvk_cmd_buffer.h"
#include "panvk_entrypoints.h"

mali_ptr
panvk_per_arch(cmd_prepare_push_uniforms)(struct panvk_cmd_buffer *cmdbuf,
                                          VkPipelineBindPoint ptype)
{
   uint32_t sysvals_sz = ptype == VK_PIPELINE_BIND_POINT_GRAPHICS
                            ? sizeof(struct panvk_graphics_sysvals)
                            : sizeof(struct panvk_compute_sysvals);
   const void *sysvals = ptype == VK_PIPELINE_BIND_POINT_GRAPHICS
                            ? (void *)&cmdbuf->state.gfx.sysvals
                            : (void *)&cmdbuf->state.compute.sysvals;
   struct panfrost_ptr push_uniforms = panvk_cmd_alloc_dev_mem(
      cmdbuf, desc, SYSVALS_PUSH_CONST_BASE + sysvals_sz, 16);

   if (push_uniforms.gpu) {
      if (ptype == VK_PIPELINE_BIND_POINT_GRAPHICS)
         cmdbuf->state.gfx.sysvals.push_consts = push_uniforms.gpu;
      else
         cmdbuf->state.compute.sysvals.push_consts = push_uniforms.gpu;

      /* The first half is used for push constants. */
      memcpy(push_uniforms.cpu, cmdbuf->state.push_constants.data,
             sizeof(cmdbuf->state.push_constants.data));

      /* The second half is used for sysvals. */
      memcpy((uint8_t *)push_uniforms.cpu + SYSVALS_PUSH_CONST_BASE, sysvals,
             sysvals_sz);
   }

   return push_uniforms.gpu;
}

VKAPI_ATTR void VKAPI_CALL
panvk_per_arch(CmdPushConstants2KHR)(
   VkCommandBuffer commandBuffer,
   const VkPushConstantsInfoKHR *pPushConstantsInfo)
{
   VK_FROM_HANDLE(panvk_cmd_buffer, cmdbuf, commandBuffer);

   if (pPushConstantsInfo->stageFlags & VK_SHADER_STAGE_ALL_GRAPHICS)
      gfx_state_set_dirty(cmdbuf, PUSH_UNIFORMS);

   if (pPushConstantsInfo->stageFlags & VK_SHADER_STAGE_COMPUTE_BIT)
      compute_state_set_dirty(cmdbuf, PUSH_UNIFORMS);

   memcpy(cmdbuf->state.push_constants.data + pPushConstantsInfo->offset,
          pPushConstantsInfo->pValues, pPushConstantsInfo->size);
}
