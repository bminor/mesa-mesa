/*
 * Copyright (C) 1999-2007  Brian Paul   All Rights Reserved.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included
 * in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS
 * OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR
 * OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE,
 * ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR
 * OTHER DEALINGS IN THE SOFTWARE.
 */

#ifndef DRI_HELPERS_H
#define DRI_HELPERS_H

#include "dri_context.h"
#include "dri_screen.h"


struct dri2_format_mapping {
   int dri_fourcc;
   int dri_format; /* image format */
   enum pipe_format pipe_format;
   int nplanes;
   struct {
      int buffer_index;
      int width_shift;
      int height_shift;
      uint32_t dri_format; /* plane format */
   } planes[3];
};

const struct dri2_format_mapping *
dri2_get_mapping_by_fourcc(int fourcc);

const struct dri2_format_mapping *
dri2_get_mapping_by_format(int format);

bool
dri2_yuv_dma_buf_supported(struct dri_screen *screen,
                           const struct dri2_format_mapping *map);

bool
dri2_validate_egl_image(struct dri_screen *screen, void *handle);
void
dri_image_fence_sync(struct dri_context *ctx, struct dri_image *img);
#endif

/* vim: set sw=3 ts=8 sts=3 expandtab: */
