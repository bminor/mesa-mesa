/**************************************************************************
 *
 * Copyright 2007 VMware, Inc.
 * Copyright (c) 2008 VMware, Inc.
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
 * IN NO EVENT SHALL VMWARE AND/OR ITS SUPPLIERS BE LIABLE FOR
 * ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,
 * TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE
 * SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 *
 **************************************************************************/

#ifndef U_FORMATS_H_
#define U_FORMATS_H_

#include "util/detect.h"
#include "util/format/u_format_gen.h"

#ifdef __cplusplus
extern "C" {
#endif

enum pipe_video_chroma_format
{
   PIPE_VIDEO_CHROMA_FORMAT_400,
   PIPE_VIDEO_CHROMA_FORMAT_420,
   PIPE_VIDEO_CHROMA_FORMAT_422,
   PIPE_VIDEO_CHROMA_FORMAT_444,
   PIPE_VIDEO_CHROMA_FORMAT_440,
   PIPE_VIDEO_CHROMA_FORMAT_NONE
};

static inline enum pipe_video_chroma_format
pipe_format_to_chroma_format(enum pipe_format format)
{
   switch (format) {
      case PIPE_FORMAT_NV12:
      case PIPE_FORMAT_NV21:
      case PIPE_FORMAT_YV12:
      case PIPE_FORMAT_IYUV:
      case PIPE_FORMAT_P010:
      case PIPE_FORMAT_P012:
      case PIPE_FORMAT_P016:
      case PIPE_FORMAT_P030:
      case PIPE_FORMAT_Y10X6_U10X6_V10X6_420_UNORM:
      case PIPE_FORMAT_Y12X4_U12X4_V12X4_420_UNORM:
      case PIPE_FORMAT_Y16_U16_V16_420_UNORM:
      case PIPE_FORMAT_Y8U8V8_420_UNORM_PACKED:
      case PIPE_FORMAT_Y10U10V10_420_UNORM_PACKED:
         return PIPE_VIDEO_CHROMA_FORMAT_420;
      case PIPE_FORMAT_UYVY:
      case PIPE_FORMAT_VYUY:
      case PIPE_FORMAT_YUYV:
      case PIPE_FORMAT_YVYU:
      case PIPE_FORMAT_YV16:
      case PIPE_FORMAT_NV16:
      case PIPE_FORMAT_Y8_U8_V8_422_UNORM:
      case PIPE_FORMAT_Y10X6_U10X6_V10X6_422_UNORM:
      case PIPE_FORMAT_Y12X4_U12X4_V12X4_422_UNORM:
      case PIPE_FORMAT_Y16_U16_V16_422_UNORM:
      case PIPE_FORMAT_Y16_U16V16_422_UNORM:
         return PIPE_VIDEO_CHROMA_FORMAT_422;
      case PIPE_FORMAT_Y8_U8_V8_444_UNORM:
      case PIPE_FORMAT_Y10X6_U10X6_V10X6_444_UNORM:
      case PIPE_FORMAT_Y12X4_U12X4_V12X4_444_UNORM:
      case PIPE_FORMAT_Y16_U16_V16_444_UNORM:
         return PIPE_VIDEO_CHROMA_FORMAT_444;
      case PIPE_FORMAT_Y8_U8_V8_440_UNORM:
         return PIPE_VIDEO_CHROMA_FORMAT_440;
      case PIPE_FORMAT_Y8_400_UNORM:
         return PIPE_VIDEO_CHROMA_FORMAT_400;
      default:
         return PIPE_VIDEO_CHROMA_FORMAT_NONE;
   }
}

/**
 * Texture & format swizzles
 */
enum pipe_swizzle {
   PIPE_SWIZZLE_X,
   PIPE_SWIZZLE_Y,
   PIPE_SWIZZLE_Z,
   PIPE_SWIZZLE_W,
   PIPE_SWIZZLE_0,
   PIPE_SWIZZLE_1,

   /* Non-existent format channel, not used for swizzle operations. */
   PIPE_SWIZZLE_NONE,

   PIPE_SWIZZLE_MAX, /**< Number of enums counter (must be last) */
};

#define PIPE_MASK_R  0x1
#define PIPE_MASK_G  0x2
#define PIPE_MASK_B  0x4
#define PIPE_MASK_A  0x8
#define PIPE_MASK_RGBA 0xf
#define PIPE_MASK_Z  0x10
#define PIPE_MASK_S  0x20
#define PIPE_MASK_ZS 0x30
#define PIPE_MASK_RGBAZS (PIPE_MASK_RGBA|PIPE_MASK_ZS)

union pipe_color_union
{
   float f[4];
   int i[4];
   unsigned int ui[4];
};

#ifdef __cplusplus
}
#endif

#endif /* U_FORMATS_H_ */
