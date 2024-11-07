/*
 * Copyright © 2024 Collabora Ltd.
 * SPDX-License-Identifier: MIT
 */

#ifndef PANVK_CMD_DISPATCH_H
#define PANVK_CMD_DISPATCH_H

#ifndef PAN_ARCH
#error "PAN_ARCH must be defined"
#endif

struct panvk_cmd_compute_state {
   struct panvk_descriptor_state desc_state;
   const struct panvk_shader *shader;
   struct panvk_compute_sysvals sysvals;
   mali_ptr push_uniforms;
   struct {
      struct panvk_shader_desc_state desc;
   } cs;
};

#endif
