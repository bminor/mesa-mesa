/*
 * Copyright © 2024 Imagination Technologies Ltd.
 *
 * SPDX-License-Identifier: MIT
 */

/**
 * \file pco_ir.c
 *
 * \brief PCO IR-specific functions.
 */

#include "pco.h"
#include "pco_internal.h"
#include "util/u_debug.h"

#include <stdbool.h>
#include <stdio.h>

static inline bool pco_should_skip_pass(const char *pass)
{
   return comma_separated_list_contains(pco_skip_passes, pass);
}

#define PCO_PASS(progress, shader, pass, ...)            \
   do {                                                  \
      if (pco_should_skip_pass(#pass)) {                 \
         fprintf(stdout, "Skipping pass '%s'\n", #pass); \
         break;                                          \
      }                                                  \
                                                         \
      if (pass(shader, ##__VA_ARGS__)) {                 \
         UNUSED bool _;                                  \
         progress = true;                                \
      }                                                  \
   } while (0)

/**
 * \brief Runs passes on a PCO shader.
 *
 * \param[in] ctx PCO compiler context.
 * \param[in,out] shader PCO shader.
 */
void pco_process_ir(pco_ctx *ctx, pco_shader *shader)
{
   PCO_PASS(_, shader, pco_end);

   puts("finishme: pco_process_ir");
}
