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

#include "pvr_srv_job_compute.h"

#include "fw-api/pvr_rogue_fwif.h"

#include "pvr_csb.h"
#include "pvr_device_info.h"

static uint32_t
pvr_srv_compute_cmd_stream_load(struct rogue_fwif_cmd_compute *const cmd,
                                const uint8_t *const stream,
                                const uint32_t stream_len,
                                const struct pvr_device_info *const dev_info)
{
   const uint32_t *stream_ptr = (const uint32_t *)stream;
   struct rogue_fwif_cdm_regs *const regs = &cmd->regs;
   uint32_t main_stream_len =
      pvr_csb_unpack((uint64_t *)stream_ptr, KMD_STREAM_HDR).length;

   stream_ptr += pvr_cmd_length(KMD_STREAM_HDR);

   regs->tpu_border_colour_table = *(const uint64_t *)stream_ptr;
   stream_ptr += pvr_cmd_length(CR_TPU_BORDER_COLOUR_TABLE_CDM);

   regs->cdm_ctrl_stream_base = *(const uint64_t *)stream_ptr;
   stream_ptr += pvr_cmd_length(CR_CDM_CTRL_STREAM_BASE);

   regs->cdm_context_state_base_addr = *(const uint64_t *)stream_ptr;
   stream_ptr += pvr_cmd_length(CR_CDM_CONTEXT_STATE_BASE);

   regs->cdm_resume_pds1 = *stream_ptr;
   stream_ptr += pvr_cmd_length(CR_CDM_CONTEXT_PDS1);

   if (PVR_HAS_FEATURE(dev_info, compute_morton_capable)) {
      regs->cdm_item = *stream_ptr;
      stream_ptr += pvr_cmd_length(CR_CDM_ITEM);
   }

   if (PVR_HAS_FEATURE(dev_info, cluster_grouping)) {
      regs->compute_cluster = *stream_ptr;
      stream_ptr += pvr_cmd_length(CR_COMPUTE_CLUSTER);
   }

   if (PVR_HAS_FEATURE(dev_info, tpu_dm_global_registers)) {
      regs->tpu_tag_cdm_ctrl = *stream_ptr;
      stream_ptr++;
   }

   if (PVR_HAS_FEATURE(dev_info, gpu_multicore_support)) {
      cmd->execute_count = *stream_ptr;
      stream_ptr++;
   }

   assert((const uint8_t *)stream_ptr - stream <= stream_len);
   assert((const uint8_t *)stream_ptr - stream == main_stream_len);

   return main_stream_len;
}

static void pvr_srv_compute_cmd_ext_stream_load(
   struct rogue_fwif_cmd_compute *const cmd,
   const uint8_t *const stream,
   const uint32_t stream_len,
   const uint32_t ext_stream_offset,
   const struct pvr_device_info *const dev_info)
{
   const uint32_t *ext_stream_ptr =
      (const uint32_t *)((uint8_t *)stream + ext_stream_offset);
   struct rogue_fwif_cdm_regs *const regs = &cmd->regs;

   struct ROGUE_KMD_STREAM_EXTHDR_COMPUTE0 header0;

   header0 = pvr_csb_unpack(ext_stream_ptr, KMD_STREAM_EXTHDR_COMPUTE0);
   ext_stream_ptr += pvr_cmd_length(KMD_STREAM_EXTHDR_COMPUTE0);

   assert(PVR_HAS_QUIRK(dev_info, 49927) == header0.has_brn49927);
   if (header0.has_brn49927) {
      regs->tpu = *ext_stream_ptr;
      ext_stream_ptr += pvr_cmd_length(CR_TPU);
   }

   assert((const uint8_t *)ext_stream_ptr - stream == stream_len);
}

void PVR_PER_ARCH(srv_compute_cmd_init)(
   const struct pvr_winsys_compute_submit_info *submit_info,
   struct rogue_fwif_cmd_compute *cmd,
   const struct pvr_device_info *const dev_info)
{
   uint32_t ext_stream_offset;

   memset(cmd, 0, sizeof(*cmd));

   cmd->cmn.frame_num = submit_info->frame_num;

   ext_stream_offset =
      pvr_srv_compute_cmd_stream_load(cmd,
                                      submit_info->fw_stream,
                                      submit_info->fw_stream_len,
                                      dev_info);

   if (ext_stream_offset < submit_info->fw_stream_len) {
      pvr_srv_compute_cmd_ext_stream_load(cmd,
                                          submit_info->fw_stream,
                                          submit_info->fw_stream_len,
                                          ext_stream_offset,
                                          dev_info);
   }

   if (submit_info->flags.prevent_all_overlap)
      cmd->flags |= ROGUE_FWIF_COMPUTE_FLAG_PREVENT_ALL_OVERLAP;

   if (submit_info->flags.use_single_core)
      cmd->flags |= ROGUE_FWIF_COMPUTE_FLAG_SINGLE_CORE;
}
