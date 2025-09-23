/*
 * Copyright © 2024 Collabora Ltd. and Red Hat Inc.
 * Copyright 2025 LunarG, Inc.
 * Copyright 2025 Google LLC
 * SPDX-License-Identifier: MIT
 */

#ifndef KK_PRIVATE_H
#define KK_PRIVATE_H 1

#include "kosmickrisp/bridge/mtl_types.h"

#include "vk_log.h"
#include "vk_util.h"

#include <assert.h>

#define KK_MAX_SETS                   32
#define KK_MAX_PUSH_SIZE              128
#define KK_MAX_DYNAMIC_BUFFERS        64
#define KK_MAX_RTS                    8
#define KK_MAX_SAMPLES                8
#define KK_MIN_SSBO_ALIGNMENT         16
#define KK_MIN_TEXEL_BUFFER_ALIGNMENT 16
#define KK_MIN_UBO_ALIGNMENT          64
#define KK_MAX_VIEWPORTS              16
#define KK_MAX_DESCRIPTOR_SIZE        64
#define KK_MAX_PUSH_DESCRIPTORS       32
#define KK_MAX_DESCRIPTOR_SET_SIZE    (1u << 30)
#define KK_MAX_DESCRIPTORS            (1 << 20)
#define KK_PUSH_DESCRIPTOR_SET_SIZE                                            \
   (KK_MAX_PUSH_DESCRIPTORS * KK_MAX_DESCRIPTOR_SIZE)
#define KK_SSBO_BOUNDS_CHECK_ALIGNMENT 4
#define KK_MAX_MULTIVIEW_VIEW_COUNT    32
#define KK_TEXTURE_BUFFER_WIDTH        (1u << 14)
#define KK_MAX_OCCLUSION_QUERIES       (32768)

#define KK_SPARSE_ADDR_SPACE_SIZE (1ull << 39)
#define KK_MAX_BUFFER_SIZE        (1ull << 31)
#define KK_MAX_SHARED_SIZE        (32 * 1024)

/* Max size of a bound cbuf */
#define KK_MAX_CBUF_SIZE (1u << 16)

/* Metal related macros */
#define KK_MTL_RESOURCE_OPTIONS                                                \
   MTL_RESOURCE_STORAGE_MODE_SHARED |                                          \
      MTL_RESOURCE_CPU_CACHE_MODE_DEFAULT_CACHE |                              \
      MTL_RESOURCE_TRACKING_MODE_UNTRACKED

#define KK_MAX_CMD_BUFFERS 256

struct kk_addr_range {
   uint64_t addr;
   uint64_t range;
};

typedef enum kk_env_option_t {
   KK_ENABLE_GPU_CAPTURE = 0,
   KK_MAX_ENV_OPTIONS,
} kk_env_option_t;

struct kk_env_option {
   const char *name;
   bool value;
};

static struct kk_env_option KK_ENV_OPTIONS[KK_MAX_ENV_OPTIONS] = {
   [KK_ENABLE_GPU_CAPTURE] =
      {
         .name = "MESA_KOSMICKRISP_ENABLE_GPU_CAPTURE",
         .value = false,
      },
};

static inline bool
kk_get_environment_boolean(kk_env_option_t option)
{
   assert(option >= 0 && option < KK_MAX_ENV_OPTIONS);
   struct kk_env_option *opt = &KK_ENV_OPTIONS[option];
   const char *env_str = getenv(opt->name);
   if (env_str) {
      if (strncmp(env_str, "0", 1) != 0) {
         opt->value = true;
      } else {
         opt->value = false;
      }
   }
   return opt->value;
}

#define kk_debug_ignored_stype(sType)                                          \
   mesa_logd("%s: ignored VkStructureType %u\n", __func__, (sType))

#endif
