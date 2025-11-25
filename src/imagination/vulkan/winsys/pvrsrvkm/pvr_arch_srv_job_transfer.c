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

#include "pvr_srv_job_transfer.h"

#include "fw-api/pvr_rogue_fwif.h"
#include "pvr_csb.h"
#include "pvr_device_info.h"

void PVR_PER_ARCH(srv_transfer_cmd_stream_load)(
   struct rogue_fwif_cmd_transfer *const cmd,
   const uint8_t *const stream,
   const uint32_t stream_len,
   const struct pvr_device_info *const dev_info)
{
   const uint32_t *stream_ptr = (const uint32_t *)stream;
   struct rogue_fwif_transfer_regs *const regs = &cmd->regs;
   uint32_t main_stream_len =
      pvr_csb_unpack((uint64_t *)stream_ptr, KMD_STREAM_HDR).length;

   stream_ptr += pvr_cmd_length(KMD_STREAM_HDR);

   memcpy(&regs->pds_bgnd0_base, stream_ptr, sizeof(regs->pds_bgnd0_base));
   stream_ptr += pvr_cmd_length(CR_PDS_BGRND0_BASE);

   memcpy(&regs->pds_bgnd1_base, stream_ptr, sizeof(regs->pds_bgnd1_base));
   stream_ptr += pvr_cmd_length(CR_PDS_BGRND1_BASE);

   memcpy(&regs->pds_bgnd3_sizeinfo,
          stream_ptr,
          sizeof(regs->pds_bgnd3_sizeinfo));
   stream_ptr += pvr_cmd_length(CR_PDS_BGRND3_SIZEINFO);

   memcpy(&regs->isp_mtile_base, stream_ptr, sizeof(regs->isp_mtile_base));
   stream_ptr += pvr_cmd_length(CR_ISP_MTILE_BASE);

   STATIC_ASSERT(ARRAY_SIZE(regs->pbe_wordx_mrty) == 9U);
   STATIC_ASSERT(sizeof(regs->pbe_wordx_mrty[0]) == sizeof(uint64_t));
   memcpy(regs->pbe_wordx_mrty, stream_ptr, sizeof(regs->pbe_wordx_mrty));
   stream_ptr += 9U * 2U;

   regs->isp_bgobjvals = *stream_ptr;
   stream_ptr += pvr_cmd_length(CR_ISP_BGOBJVALS);

   regs->usc_pixel_output_ctrl = *stream_ptr;
   stream_ptr += pvr_cmd_length(CR_USC_PIXEL_OUTPUT_CTRL);

   regs->usc_clear_register0 = *stream_ptr;
   stream_ptr += pvr_cmd_length(CR_USC_CLEAR_REGISTER);

   regs->usc_clear_register1 = *stream_ptr;
   stream_ptr += pvr_cmd_length(CR_USC_CLEAR_REGISTER);

   regs->usc_clear_register2 = *stream_ptr;
   stream_ptr += pvr_cmd_length(CR_USC_CLEAR_REGISTER);

   regs->usc_clear_register3 = *stream_ptr;
   stream_ptr += pvr_cmd_length(CR_USC_CLEAR_REGISTER);

   regs->isp_mtile_size = *stream_ptr;
   stream_ptr += pvr_cmd_length(CR_ISP_MTILE_SIZE);

   regs->isp_render_origin = *stream_ptr;
   stream_ptr += pvr_cmd_length(CR_ISP_RENDER_ORIGIN);

   regs->isp_ctl = *stream_ptr;
   stream_ptr += pvr_cmd_length(CR_ISP_CTL);

   regs->isp_aa = *stream_ptr;
   stream_ptr += pvr_cmd_length(CR_ISP_AA);

   regs->event_pixel_pds_info = *stream_ptr;
   stream_ptr += pvr_cmd_length(CR_EVENT_PIXEL_PDS_INFO);

   regs->event_pixel_pds_code = *stream_ptr;
   stream_ptr += pvr_cmd_length(CR_EVENT_PIXEL_PDS_CODE);

   regs->event_pixel_pds_data = *stream_ptr;
   stream_ptr += pvr_cmd_length(CR_EVENT_PIXEL_PDS_DATA);

   regs->isp_render = *stream_ptr;
   stream_ptr += pvr_cmd_length(CR_ISP_RENDER);

   regs->isp_rgn = *stream_ptr;
   stream_ptr++;

   if (PVR_HAS_FEATURE(dev_info, gpu_multicore_support)) {
      regs->frag_screen = *stream_ptr;
      stream_ptr++;
   }

   assert((const uint8_t *)stream_ptr - stream == stream_len);
   assert((const uint8_t *)stream_ptr - stream == main_stream_len);
}
