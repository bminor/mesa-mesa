/*
 * Copyright © 2022 Imagination Technologies Ltd.
 *
 * based in part on anv driver which is:
 * Copyright © 2015 Intel Corporation
 *
 * based in part on radv driver which is:
 * Copyright © 2016 Red Hat.
 * Copyright © 2016 Bas Nieuwenhuizen
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

#ifndef PVR_PRIVATE_H
#define PVR_PRIVATE_H

#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <vulkan/vulkan.h>

#include "compiler/shader_enums.h"
#include "hwdef/rogue_hw_defs.h"
#include "pco/pco.h"
#include "pco/pco_data.h"
#include "pvr_border.h"
#include "pvr_clear.h"
#include "pvr_common.h"
#include "pvr_csb.h"
#include "pvr_device_info.h"
#include "pvr_entrypoints.h"
#include "pvr_framebuffer.h"
#include "pvr_hw_pass.h"
#include "pvr_job_render.h"
#include "pvr_limits.h"
#include "pvr_pds.h"
#include "pvr_usc.h"
#include "pvr_spm.h"
#include "pvr_types.h"
#include "pvr_winsys.h"
#include "util/bitscan.h"
#include "util/format/u_format.h"
#include "util/log.h"
#include "util/macros.h"
#include "util/simple_mtx.h"
#include "util/u_dynarray.h"
#include "util/u_math.h"
#include "vk_enum_to_str.h"
#include "vk_graphics_state.h"
#include "vk_log.h"
#include "vk_sync.h"
#include "wsi_common.h"

#ifdef HAVE_VALGRIND
#   include <valgrind/valgrind.h>
#   include <valgrind/memcheck.h>
#   define VG(x) x
#else
#   define VG(x) ((void)0)
#endif

/**
 * Print a FINISHME message, including its source location.
 */
#define pvr_finishme(format, ...)              \
   do {                                        \
      static bool reported = false;            \
      if (!reported) {                         \
         mesa_logw("%s:%d: FINISHME: " format, \
                   __FILE__,                   \
                   __LINE__,                   \
                   ##__VA_ARGS__);             \
         reported = true;                      \
      }                                        \
   } while (false)

#define PVR_WRITE(_buffer, _value, _offset, _max)                \
   do {                                                          \
      __typeof__(_value) __value = _value;                       \
      uint64_t __offset = _offset;                               \
      uint32_t __nr_dwords = sizeof(__value) / sizeof(uint32_t); \
      static_assert(__same_type(*_buffer, __value),              \
                    "Buffer and value type mismatch");           \
      assert((__offset + __nr_dwords) <= (_max));                \
      assert((__offset % __nr_dwords) == 0U);                    \
      _buffer[__offset / __nr_dwords] = __value;                 \
   } while (0)

/* A non-fatal assert. Useful for debugging. */
#if MESA_DEBUG
#   define pvr_assert(x)                                           \
      ({                                                           \
         if (unlikely(!(x)))                                       \
            mesa_loge("%s:%d ASSERT: %s", __FILE__, __LINE__, #x); \
      })
#else
#   define pvr_assert(x)
#endif

#endif /* PVR_PRIVATE_H */
