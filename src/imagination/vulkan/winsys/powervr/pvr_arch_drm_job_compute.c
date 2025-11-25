/*
 * Copyright © 2022 Imagination Technologies Ltd.
 *
 * SPDX-License-Identifier: MIT
 */

#include "pvr_drm_job_compute.h"

#include "pvr_csb.h"

void PVR_PER_ARCH(drm_compute_ctx_static_state_init)(
   const struct pvr_winsys_compute_ctx_create_info *create_info,
   uint8_t *stream_ptr_start,
   uint32_t *stream_len_ptr)
{
   const struct pvr_winsys_compute_ctx_static_state *ws_static_state =
      &create_info->static_state;
   uint64_t *stream_ptr = (uint64_t *)stream_ptr_start;

   /* Leave space for stream header. */
   stream_ptr += pvr_cmd_length(KMD_STREAM_HDR) / 2;

   *stream_ptr++ = ws_static_state->cdm_ctx_store_pds0;
   *stream_ptr++ = ws_static_state->cdm_ctx_store_pds1;
   *stream_ptr++ = ws_static_state->cdm_ctx_terminate_pds;
   *stream_ptr++ = ws_static_state->cdm_ctx_terminate_pds1;
   *stream_ptr++ = ws_static_state->cdm_ctx_resume_pds0;
   *stream_ptr++ = ws_static_state->cdm_ctx_store_pds0_b;
   *stream_ptr++ = ws_static_state->cdm_ctx_resume_pds0_b;

   *stream_len_ptr = ((uint8_t *)stream_ptr - stream_ptr_start);

   pvr_csb_pack ((uint64_t *)stream_ptr_start, KMD_STREAM_HDR, value) {
      value.length = *stream_len_ptr;
   }
}
