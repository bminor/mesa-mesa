/**************************************************************************
 *
 * Copyright 2018 Advanced Micro Devices, Inc.
 *
 * SPDX-License-Identifier: MIT
 *
 **************************************************************************/

#include "radeon_uvd_enc.h"

#include "pipe/p_video_codec.h"
#include "radeon_video.h"
#include "radeonsi/si_pipe.h"
#include "util/u_memory.h"
#include "util/u_video.h"
#include "vl/vl_video_buffer.h"

#include <stdio.h>

static void radeon_uvd_enc_get_param(struct radeon_uvd_encoder *enc,
                                     struct pipe_h265_enc_picture_desc *pic)
{
   enc->enc_pic.desc = pic;
   enc->enc_pic.picture_type = pic->picture_type;
   enc->enc_pic.nal_unit_type = pic->pic.nal_unit_type;
   enc->enc_pic.temporal_id = pic->pic.temporal_id;
   enc->enc_pic.is_iframe = (pic->picture_type == PIPE_H2645_ENC_PICTURE_TYPE_IDR) ||
                            (pic->picture_type == PIPE_H2645_ENC_PICTURE_TYPE_I);
   enc->enc_pic.enc_params.reference_picture_index =
      pic->ref_list0[0] == PIPE_H2645_LIST_REF_INVALID_ENTRY ? 0xffffffff : pic->ref_list0[0];
   enc->enc_pic.enc_params.reconstructed_picture_index = pic->dpb_curr_pic;

   if (pic->seq.conformance_window_flag) {
         enc->enc_pic.crop_left = pic->seq.conf_win_left_offset;
         enc->enc_pic.crop_right = pic->seq.conf_win_right_offset;
         enc->enc_pic.crop_top = pic->seq.conf_win_top_offset;
         enc->enc_pic.crop_bottom = pic->seq.conf_win_bottom_offset;
   } else {
         enc->enc_pic.crop_left = 0;
         enc->enc_pic.crop_right = 0;
         enc->enc_pic.crop_top = 0;
         enc->enc_pic.crop_bottom = 0;
   }
}

static int flush(struct radeon_uvd_encoder *enc, unsigned flags, struct pipe_fence_handle **fence)
{
   return enc->ws->cs_flush(&enc->cs, flags, fence);
}

static void radeon_uvd_enc_flush(struct pipe_video_codec *encoder)
{
   struct radeon_uvd_encoder *enc = (struct radeon_uvd_encoder *)encoder;
   flush(enc, PIPE_FLUSH_ASYNC, NULL);
}

static void radeon_uvd_enc_cs_flush(void *ctx, unsigned flags, struct pipe_fence_handle **fence)
{
   // just ignored
}

static uint32_t setup_dpb(struct radeon_uvd_encoder *enc, uint32_t num_reconstructed_pictures)
{
   uint32_t i;
   uint32_t alignment = 256;
   uint32_t aligned_width = align(enc->base.width, 64);
   uint32_t aligned_height = align(enc->base.height, 16);
   uint32_t pitch = align(aligned_width, alignment);
   uint32_t luma_size = align(pitch * MAX2(256, aligned_height), alignment);
   uint32_t chroma_size = align(luma_size / 2, alignment);
   uint32_t offset = 0;

   assert(num_reconstructed_pictures <= RENC_UVD_MAX_NUM_RECONSTRUCTED_PICTURES);

   enc->enc_pic.ctx_buf.rec_luma_pitch = pitch;
   enc->enc_pic.ctx_buf.rec_chroma_pitch = pitch;
   enc->enc_pic.ctx_buf.num_reconstructed_pictures = num_reconstructed_pictures;

   for (i = 0; i < num_reconstructed_pictures; i++) {
      enc->enc_pic.ctx_buf.reconstructed_pictures[i].luma_offset = offset;
      offset += luma_size;
      enc->enc_pic.ctx_buf.reconstructed_pictures[i].chroma_offset = offset;
      offset += chroma_size;
   }

   enc->dpb_slots = num_reconstructed_pictures;

   return offset;
}

