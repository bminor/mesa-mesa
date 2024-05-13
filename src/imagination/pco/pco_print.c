/*
 * Copyright © 2024 Imagination Technologies Ltd.
 *
 * SPDX-License-Identifier: MIT
 */

/**
 * \file pco_print.c
 *
 * \brief PCO printing functions.
 */

#include "pco.h"
#include "pco_builder.h"
#include "pco_common.h"
#include "pco_internal.h"
#include "util/macros.h"
#include "util/u_hexdump.h"

#include <inttypes.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>

typedef struct _pco_print_state {
   FILE *fp; /** The print target file pointer. */
   pco_shader *shader; /** The shader being printed. */
   unsigned indent; /** The current printing indent. */
   bool is_grouped; /** Whether the shader uses igrps. */
} pco_print_state;

enum color_esc {
   ESC_RESET = 0,
   ESC_BLACK,
   ESC_RED,
   ESC_GREEN,
   ESC_YELLOW,
   ESC_BLUE,
   ESC_PURPLE,
   ESC_CYAN,
   ESC_WHITE,
   _ESC_COUNT,
};

static
const char *color_esc[2][_ESC_COUNT] = {
   [0] = {
      [ESC_RESET] = "",
      [ESC_BLACK] = "",
      [ESC_RED] = "",
      [ESC_GREEN] = "",
      [ESC_YELLOW] = "",
      [ESC_BLUE] = "",
      [ESC_PURPLE] = "",
      [ESC_CYAN] = "",
      [ESC_WHITE] = "",
   },
   [1] = {
      [ESC_RESET] = "\033[0m",
      [ESC_BLACK] = "\033[0;30m",
      [ESC_RED] = "\033[0;31m",
      [ESC_GREEN] = "\033[0;32m",
      [ESC_YELLOW] = "\033[0;33m",
      [ESC_BLUE] = "\033[0;34m",
      [ESC_PURPLE] = "\033[0;35m",
      [ESC_CYAN] = "\033[0;36m",
      [ESC_WHITE] = "\033[0;37m",
   },
};

static inline void RESET(pco_print_state *state)
{
   fputs(color_esc[pco_color][ESC_RESET], state->fp);
}

static inline void BLACK(pco_print_state *state)
{
   fputs(color_esc[pco_color][ESC_BLACK], state->fp);
}

static inline void RED(pco_print_state *state)
{
   fputs(color_esc[pco_color][ESC_RED], state->fp);
}

static inline void GREEN(pco_print_state *state)
{
   fputs(color_esc[pco_color][ESC_GREEN], state->fp);
}

static inline void YELLOW(pco_print_state *state)
{
   fputs(color_esc[pco_color][ESC_YELLOW], state->fp);
}

static inline void BLUE(pco_print_state *state)
{
   fputs(color_esc[pco_color][ESC_BLUE], state->fp);
}

static inline void PURPLE(pco_print_state *state)
{
   fputs(color_esc[pco_color][ESC_PURPLE], state->fp);
}

static inline void CYAN(pco_print_state *state)
{
   fputs(color_esc[pco_color][ESC_CYAN], state->fp);
}

static inline void WHITE(pco_print_state *state)
{
   fputs(color_esc[pco_color][ESC_WHITE], state->fp);
}

inline static const char *true_false_str(bool b)
{
   return b ? "true" : "false";
}

static void
_pco_printf(pco_print_state *state, bool nl, const char *fmt, va_list args)
{
   if (nl)
      for (unsigned u = 0; u < state->indent; ++u)
         fputs("    ", state->fp);

   vfprintf(state->fp, fmt, args);
}

/**
 * \brief Formatted print.
 *
 * \param[in] state Print state.
 * \param[in] fmt Print format.
 */
PRINTFLIKE(2, 3)
static void pco_printf(pco_print_state *state, const char *fmt, ...)
{
   va_list args;
   va_start(args, fmt);
   _pco_printf(state, false, fmt, args);
   va_end(args);
}

/**
 * \brief Formatted print, with indentation.
 *
 * \param[in] state Print state.
 * \param[in] fmt Print format.
 */
PRINTFLIKE(2, 3)
static void pco_printfi(pco_print_state *state, const char *fmt, ...)
{
   va_list args;
   va_start(args, fmt);
   _pco_printf(state, true, fmt, args);
   va_end(args);
}

/**
 * \brief Print PCO shader info.
 *
 * \param[in] state Print state.
 * \param[in] shader PCO shader.
 */
static void pco_print_shader_info(pco_print_state *state, pco_shader *shader)
{
   if (shader->name)
      pco_printfi(state, "name: \"%s\"\n", shader->name);
   pco_printfi(state, "stage: %s\n", gl_shader_stage_name(shader->stage));
   pco_printfi(state, "internal: %s\n", true_false_str(shader->is_internal));
   /* TODO: more info/stats, e.g. stage, temps/other regs used, etc.? */
}

/**
 * \brief Print PCO shader.
 *
 * \param[in] shader PCO shader.
 * \param[in] fp Print target file pointer.
 * \param[in] when When the printing is being performed.
 */
void pco_print_shader(pco_shader *shader, FILE *fp, const char *when)
{
   pco_print_state state = {
      .fp = fp,
      .shader = shader,
      .indent = 0,
      .is_grouped = shader->is_grouped,
   };

   if (when)
      fprintf(fp, "%s\n", when);

   pco_print_shader_info(&state, shader);
   pco_printfi(&state, "finishme: pco_print_shader\n");
}

/**
 * \brief Print PCO shader binary.
 *
 * \param[in] shader PCO shader.
 * \param[in] fp Print target file pointer.
 * \param[in] when When the printing is being performed.
 */
void pco_print_binary(pco_shader *shader, FILE *fp, const char *when)
{
   pco_print_state state = {
      .fp = fp,
      .shader = shader,
      .indent = 0,
      .is_grouped = shader->is_grouped,
   };

   if (when)
      fprintf(fp, "%s\n", when);

   pco_print_shader_info(&state, shader);

   return u_hexdump(fp,
                    pco_shader_binary_data(shader),
                    pco_shader_binary_size(shader),
                    true);
}
