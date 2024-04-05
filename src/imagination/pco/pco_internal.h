/*
 * Copyright © 2024 Imagination Technologies Ltd.
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef PCO_INTERNAL_H
#define PCO_INTERNAL_H

/**
 * \file pco_internal.h
 *
 * \brief PCO internal header.
 */

#include "pco.h"
#include "spirv/nir_spirv.h"

/** PCO compiler context. */
typedef struct _pco_ctx {
   /** Device information. */
   const struct pvr_device_info *dev_info;

   /** Device-specific NIR options. */
   nir_shader_compiler_options nir_options;

   /** Device-specific SPIR-V to NIR options. */
   struct spirv_to_nir_options spirv_options;
} pco_ctx;

#endif /* PCO_INTERNAL_H */