static void radeon_uvd_enc_begin_frame(struct pipe_video_codec *encoder,
                                       struct pipe_video_buffer *source,
                                       struct pipe_picture_desc *picture)
{
   struct radeon_uvd_encoder *enc = (struct radeon_uvd_encoder *)encoder;
   struct vl_video_buffer *vid_buf = (struct vl_video_buffer *)source;
   struct pipe_h265_enc_picture_desc *pic = (struct pipe_h265_enc_picture_desc *)picture;

   radeon_uvd_enc_get_param(enc, pic);

   enc->get_buffer(vid_buf->resources[0], &enc->handle, &enc->luma);
   enc->get_buffer(vid_buf->resources[1], NULL, &enc->chroma);

   enc->need_feedback = false;

   unsigned dpb_slots = MAX2(pic->seq.sps_max_dec_pic_buffering_minus1[0] + 1, pic->dpb_size);

   if (enc->dpb_slots < dpb_slots) {
      uint32_t dpb_size = setup_dpb(enc, dpb_slots);
      if (!enc->dpb.res) {
         if (!si_vid_create_buffer(enc->screen, &enc->dpb, dpb_size, PIPE_USAGE_DEFAULT)) {
            RVID_ERR("Can't create DPB buffer.\n");
            return;
         }
      } else if (!si_vid_resize_buffer(enc->base.context, &enc->cs, &enc->dpb, dpb_size, NULL)) {
         RVID_ERR("Can't resize DPB buffer.\n");
         return;
      }
   }

   if (!enc->stream_handle) {
      struct rvid_buffer fb;
      enc->stream_handle = si_vid_alloc_stream_handle();
      enc->si = CALLOC_STRUCT(rvid_buffer);
      si_vid_create_buffer(enc->screen, enc->si, 128 * 1024, PIPE_USAGE_DEFAULT);
      si_vid_create_buffer(enc->screen, &fb, 4096, PIPE_USAGE_STAGING);
      enc->fb = &fb;
      enc->begin(enc, picture);
      flush(enc, PIPE_FLUSH_ASYNC, NULL);
      si_vid_destroy_buffer(&fb);
   }
}

static void radeon_uvd_enc_encode_bitstream(struct pipe_video_codec *encoder,
                                            struct pipe_video_buffer *source,
                                            struct pipe_resource *destination, void **fb)
{
   struct radeon_uvd_encoder *enc = (struct radeon_uvd_encoder *)encoder;
   enc->get_buffer(destination, &enc->bs_handle, NULL);
   enc->bs_size = destination->width0;

   *fb = enc->fb = CALLOC_STRUCT(rvid_buffer);

   if (!si_vid_create_buffer(enc->screen, enc->fb, 4096, PIPE_USAGE_STAGING)) {
      RVID_ERR("Can't create feedback buffer.\n");
      return;
   }

   enc->need_feedback = true;
   enc->encode(enc);
}

static int radeon_uvd_enc_end_frame(struct pipe_video_codec *encoder,
                                     struct pipe_video_buffer *source,
                                     struct pipe_picture_desc *picture)
{
   struct radeon_uvd_encoder *enc = (struct radeon_uvd_encoder *)encoder;
   return flush(enc, picture->flush_flags, picture->fence);
}

static void radeon_uvd_enc_destroy(struct pipe_video_codec *encoder)
{
   struct radeon_uvd_encoder *enc = (struct radeon_uvd_encoder *)encoder;

   if (enc->stream_handle) {
      struct rvid_buffer fb;
      enc->need_feedback = false;
      si_vid_create_buffer(enc->screen, &fb, 512, PIPE_USAGE_STAGING);
      enc->fb = &fb;
      enc->destroy(enc);
      flush(enc, PIPE_FLUSH_ASYNC, NULL);
      if (enc->si) {
         si_vid_destroy_buffer(enc->si);
         FREE(enc->si);
      }
      si_vid_destroy_buffer(&fb);
   }

   if (enc->dpb.res)
      si_vid_destroy_buffer(&enc->dpb);
   enc->ws->cs_destroy(&enc->cs);
   FREE(enc);
}

