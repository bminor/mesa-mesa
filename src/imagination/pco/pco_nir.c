/*
 * Copyright © 2024 Imagination Technologies Ltd.
 *
 * SPDX-License-Identifier: MIT
 */

/**
 * \file pco_nir.c
 *
 * \brief NIR-specific functions.
 */

#include "pco.h"
#include "pco_internal.h"

#include <stdio.h>

/** Base/common SPIR-V to NIR options. */
static const struct spirv_to_nir_options pco_base_spirv_options = {
   .environment = NIR_SPIRV_VULKAN,
};

/** Base/common NIR options. */
static const nir_shader_compiler_options pco_base_nir_options = {};

/**
 * \brief Sets up device/core-specific SPIR-V to NIR options.
 *
 * \param[in] dev_info Device info.
 * \param[out] spirv_options SPIR-V to NIR options.
 */
void pco_setup_spirv_options(const struct pvr_device_info *dev_info,
                             struct spirv_to_nir_options *spirv_options)
{
   memcpy(spirv_options, &pco_base_spirv_options, sizeof(*spirv_options));

   /* TODO: Device/core-dependent options. */
   puts("finishme: pco_setup_spirv_options");
}

/**
 * \brief Sets up device/core-specific NIR options.
 *
 * \param[in] dev_info Device info.
 * \param[out] nir_options NIR options.
 */
void pco_setup_nir_options(const struct pvr_device_info *dev_info,
                           nir_shader_compiler_options *nir_options)
{
   memcpy(nir_options, &pco_base_nir_options, sizeof(*nir_options));

   /* TODO: Device/core-dependent options. */
   puts("finishme: pco_setup_nir_options");
}

/**
 * \brief Runs pre-processing passes on a NIR shader.
 *
 * \param[in] ctx PCO compiler context.
 * \param[in,out] nir NIR shader.
 */
void pco_preprocess_nir(pco_ctx *ctx, nir_shader *nir)
{
   puts("finishme: pco_preprocess_nir");
}

/**
 * \brief Lowers a NIR shader.
 *
 * \param[in] ctx PCO compiler context.
 * \param[in,out] nir NIR shader.
 */
void pco_lower_nir(pco_ctx *ctx, nir_shader *nir)
{
   puts("finishme: pco_lower_nir");
}

/**
 * \brief Runs post-processing passes on a NIR shader.
 *
 * \param[in] ctx PCO compiler context.
 * \param[in,out] nir NIR shader.
 */
void pco_postprocess_nir(pco_ctx *ctx, nir_shader *nir)
{
   puts("finishme: pco_postprocess_nir");
}

/**
 * \brief Performs linking optimizations on consecutive NIR shader stages.
 *
 * \param[in] ctx PCO compiler context.
 * \param[in,out] producer NIR producer shader.
 * \param[in,out] consumer NIR consumer shader.
 */
void pco_link_nir(pco_ctx *ctx, nir_shader *producer, nir_shader *consumer)
{
   puts("finishme: pco_link_nir");
}
