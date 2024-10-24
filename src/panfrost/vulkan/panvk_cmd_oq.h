/*
 * Copyright © 2024 Collabora Ltd.
 * SPDX-License-Identifier: MIT
 */

#ifndef PANVK_CMD_OQ_H
#define PANVK_CMD_OQ_H

#ifndef PAN_ARCH
#error "PAN_ARCH must be defined"
#endif

#include "genxml/gen_macros.h"
#include "panfrost-job.h"

struct panvk_occlusion_query_state {
   mali_ptr ptr;
   enum mali_occlusion_mode mode;
};

#endif