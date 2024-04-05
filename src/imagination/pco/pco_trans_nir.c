/*
 * Copyright © 2024 Imagination Technologies Ltd.
 *
 * SPDX-License-Identifier: MIT
 */

/**
 * \file pco_trans_nir.c
 *
 * \brief NIR translation functions.
 */

#include "pco.h"
#include "pco_internal.h"

#include <stdio.h>

/**
 * \brief Translates a NIR shader into a PCO shader.
 *
 * \param[in] ctx PCO compiler context.
 * \param[in] nir NIR shader.
 * \param[in] mem_ctx Ralloc memory allocation context.
 * \return The PCO shader.
 */
pco_shader *pco_trans_nir(pco_ctx *ctx, nir_shader *nir, void *mem_ctx)
{
   puts("finishme: pco_trans_nir");
   return ralloc_context(mem_ctx);
}
