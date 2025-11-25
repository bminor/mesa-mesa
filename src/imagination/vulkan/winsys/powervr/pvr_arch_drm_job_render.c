/*
 * Copyright © 2022 Imagination Technologies Ltd.
 *
 * SPDX-License-Identifier: MIT
 */

#include "pvr_drm_job_render.h"

#include "pvr_csb.h"

void PVR_PER_ARCH(drm_render_ctx_static_state_init)(
   struct pvr_winsys_render_ctx_create_info *create_info,
   uint8_t *stream_ptr_start,
   uint32_t *stream_len_ptr)
{
   struct pvr_winsys_render_ctx_static_state *ws_static_state =
      &create_info->static_state;
   uint64_t *stream_ptr = (uint64_t *)stream_ptr_start;

   /* Leave space for stream header. */
   stream_ptr += pvr_cmd_length(KMD_STREAM_HDR) / 2;

   *stream_ptr++ = ws_static_state->vdm_ctx_state_base_addr;
   /* geom_reg_vdm_context_state_resume_addr is unused and zeroed. */
   *stream_ptr++ = 0;
   *stream_ptr++ = ws_static_state->geom_ctx_state_base_addr;

   for (uint32_t i = 0; i < ARRAY_SIZE(ws_static_state->geom_state); i++) {
      *stream_ptr++ = ws_static_state->geom_state[i].vdm_ctx_store_task0;
      *stream_ptr++ = ws_static_state->geom_state[i].vdm_ctx_store_task1;
      *stream_ptr++ = ws_static_state->geom_state[i].vdm_ctx_store_task2;
      /* {store, resume}_task{3, 4} are unused and zeroed. */
      *stream_ptr++ = 0;
      *stream_ptr++ = 0;

      *stream_ptr++ = ws_static_state->geom_state[i].vdm_ctx_resume_task0;
      *stream_ptr++ = ws_static_state->geom_state[i].vdm_ctx_resume_task1;
      *stream_ptr++ = ws_static_state->geom_state[i].vdm_ctx_resume_task2;
      /* {store, resume}_task{3, 4} are unused and zeroed. */
      *stream_ptr++ = 0;
      *stream_ptr++ = 0;
   }

   *stream_len_ptr = ((uint8_t *)stream_ptr - stream_ptr_start);

   pvr_csb_pack ((uint64_t *)stream_ptr_start, KMD_STREAM_HDR, value) {
      value.length = *stream_len_ptr;
   }
}
