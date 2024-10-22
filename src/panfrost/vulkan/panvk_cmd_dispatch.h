/*
 * Copyright © 2024 Collabora Ltd.
 * SPDX-License-Identifier: MIT
 */

#ifndef PANVK_CMD_DISPATCH_H
#define PANVK_CMD_DISPATCH_H

#ifndef PAN_ARCH
#error "PAN_ARCH must be defined"
#endif

enum panvk_cmd_compute_dirty_state {
   PANVK_CMD_COMPUTE_DIRTY_CS,
   PANVK_CMD_COMPUTE_DIRTY_DESC_STATE,
   PANVK_CMD_COMPUTE_DIRTY_PUSH_UNIFORMS,
   PANVK_CMD_COMPUTE_DIRTY_STATE_COUNT,
};

struct panvk_cmd_compute_state {
   struct panvk_descriptor_state desc_state;
   const struct panvk_shader *shader;
   struct panvk_compute_sysvals sysvals;
   mali_ptr push_uniforms;
   struct {
      struct panvk_shader_desc_state desc;
   } cs;
   BITSET_DECLARE(dirty, PANVK_CMD_COMPUTE_DIRTY_STATE_COUNT);
};

#define compute_state_dirty(__cmdbuf, __name)                                  \
   BITSET_TEST((__cmdbuf)->state.compute.dirty,                                \
               PANVK_CMD_COMPUTE_DIRTY_##__name)

#define compute_state_set_dirty(__cmdbuf, __name)                              \
   BITSET_SET((__cmdbuf)->state.compute.dirty, PANVK_CMD_COMPUTE_DIRTY_##__name)

#define compute_state_clear_all_dirty(__cmdbuf)                                \
   BITSET_ZERO((__cmdbuf)->state.compute.dirty)

#define clear_dirty_after_dispatch(__cmdbuf)                                   \
   do {                                                                        \
      compute_state_clear_all_dirty(__cmdbuf);                                 \
      desc_state_clear_all_dirty(&(__cmdbuf)->state.compute.desc_state);       \
   } while (0)

#endif
