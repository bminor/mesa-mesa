/**************************************************************************
 *
 * Copyright 2010 Thomas Balling Sørensen & Orasanu Lucian.
 * Copyright 2014 Advanced Micro Devices, Inc.
 * All Rights Reserved.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the
 * "Software"), to deal in the Software without restriction, including
 * without limitation the rights to use, copy, modify, merge, publish,
 * distribute, sub license, and/or sell copies of the Software, and to
 * permit persons to whom the Software is furnished to do so, subject to
 * the following conditions:
 *
 * The above copyright notice and this permission notice (including the
 * next paragraph) shall be included in all copies or substantial portions
 * of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS
 * OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NON-INFRINGEMENT.
 * IN NO EVENT SHALL THE COPYRIGHT HOLDER(S) OR AUTHOR(S) BE LIABLE FOR
 * ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,
 * TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE
 * SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 *
 **************************************************************************/

#include "pipe/p_screen.h"
#include "frontend/drm_driver.h"
#include "util/u_memory.h"
#include "util/u_handle_table.h"
#include "util/u_transfer.h"
#include "util/set.h"
#include "vl/vl_winsys.h"

#include "va_private.h"

#ifdef _WIN32
#include <va/va_win32.h>
#endif

#ifndef VA_MAPBUFFER_FLAG_DEFAULT
#define VA_MAPBUFFER_FLAG_DEFAULT 0
#define VA_MAPBUFFER_FLAG_READ    1
#define VA_MAPBUFFER_FLAG_WRITE   2
#endif

VAStatus
vlVaCreateBuffer(VADriverContextP ctx, VAContextID context, VABufferType type,
                 unsigned int size, unsigned int num_elements, void *data,
                 VABufferID *buf_id)
{
   vlVaDriver *drv;
   vlVaBuffer *buf;

   if (!ctx)
      return VA_STATUS_ERROR_INVALID_CONTEXT;

   buf = CALLOC(1, sizeof(vlVaBuffer));
   if (!buf)
      return VA_STATUS_ERROR_ALLOCATION_FAILED;

   buf->type = type;
   buf->size = size;
   buf->num_elements = num_elements;

   if (buf->type == VAEncCodedBufferType)
      buf->data = CALLOC(1, sizeof(VACodedBufferSegment));
   else
      buf->data = MALLOC((size_t)size * num_elements);

   if (!buf->data) {
      FREE(buf);
      return VA_STATUS_ERROR_ALLOCATION_FAILED;
   }

   if (data)
      memcpy(buf->data, data, (size_t)size * num_elements);

   drv = VL_VA_DRIVER(ctx);
   mtx_lock(&drv->mutex);
   *buf_id = handle_table_add(drv->htab, buf);
   mtx_unlock(&drv->mutex);

   return VA_STATUS_SUCCESS;
}

VAStatus
vlVaBufferSetNumElements(VADriverContextP ctx, VABufferID buf_id,
                         unsigned int num_elements)
{
   vlVaDriver *drv;
   vlVaBuffer *buf;

   if (!ctx)
      return VA_STATUS_ERROR_INVALID_CONTEXT;

   drv = VL_VA_DRIVER(ctx);
   mtx_lock(&drv->mutex);
   buf = handle_table_get(drv->htab, buf_id);
   mtx_unlock(&drv->mutex);
   if (!buf)
      return VA_STATUS_ERROR_INVALID_BUFFER;

   if (buf->derived_surface.resource)
      return VA_STATUS_ERROR_INVALID_BUFFER;

   buf->data = REALLOC(buf->data, buf->size * buf->num_elements,
                       buf->size * num_elements);
   buf->num_elements = num_elements;

   if (!buf->data)
      return VA_STATUS_ERROR_ALLOCATION_FAILED;

   return VA_STATUS_SUCCESS;
}

VAStatus
vlVaMapBuffer(VADriverContextP ctx, VABufferID buf_id, void **pbuff)
{
   return vlVaMapBuffer2(ctx, buf_id, pbuff, VA_MAPBUFFER_FLAG_DEFAULT);
}

