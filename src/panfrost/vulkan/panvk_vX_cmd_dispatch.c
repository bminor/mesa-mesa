/*
 * Copyright © 2024 Collabora Ltd.
 * Copyright © 2024 Arm Ltd.
 *
 * SPDX-License-Identifier: MIT
 */

#include "panvk_cmd_buffer.h"
#include "panvk_cmd_dispatch.h"

void
panvk_per_arch(cmd_prepare_dispatch_sysvals)(
   struct panvk_cmd_buffer *cmdbuf, const struct panvk_dispatch_info *info)
{
   struct panvk_compute_sysvals *sysvals = &cmdbuf->state.compute.sysvals;
   const struct panvk_shader *shader = cmdbuf->state.compute.shader;

   /* In indirect case, some sysvals are read from the indirect dispatch
    * buffer.
    */
   if (info->indirect.buffer_dev_addr == 0) {
      sysvals->base.x = info->direct.wg_base.x;
      sysvals->base.y = info->direct.wg_base.y;
      sysvals->base.z = info->direct.wg_base.z;
      sysvals->num_work_groups.x = info->direct.wg_count.x;
      sysvals->num_work_groups.y = info->direct.wg_count.y;
      sysvals->num_work_groups.z = info->direct.wg_count.z;
   }

   sysvals->local_group_size.x = shader->local_size.x;
   sysvals->local_group_size.y = shader->local_size.y;
   sysvals->local_group_size.z = shader->local_size.z;

#if PAN_ARCH <= 7
   struct panvk_descriptor_state *desc_state =
      &cmdbuf->state.compute.desc_state;
   struct panvk_shader_desc_state *cs_desc_state =
      &cmdbuf->state.compute.cs.desc;

   if (compute_state_dirty(cmdbuf, CS) ||
       compute_state_dirty(cmdbuf, DESC_STATE)) {
      sysvals->desc.sets[PANVK_DESC_TABLE_CS_DYN_SSBOS] =
         cs_desc_state->dyn_ssbos;
   }

   for (uint32_t i = 0; i < MAX_SETS; i++) {
      if (shader->desc_info.used_set_mask & BITFIELD_BIT(i))
         sysvals->desc.sets[i] = desc_state->sets[i]->descs.dev;
   }
#endif

   /* We unconditionally update the sysvals, so push_uniforms is always dirty. */
   compute_state_set_dirty(cmdbuf, PUSH_UNIFORMS);
}
