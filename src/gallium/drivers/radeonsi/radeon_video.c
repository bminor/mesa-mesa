/**************************************************************************
 *
 * Copyright 2013 Advanced Micro Devices, Inc.
 *
 * SPDX-License-Identifier: MIT
 *
 **************************************************************************/

#include "radeon_video.h"

#include "radeon_vce.h"
#include "radeonsi/si_pipe.h"
#include "util/u_memory.h"
#include "util/u_video.h"
#include "vl/vl_defines.h"
#include "vl/vl_video_buffer.h"
#include "ac_uvd_dec.h"

#include <unistd.h>

/* generate an stream handle */
unsigned si_vid_alloc_stream_handle()
{
   static struct ac_uvd_stream_handle stream_handle;
   if (!stream_handle.base)
      ac_uvd_init_stream_handle(&stream_handle);
   return ac_uvd_alloc_stream_handle(&stream_handle);
}

/* create a buffer in the winsys */
bool si_vid_create_buffer(struct pipe_screen *screen, struct rvid_buffer *buffer, unsigned size,
                          unsigned usage)
{
   memset(buffer, 0, sizeof(*buffer));
   buffer->usage = usage;

   /* Hardware buffer placement restrictions require the kernel to be
    * able to move buffers around individually, so request a
    * non-sub-allocated buffer.
    */
   buffer->res = si_resource(pipe_buffer_create(screen, PIPE_BIND_CUSTOM, usage, size));

   return buffer->res != NULL;
}

/* create a tmz buffer in the winsys */
bool si_vid_create_tmz_buffer(struct pipe_screen *screen, struct rvid_buffer *buffer, unsigned size,
                              unsigned usage)
{
   memset(buffer, 0, sizeof(*buffer));
   buffer->usage = usage;
   buffer->res = si_resource(pipe_buffer_create(screen, PIPE_BIND_CUSTOM | PIPE_BIND_PROTECTED,
                                                usage, size));
   return buffer->res != NULL;
}


/* destroy a buffer */
void si_vid_destroy_buffer(struct rvid_buffer *buffer)
{
   si_resource_reference(&buffer->res, NULL);
}

/* reallocate a buffer, preserving its content */
bool si_vid_resize_buffer(struct pipe_context *context,
                          struct si_resource **buf, unsigned new_size,
                          struct rvid_buf_offset_info *buf_ofst_info)
{
   struct si_context *sctx = (struct si_context *)context;
   struct si_screen *sscreen = (struct si_screen *)context->screen;
   struct radeon_winsys *ws = sscreen->ws;
   struct si_resource *new_buf = *buf;
   unsigned bytes = MIN2(new_buf->buf->size, new_size);
   struct si_resource *old_buf = new_buf;
   void *src = NULL, *dst = NULL;

   new_buf = si_resource(pipe_buffer_create(context->screen, old_buf->b.b.bind, old_buf->b.b.usage, new_size));
   if (!new_buf)
      goto error;

   if (old_buf->b.b.usage == PIPE_USAGE_STAGING) {
      src = ws->buffer_map(ws, old_buf->buf, NULL, PIPE_MAP_READ | RADEON_MAP_TEMPORARY);
      if (!src)
         goto error;

      dst = ws->buffer_map(ws, new_buf->buf, NULL, PIPE_MAP_WRITE | RADEON_MAP_TEMPORARY);
      if (!dst)
         goto error;

      if (buf_ofst_info) {
         memset(dst, 0, new_size);
         for(int i =0; i < buf_ofst_info->num_units; i++) {
             memcpy(dst, src, buf_ofst_info->old_offset);
             dst += buf_ofst_info->new_offset;
             src += buf_ofst_info->old_offset;
         }
      } else {
         memcpy(dst, src, bytes);
         if (new_size > bytes) {
            new_size -= bytes;
            dst += bytes;
            memset(dst, 0, new_size);
         }
      }
      ws->buffer_unmap(ws, new_buf->buf);
      ws->buffer_unmap(ws, old_buf->buf);
   } else {
      si_barrier_before_simple_buffer_op(sctx, 0, &new_buf->b.b, &old_buf->b.b);
      if (buf_ofst_info) {
         uint64_t dst_offset = 0, src_offset = 0;
         for (int i = 0; i < buf_ofst_info->num_units; i++) {
            si_copy_buffer(sctx, &new_buf->b.b, &old_buf->b.b,
                           dst_offset, src_offset, buf_ofst_info->old_offset);
            dst_offset += buf_ofst_info->new_offset;
            src_offset += buf_ofst_info->old_offset;
         }
      } else {
         bytes = MIN2(new_buf->b.b.width0, old_buf->b.b.width0);
         si_copy_buffer(sctx, &new_buf->b.b, &old_buf->b.b, 0, 0, bytes);
      }
      context->flush(context, NULL, 0);
   }

   si_resource_reference(&old_buf, NULL);
   *buf = new_buf;
   return true;

error:
   if (src)
      ws->buffer_unmap(ws, old_buf->buf);
   si_resource_reference(&new_buf, NULL);
   return false;
}

/* clear the buffer with zeros */
void si_vid_clear_buffer(struct pipe_context *context, struct rvid_buffer *buffer)
{
   struct si_context *sctx = (struct si_context *)context;
   uint32_t zero = 0;

   sctx->b.clear_buffer(&sctx->b, &buffer->res->b.b, 0, buffer->res->b.b.width0, &zero, 4);
   context->flush(context, NULL, 0);
}
