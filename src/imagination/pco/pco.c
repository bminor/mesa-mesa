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

#ifndef NDEBUG
   /* Ensure NIR debug variables are processed. */
   nir_process_debug_variable();
#endif /* NDEBUG */

   pco_setup_spirv_options(dev_info, &ctx->spirv_options);
   pco_setup_nir_options(dev_info, &ctx->nir_options);

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
   return &ctx->nir_options;
}

/**
 * \brief Allocates and sets up a PCO instruction.
 *
 * \param[in,out] func Parent function.
 * \param[in] op Instruction op.
 * \param[in] num_dests Number of destinations.
 * \param[in] num_srcs Number of sources.
 * \return The PCO instruction, or NULL on failure.
 */
pco_instr *pco_instr_create(pco_func *func,
                            enum pco_op op,
                            unsigned num_dests,
                            unsigned num_srcs)
{
   pco_instr *instr;
   unsigned size = sizeof(*instr);
   size += num_dests * sizeof(*instr->dest);
   size += num_srcs * sizeof(*instr->src);

   instr = rzalloc_size(func, size);

   instr->parent_func = func;

   instr->op = op;

   instr->num_dests = num_dests;
   instr->dest = (pco_ref *)(instr + 1);

   instr->num_srcs = num_srcs;
   instr->src = instr->dest + num_dests;

   instr->index = func->next_instr++;

   return instr;
}
