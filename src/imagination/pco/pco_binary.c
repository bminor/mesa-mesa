/*
 * Copyright © 2024 Imagination Technologies Ltd.
 *
 * SPDX-License-Identifier: MIT
 */

/**
 * \file pco_binary.c
 *
 * \brief PCO binary-specific functions.
 */

#include "pco.h"
#include "pco_internal.h"
#include "pco_isa.h"

#include <stdio.h>

/**
 * \brief Encodes a PCO shader into binary.
 *
 * \param[in] ctx PCO compiler context.
 * \param[in,out] shader PCO shader.
 */
void pco_encode_ir(pco_ctx *ctx, pco_shader *shader)
{
   puts("finishme: pco_encode_ir");

   if (pco_should_print_binary(shader))
      pco_print_binary(shader, stdout, "after encoding");
}

/**
 * \brief Finalizes a PCO shader binary.
 *
 * \param[in] ctx PCO compiler context.
 * \param[in,out] shader PCO shader.
 */
void pco_shader_finalize(pco_ctx *ctx, pco_shader *shader)
{
   puts("finishme: pco_shader_finalize");

   if (pco_should_print_binary(shader))
      pco_print_binary(shader, stdout, "after finalize");
}

/**
 * \brief Returns the size in bytes of a PCO shader binary.
 *
 * \param[in] shader PCO shader.
 * \return The size in bytes of the PCO shader binary.
 */
unsigned pco_shader_binary_size(pco_shader *shader)
{
   puts("finishme: pco_binary_size");
   return 0;
}

/**
 * \brief Returns the PCO shader binary data.
 *
 * \param[in] shader PCO shader.
 * \return The PCO shader binary data.
 */
const void *pco_shader_binary_data(pco_shader *shader)
{
   puts("finishme: pco_binary_data");
   return NULL;
}
