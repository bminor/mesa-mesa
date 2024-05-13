/*
 * Copyright © 2024 Imagination Technologies Ltd.
 *
 * SPDX-License-Identifier: MIT
 */

/**
 * \file pco_validate.c
 *
 * \brief PCO validation functions.
 */

#include "pco.h"
#include "pco_internal.h"
#include "util/macros.h"

/**
 * \brief Validates a PCO shader.
 *
 * \param[in] shader PCO shader.
 * \param[in] when When the validation check is being run.
 */
void pco_validate_shader(UNUSED pco_shader *shader, UNUSED const char *when)
{
#ifndef NDEBUG
   if (PCO_DEBUG(VAL_SKIP))
      return;

   puts("finishme: pco_validate_shader");
#endif /* NDEBUG */
}