VAStatus vlVaMapBuffer2(VADriverContextP ctx, VABufferID buf_id,
                        void **pbuff, uint32_t flags)
{
   vlVaDriver *drv;
   vlVaBuffer *buf;

   if (!ctx)
      return VA_STATUS_ERROR_INVALID_CONTEXT;

   drv = VL_VA_DRIVER(ctx);
   if (!drv)
      return VA_STATUS_ERROR_INVALID_CONTEXT;

   if (!pbuff)
      return VA_STATUS_ERROR_INVALID_PARAMETER;

   mtx_lock(&drv->mutex);
   buf = handle_table_get(drv->htab, buf_id);
   if (!buf || buf->export_refcount > 0) {
      mtx_unlock(&drv->mutex);
      return VA_STATUS_ERROR_INVALID_BUFFER;
   }

   if (buf->type == VAEncCodedBufferType)
      vlVaGetBufferFeedback(buf);

   if (buf->derived_surface.resource) {
      struct pipe_resource *resource;
      struct pipe_box box;
      unsigned usage = 0;
      void *(*map_func)(struct pipe_context *,
             struct pipe_resource *resource,
             unsigned level,
             unsigned usage,  /* a combination of PIPE_MAP_x */
             const struct pipe_box *,
             struct pipe_transfer **out_transfer);

      memset(&box, 0, sizeof(box));
      resource = buf->derived_surface.resource;
      box.width = resource->width0;
      box.height = resource->height0;
      box.depth = resource->depth0;

      if (resource->target == PIPE_BUFFER)
         map_func = drv->pipe->buffer_map;
      else
         map_func = drv->pipe->texture_map;

      if (flags == VA_MAPBUFFER_FLAG_DEFAULT) {
         /* For VAImageBufferType, use PIPE_MAP_WRITE for now,
          * PIPE_MAP_READ_WRITE degradate perf with two copies when map/unmap. */
         if (buf->type == VAEncCodedBufferType)
            usage = PIPE_MAP_READ;
         else
            usage = PIPE_MAP_WRITE;

         /* Map decoder and postproc surfaces also for reading. */
         if (buf->derived_surface.entrypoint == PIPE_VIDEO_ENTRYPOINT_BITSTREAM ||
             buf->derived_surface.entrypoint == PIPE_VIDEO_ENTRYPOINT_PROCESSING)
            usage |= PIPE_MAP_READ;
      }

      if (flags & VA_MAPBUFFER_FLAG_READ)
         usage |= PIPE_MAP_READ;
      if (flags & VA_MAPBUFFER_FLAG_WRITE)
         usage |= PIPE_MAP_WRITE;

      assert(usage);

      *pbuff = map_func(drv->pipe, resource, 0, usage,
                        &box, &buf->derived_surface.transfer);
      mtx_unlock(&drv->mutex);

      if (!buf->derived_surface.transfer || !*pbuff)
         return VA_STATUS_ERROR_INVALID_BUFFER;

      if (buf->type == VAEncCodedBufferType) {
         VACodedBufferSegment* curr_buf_ptr = (VACodedBufferSegment*) buf->data;

         if ((buf->extended_metadata.present_metadata & PIPE_VIDEO_FEEDBACK_METADATA_TYPE_ENCODE_RESULT) &&
             (buf->extended_metadata.encode_result & PIPE_VIDEO_FEEDBACK_METADATA_ENCODE_FLAG_FAILED)) {
            curr_buf_ptr->status = VA_CODED_BUF_STATUS_BAD_BITSTREAM;
            return VA_STATUS_ERROR_OPERATION_FAILED;
         }

         curr_buf_ptr->status = (buf->extended_metadata.average_frame_qp & VA_CODED_BUF_STATUS_PICTURE_AVE_QP_MASK);
         if (buf->extended_metadata.encode_result & PIPE_VIDEO_FEEDBACK_METADATA_ENCODE_FLAG_MAX_FRAME_SIZE_OVERFLOW)
            curr_buf_ptr->status |= VA_CODED_BUF_STATUS_FRAME_SIZE_OVERFLOW;

         if ((buf->extended_metadata.present_metadata & PIPE_VIDEO_FEEDBACK_METADATA_TYPE_CODEC_UNIT_LOCATION) == 0) {
            curr_buf_ptr->buf = *pbuff;
            curr_buf_ptr->size = buf->coded_size;
            *pbuff = buf->data;
         } else {
            uint8_t* compressed_bitstream_data = *pbuff;
            *pbuff = buf->data;

            for (size_t i = 0; i < buf->extended_metadata.codec_unit_metadata_count - 1; i++) {
               if (!curr_buf_ptr->next)
                  curr_buf_ptr->next = CALLOC(1, sizeof(VACodedBufferSegment));
               if (!curr_buf_ptr->next)
                  return VA_STATUS_ERROR_ALLOCATION_FAILED;
               curr_buf_ptr = curr_buf_ptr->next;
            }
            if (curr_buf_ptr->next) {
               VACodedBufferSegment *node = curr_buf_ptr->next;
               while (node) {
                  VACodedBufferSegment *next = node->next;
                  FREE(node);
                  node = next;
               }
            }
            curr_buf_ptr->next = NULL;

            curr_buf_ptr = buf->data;
            for (size_t i = 0; i < buf->extended_metadata.codec_unit_metadata_count; i++) {
               curr_buf_ptr->size = buf->extended_metadata.codec_unit_metadata[i].size;
               curr_buf_ptr->buf = compressed_bitstream_data + buf->extended_metadata.codec_unit_metadata[i].offset;
               if (buf->extended_metadata.codec_unit_metadata[i].flags & PIPE_VIDEO_CODEC_UNIT_LOCATION_FLAG_MAX_SLICE_SIZE_OVERFLOW)
                  curr_buf_ptr->status |= VA_CODED_BUF_STATUS_SLICE_OVERFLOW_MASK;
               if (buf->extended_metadata.codec_unit_metadata[i].flags & PIPE_VIDEO_CODEC_UNIT_LOCATION_FLAG_SINGLE_NALU)
                  curr_buf_ptr->status |= VA_CODED_BUF_STATUS_SINGLE_NALU;

               curr_buf_ptr = curr_buf_ptr->next;
            }
         }
      }
   } else {
      mtx_unlock(&drv->mutex);
      *pbuff = buf->data;
   }

   return VA_STATUS_SUCCESS;
}