static void radeon_uvd_enc_get_feedback(struct pipe_video_codec *encoder, void *feedback,
                                        unsigned *size, struct pipe_enc_feedback_metadata* metadata)
{
   struct radeon_uvd_encoder *enc = (struct radeon_uvd_encoder *)encoder;
   struct rvid_buffer *fb = feedback;

   if (NULL != size) {
      radeon_uvd_enc_feedback_t *fb_data = (radeon_uvd_enc_feedback_t *)enc->ws->buffer_map(
         enc->ws, fb->res->buf, &enc->cs, PIPE_MAP_READ_WRITE | RADEON_MAP_TEMPORARY);

      if (!fb_data->status)
         *size = fb_data->bitstream_size;
      else
         *size = 0;
      enc->ws->buffer_unmap(enc->ws, fb->res->buf);
   }

   si_vid_destroy_buffer(fb);
   FREE(fb);
}

static int radeon_uvd_enc_fence_wait(struct pipe_video_codec *encoder,
                                     struct pipe_fence_handle *fence,
                                     uint64_t timeout)
{
   struct radeon_uvd_encoder *enc = (struct radeon_uvd_encoder *)encoder;

   return enc->ws->fence_wait(enc->ws, fence, timeout);
}

static void radeon_uvd_enc_destroy_fence(struct pipe_video_codec *encoder,
                                         struct pipe_fence_handle *fence)
{
   struct radeon_uvd_encoder *enc = (struct radeon_uvd_encoder *)encoder;

   enc->ws->fence_reference(enc->ws, &fence, NULL);
}

struct pipe_video_codec *radeon_uvd_create_encoder(struct pipe_context *context,
                                                   const struct pipe_video_codec *templ,
                                                   struct radeon_winsys *ws,
                                                   radeon_uvd_enc_get_buffer get_buffer)
{
   struct si_screen *sscreen = (struct si_screen *)context->screen;
   struct si_context *sctx = (struct si_context *)context;
   struct radeon_uvd_encoder *enc;

   if (!si_radeon_uvd_enc_supported(sscreen)) {
      RVID_ERR("Unsupported UVD ENC fw version loaded!\n");
      return NULL;
   }

   enc = CALLOC_STRUCT(radeon_uvd_encoder);

   if (!enc)
      return NULL;

   enc->base = *templ;
   enc->base.context = context;
   enc->base.destroy = radeon_uvd_enc_destroy;
   enc->base.begin_frame = radeon_uvd_enc_begin_frame;
   enc->base.encode_bitstream = radeon_uvd_enc_encode_bitstream;
   enc->base.end_frame = radeon_uvd_enc_end_frame;
   enc->base.flush = radeon_uvd_enc_flush;
   enc->base.get_feedback = radeon_uvd_enc_get_feedback;
   enc->base.fence_wait = radeon_uvd_enc_fence_wait;
   enc->base.destroy_fence = radeon_uvd_enc_destroy_fence;
   enc->get_buffer = get_buffer;
   enc->screen = context->screen;
   enc->ws = ws;

   if (!ws->cs_create(&enc->cs, sctx->ctx, AMD_IP_UVD_ENC, radeon_uvd_enc_cs_flush, enc)) {
      RVID_ERR("Can't get command submission context.\n");
      goto error;
   }

   radeon_uvd_enc_1_1_init(enc);

   return &enc->base;

error:
   enc->ws->cs_destroy(&enc->cs);

   FREE(enc);
   return NULL;
}

bool si_radeon_uvd_enc_supported(struct si_screen *sscreen)
{
   return sscreen->info.ip[AMD_IP_UVD_ENC].num_queues;
}
