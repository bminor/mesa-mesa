/*
 * Copyright © 2024 Imagination Technologies Ltd.
 *
 * SPDX-License-Identifier: MIT
 */

/**
 * \file pco_end.c
 *
 * \brief PCO shader ending pass.
 */

#include "pco.h"
#include "pco_builder.h"
#include "pco_internal.h"
#include "util/macros.h"

#include <stdbool.h>

/**
 * \brief Processes end of shader instruction(s).
 *
 * \param[in,out] shader PCO shader.
 */
bool pco_end(pco_shader *shader)
{
   /* TODO: Support for multiple end points. */
   pco_func *entry = pco_entrypoint(shader);
   pco_block *last_block = pco_func_last_block(entry);
   pco_instr *last_instr = pco_last_instr(last_block);

   pco_builder b =
      pco_builder_create(entry, pco_cursor_after_block(last_block));

   if (last_instr && pco_instr_has_end(last_instr)) {
      pco_instr_set_end(last_instr, true);
      return true;
   }

   pco_nop_end(&b);

   return true;
}