VAStatus
vlVaUnmapBuffer(VADriverContextP ctx, VABufferID buf_id)
{
   vlVaDriver *drv;
   vlVaBuffer *buf;
   struct pipe_resource *resource;

   if (!ctx)
      return VA_STATUS_ERROR_INVALID_CONTEXT;

   drv = VL_VA_DRIVER(ctx);
   if (!drv)
      return VA_STATUS_ERROR_INVALID_CONTEXT;

   mtx_lock(&drv->mutex);
   buf = handle_table_get(drv->htab, buf_id);
   if (!buf || buf->export_refcount > 0) {
      mtx_unlock(&drv->mutex);
      return VA_STATUS_ERROR_INVALID_BUFFER;
   }

   resource = buf->derived_surface.resource;
   if (resource) {
      void (*unmap_func)(struct pipe_context *pipe,
                         struct pipe_transfer *transfer);

      if (!buf->derived_surface.transfer) {
         mtx_unlock(&drv->mutex);
         return VA_STATUS_ERROR_INVALID_BUFFER;
      }

      if (resource->target == PIPE_BUFFER)
         unmap_func = pipe_buffer_unmap;
      else
         unmap_func = pipe_texture_unmap;

      unmap_func(drv->pipe, buf->derived_surface.transfer);
      buf->derived_surface.transfer = NULL;

      if (buf->type == VAImageBufferType)
         drv->pipe->flush(drv->pipe, NULL, 0);
   }
   mtx_unlock(&drv->mutex);

   return VA_STATUS_SUCCESS;
}

VAStatus
vlVaDestroyBuffer(VADriverContextP ctx, VABufferID buf_id)
{
   vlVaDriver *drv;
   vlVaBuffer *buf;

   if (!ctx)
      return VA_STATUS_ERROR_INVALID_CONTEXT;

   drv = VL_VA_DRIVER(ctx);
   mtx_lock(&drv->mutex);
   buf = handle_table_get(drv->htab, buf_id);
   if (!buf) {
      mtx_unlock(&drv->mutex);
      return VA_STATUS_ERROR_INVALID_BUFFER;
   }

   if (buf->derived_surface.resource)
      pipe_resource_reference(&buf->derived_surface.resource, NULL);

   if (buf->type == VAEncCodedBufferType) {
      VACodedBufferSegment* node = buf->data;
      while (node) {
         VACodedBufferSegment* next = (VACodedBufferSegment*) node->next;
         FREE(node);
         node = next;
      }
   } else {
      FREE(buf->data);
   }

   if (buf->ctx) {
      assert(_mesa_set_search(buf->ctx->buffers, buf));
      _mesa_set_remove_key(buf->ctx->buffers, buf);
      vlVaGetBufferFeedback(buf);
      if (buf->fence && buf->ctx->decoder && buf->ctx->decoder->destroy_fence)
         buf->ctx->decoder->destroy_fence(buf->ctx->decoder, buf->fence);
   }

   if (buf->coded_surf)
      buf->coded_surf->coded_buf = NULL;

   FREE(buf);
   handle_table_remove(VL_VA_DRIVER(ctx)->htab, buf_id);
   mtx_unlock(&drv->mutex);

   return VA_STATUS_SUCCESS;
}

