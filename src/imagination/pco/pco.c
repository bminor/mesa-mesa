/*
 * Copyright © 2024 Imagination Technologies Ltd.
 *
 * SPDX-License-Identifier: MIT
 */

/**
 * \file pco.c
 *
 * \brief Main compiler interface.
 */

#include "pco.h"
#include "pco_internal.h"
#include "util/macros.h"
#include "util/ralloc.h"

/**
 * \brief Allocates and sets up a PCO compiler context.
 *
 * \param[in] dev_info Device info.
 * \param[in] mem_ctx Ralloc memory allocation context.
 * \return The PCO compiler context, or NULL on failure.
 */
pco_ctx *pco_ctx_create(const struct pvr_device_info *dev_info, void *mem_ctx)
{
   pco_ctx *ctx = rzalloc_size(mem_ctx, sizeof(*ctx));

   ctx->dev_info = dev_info;

   pco_debug_init();

   return ctx;
}

/**
 * \brief Returns the device/core-specific SPIR-V to NIR options for a PCO
 * compiler context.
 *
 * \param[in] ctx PCO compiler context.
 * \return The device/core-specific SPIR-V to NIR options.
 */
const struct spirv_to_nir_options *pco_spirv_options(pco_ctx *ctx)
{
   unreachable("finishme: pco_spirv_options");
   return &ctx->spirv_options;
}

/**
 * \brief Returns the device/core-specific NIR options for a PCO compiler
 * context.
 *
 * \param[in] ctx PCO compiler context.
 * \return The device/core-specific NIR options.
 */
const nir_shader_compiler_options *pco_nir_options(pco_ctx *ctx)
{
   unreachable("finishme: pco_nir_options");
   return &ctx->nir_options;
}
