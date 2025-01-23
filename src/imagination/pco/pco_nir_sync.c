/*
 * Copyright © 2025 Imagination Technologies Ltd.
 *
 * SPDX-License-Identifier: MIT
 */

/**
 * \file pco_nir_sync.c
 *
 * \brief PCO NIR sync-related passes.
 */

#include "hwdef/rogue_hw_defs.h"
#include "nir.h"
#include "nir_builder.h"
#include "nir_builtin_builder.h"
#include "pco.h"
#include "pco_builder.h"
#include "pco_internal.h"
#include "pco_usclib.h"
#include "util/macros.h"

#include <assert.h>
#include <stdbool.h>
#include <stdio.h>

/**
 * \brief Lowers a barrier instruction.
 *
 * \param[in] b NIR builder.
 * \param[in] instr NIR instruction.
 * \param[in] cb_data User callback data.
 * \return The replacement/lowered def.
 */
static nir_def *lower_barrier(nir_builder *b, nir_instr *instr, void *cb_data)
{
   struct shader_info *info = &b->shader->info;
   bool *barrier_emitted = cb_data;

   nir_intrinsic_instr *intr = nir_instr_as_intrinsic(instr);
   mesa_scope exec_scope = nir_intrinsic_execution_scope(intr);

   unsigned wg_size = info->workgroup_size[0] * info->workgroup_size[1] *
                      info->workgroup_size[2];

   if (wg_size <= ROGUE_MAX_INSTANCES_PER_TASK || exec_scope == SCOPE_NONE)
      return NIR_LOWER_INSTR_PROGRESS_REPLACE;

   /* TODO: We might be able to re-use barrier counters. */
   unsigned counter_offset = info->shared_size;
   info->shared_size += sizeof(uint32_t);
   info->zero_initialize_shared_memory = true;

   *barrier_emitted = true;

   unsigned num_slots = DIV_ROUND_UP(wg_size, ROGUE_MAX_INSTANCES_PER_TASK);

   b->cursor = nir_before_instr(instr);
   usclib_barrier(b, nir_imm_int(b, num_slots), nir_imm_int(b, counter_offset));

   return NIR_LOWER_INSTR_PROGRESS_REPLACE;
}

/**
 * \brief Filters barrier instructions.
 *
 * \param[in] instr NIR instruction.
 * \param[in] cb_data User callback data.
 * \return True if the instruction matches the filter.
 */
static bool is_barrier(const nir_instr *instr, UNUSED const void *cb_data)
{
   if (instr->type != nir_instr_type_intrinsic)
      return false;

   return nir_instr_as_intrinsic(instr)->intrinsic == nir_intrinsic_barrier;
}

/**
 * \brief Barrier lowering pass.
 *
 * \param[in,out] shader NIR shader.
 * \return True if the pass made progress.
 */
bool pco_nir_lower_barriers(nir_shader *shader, bool *uses_usclib)
{
   bool barrier_emitted = false;
   bool progress = nir_shader_lower_instructions(shader,
                                                 is_barrier,
                                                 lower_barrier,
                                                 &barrier_emitted);

   *uses_usclib |= barrier_emitted;

   return progress;
}

static nir_def *lower_atomic(nir_builder *b, nir_instr *instr, void *cb_data)
{
   nir_intrinsic_instr *intr = nir_instr_as_intrinsic(instr);
   bool *uses_usclib = cb_data;

   b->cursor = nir_before_instr(instr);

   nir_def *buffer = intr->src[0].ssa;
   nir_def *offset = intr->src[1].ssa;
   nir_def *value = intr->src[2].ssa;
   nir_def *value_swap = intr->src[3].ssa;

   ASSERTED enum gl_access_qualifier access = nir_intrinsic_access(intr);
   ASSERTED unsigned num_components = intr->def.num_components;
   ASSERTED unsigned bit_size = intr->def.bit_size;
   assert(access == ACCESS_COHERENT);
   assert(num_components == 1 && bit_size == 32);

   *uses_usclib = true;
   return usclib_emu_ssbo_atomic_comp_swap(b, buffer, offset, value, value_swap);
}

/**
 * \brief Filters lowerable atomic instructions.
 *
 * \param[in] instr NIR instruction.
 * \param[in] cb_data User callback data.
 * \return True if the instruction matches the filter.
 */
static bool is_lowerable_atomic(const nir_instr *instr,
                                UNUSED const void *cb_data)
{
   if (instr->type != nir_instr_type_intrinsic)
      return false;

   return nir_instr_as_intrinsic(instr)->intrinsic ==
          nir_intrinsic_ssbo_atomic_swap;
}

/**
 * \brief Atomics lowering pass.
 *
 * \param[in,out] shader NIR shader.
 * \return True if the pass made progress.
 */
bool pco_nir_lower_atomics(nir_shader *shader, bool *uses_usclib)
{
   return nir_shader_lower_instructions(shader,
                                        is_lowerable_atomic,
                                        lower_atomic,
                                        uses_usclib);
}