VAStatus
vlVaBufferInfo(VADriverContextP ctx, VABufferID buf_id, VABufferType *type,
               unsigned int *size, unsigned int *num_elements)
{
   vlVaDriver *drv;
   vlVaBuffer *buf;

   if (!ctx)
      return VA_STATUS_ERROR_INVALID_CONTEXT;

   drv = VL_VA_DRIVER(ctx);
   mtx_lock(&drv->mutex);
   buf = handle_table_get(drv->htab, buf_id);
   mtx_unlock(&drv->mutex);
   if (!buf)
      return VA_STATUS_ERROR_INVALID_BUFFER;

   *type = buf->type;
   *size = buf->size;
   *num_elements = buf->num_elements;

   return VA_STATUS_SUCCESS;
}

VAStatus
vlVaAcquireBufferHandle(VADriverContextP ctx, VABufferID buf_id,
                        VABufferInfo *out_buf_info)
{
   vlVaDriver *drv;
   uint32_t i;
   uint32_t mem_type;
   vlVaBuffer *buf ;
   struct pipe_screen *screen;

   /* List of supported memory types, in preferred order. */
   static const uint32_t mem_types[] = {
#ifdef _WIN32
      VA_SURFACE_ATTRIB_MEM_TYPE_NTHANDLE,
      VA_SURFACE_ATTRIB_MEM_TYPE_D3D12_RESOURCE,
#else
      VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME,
#endif
      0
   };

   if (!ctx)
      return VA_STATUS_ERROR_INVALID_CONTEXT;

   drv = VL_VA_DRIVER(ctx);
   screen = VL_VA_PSCREEN(ctx);
   mtx_lock(&drv->mutex);
   buf = handle_table_get(VL_VA_DRIVER(ctx)->htab, buf_id);
   mtx_unlock(&drv->mutex);

   if (!buf)
      return VA_STATUS_ERROR_INVALID_BUFFER;

   /* Only VA surface|image like buffers are supported for now .*/
   if (buf->type != VAImageBufferType)
      return VA_STATUS_ERROR_UNSUPPORTED_BUFFERTYPE;

   if (!out_buf_info)
      return VA_STATUS_ERROR_INVALID_PARAMETER;

   if (!out_buf_info->mem_type)
      mem_type = mem_types[0];
   else {
      mem_type = 0;
      for (i = 0; mem_types[i] != 0; i++) {
         if (out_buf_info->mem_type & mem_types[i]) {
            mem_type = out_buf_info->mem_type;
            break;
         }
      }
      if (!mem_type)
         return VA_STATUS_ERROR_UNSUPPORTED_MEMORY_TYPE;
   }

   if (!buf->derived_surface.resource)
      return VA_STATUS_ERROR_INVALID_BUFFER;

   if (buf->export_refcount > 0) {
      if (buf->export_state.mem_type != mem_type)
         return VA_STATUS_ERROR_INVALID_PARAMETER;
   } else {
      VABufferInfo * const buf_info = &buf->export_state;

      switch (mem_type) {
#ifdef _WIN32
      case VA_SURFACE_ATTRIB_MEM_TYPE_D3D12_RESOURCE:
      case VA_SURFACE_ATTRIB_MEM_TYPE_NTHANDLE:
#else
      case VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME:
#endif
      {
         struct winsys_handle whandle;

         mtx_lock(&drv->mutex);

         memset(&whandle, 0, sizeof(whandle));
         whandle.type = WINSYS_HANDLE_TYPE_FD;

#ifdef _WIN32
         if (mem_type == VA_SURFACE_ATTRIB_MEM_TYPE_D3D12_RESOURCE)
            whandle.type = WINSYS_HANDLE_TYPE_D3D12_RES;
#endif
         if (!screen->resource_get_handle(screen, drv->pipe,
                                          buf->derived_surface.resource,
                                          &whandle, PIPE_HANDLE_USAGE_FRAMEBUFFER_WRITE)) {
            mtx_unlock(&drv->mutex);
            return VA_STATUS_ERROR_INVALID_BUFFER;
         }

         mtx_unlock(&drv->mutex);

         buf_info->handle = (intptr_t)whandle.handle;

#ifdef _WIN32
         if (mem_type == VA_SURFACE_ATTRIB_MEM_TYPE_D3D12_RESOURCE)
            buf_info->handle = (intptr_t)whandle.com_obj;
#endif
         break;
      }
      default:
         return VA_STATUS_ERROR_UNSUPPORTED_MEMORY_TYPE;
      }

      buf_info->type = buf->type;
      buf_info->mem_type = mem_type;
      buf_info->mem_size = buf->num_elements * (size_t)buf->size;
   }

   buf->export_refcount++;

   *out_buf_info = buf->export_state;

   return VA_STATUS_SUCCESS;
}

VAStatus
vlVaReleaseBufferHandle(VADriverContextP ctx, VABufferID buf_id)
{
   vlVaDriver *drv;
   vlVaBuffer *buf;

   if (!ctx)
      return VA_STATUS_ERROR_INVALID_CONTEXT;

   drv = VL_VA_DRIVER(ctx);
   mtx_lock(&drv->mutex);
   buf = handle_table_get(drv->htab, buf_id);
   mtx_unlock(&drv->mutex);

   if (!buf)
      return VA_STATUS_ERROR_INVALID_BUFFER;

   if (buf->export_refcount == 0)
      return VA_STATUS_ERROR_INVALID_BUFFER;

   if (--buf->export_refcount == 0) {
      VABufferInfo * const buf_info = &buf->export_state;

      switch (buf_info->mem_type) {
#ifdef _WIN32
      case VA_SURFACE_ATTRIB_MEM_TYPE_D3D12_RESOURCE:
         // Do nothing for this case.
         break;
      case VA_SURFACE_ATTRIB_MEM_TYPE_NTHANDLE:
         CloseHandle((HANDLE) buf_info->handle);
      break;
#else
      case VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME:
         close((intptr_t)buf_info->handle);
         break;
#endif
      default:
         return VA_STATUS_ERROR_INVALID_BUFFER;
      }

      buf_info->mem_type = 0;
   }

   return VA_STATUS_SUCCESS;
}

#if VA_CHECK_VERSION(1, 15, 0)
VAStatus
vlVaSyncBuffer(VADriverContextP ctx, VABufferID buf_id, uint64_t timeout_ns)
{
   vlVaDriver *drv;
   vlVaContext *context;
   vlVaBuffer *buf;

   if (!ctx)
      return VA_STATUS_ERROR_INVALID_CONTEXT;

   drv = VL_VA_DRIVER(ctx);
   if (!drv)
      return VA_STATUS_ERROR_INVALID_CONTEXT;

   mtx_lock(&drv->mutex);
   buf = handle_table_get(drv->htab, buf_id);
   if (!buf) {
      mtx_unlock(&drv->mutex);
      return VA_STATUS_ERROR_INVALID_BUFFER;
   }

   /* No outstanding operation: nothing to do. */
   if (!buf->fence) {
      mtx_unlock(&drv->mutex);
      return VA_STATUS_SUCCESS;
   }

   context = buf->ctx;
   if (!context || !context->decoder) {
      mtx_unlock(&drv->mutex);
      return VA_STATUS_ERROR_INVALID_CONTEXT;
   }

   mtx_lock(&context->mutex);
   mtx_unlock(&drv->mutex);
   int ret = context->decoder->fence_wait(context->decoder, buf->fence, timeout_ns);
   mtx_unlock(&context->mutex);
   return ret ? VA_STATUS_SUCCESS : VA_STATUS_ERROR_TIMEDOUT;
}
#endif

void vlVaGetBufferFeedback(vlVaBuffer *buf)
{
   if (!buf->ctx || !buf->ctx->decoder || !buf->feedback)
      return;

   buf->ctx->decoder->get_feedback(buf->ctx->decoder, buf->feedback,
                                   &buf->coded_size, &buf->extended_metadata);
   buf->feedback = NULL;
}
