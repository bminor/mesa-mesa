/*
 * Copyright © 2025 Imagination Technologies Ltd.
 *
 * SPDX-License-Identifier: MIT
 */

/**
 * \file pco_nir_io.c
 *
 * \brief PCO NIR I/O lowering pass.
 */

#include "nir.h"
#include "nir_builder.h"
#include "pco.h"
#include "pco_builder.h"
#include "pco_internal.h"
#include "util/macros.h"

#include <assert.h>
#include <stdbool.h>
#include <stdio.h>

/**
 * \brief Lowers an I/O instruction.
 *
 * \param[in] b NIR builder.
 * \param[in] instr NIR instruction.
 * \param[in] cb_data User callback data.
 * \return The replacement/lowered def.
 */
static nir_def *lower_io(nir_builder *b, nir_instr *instr, UNUSED void *cb_data)
{
   nir_intrinsic_instr *intr = nir_instr_as_intrinsic(instr);
   b->cursor = nir_before_instr(instr);

   ASSERTED unsigned base = nir_intrinsic_base(intr);
   assert(!base);

   nir_src *offset_src;
   switch (intr->intrinsic) {
   case nir_intrinsic_load_push_constant:
   case nir_intrinsic_load_shared:
   case nir_intrinsic_shared_atomic:
   case nir_intrinsic_shared_atomic_swap:
      offset_src = &intr->src[0];
      break;

   case nir_intrinsic_store_shared:
      offset_src = &intr->src[1];
      break;

   default:
      UNREACHABLE("");
   }

   /* Byte offset to DWORD offset. */
   nir_src_rewrite(offset_src, nir_ushr_imm(b, offset_src->ssa, 2));

   return NIR_LOWER_INSTR_PROGRESS;
}

/**
 * \brief Filters I/O instructions that need lowering.
 *
 * \param[in] instr NIR instruction.
 * \param[in] cb_data User callback data.
 * \return True if the instruction matches the filter.
 */
static bool is_lowerable_io(const nir_instr *instr, UNUSED const void *cb_data)
{
   if (instr->type != nir_instr_type_intrinsic)
      return false;

   nir_intrinsic_instr *intr = nir_instr_as_intrinsic(instr);
   switch (intr->intrinsic) {
   case nir_intrinsic_load_push_constant:
   case nir_intrinsic_load_shared:
   case nir_intrinsic_store_shared:
   case nir_intrinsic_shared_atomic:
   case nir_intrinsic_shared_atomic_swap:
      return true;

   default:
      break;
   }

   return false;
}

/**
 * \brief I/O lowering pass.
 *
 * \param[in,out] shader NIR shader.
 * \return True if the pass made progress.
 */
bool pco_nir_lower_io(nir_shader *shader)
{
   bool progress = false;

   progress |=
      nir_shader_lower_instructions(shader, is_lowerable_io, lower_io, NULL);

   return progress;
}

bool pco_nir_lower_variables(nir_shader *shader, bool inputs, bool outputs)
{
   bool progress = false;

   nir_variable_mode modes = 0;
   assert(inputs || outputs);

   if (inputs)
      modes |= nir_var_shader_in;

   if (outputs)
      modes |= nir_var_shader_out;

   nir_foreach_variable_with_modes (var, shader, modes) {
      unsigned slots = glsl_count_vec4_slots(var->type, false, false);
      if (slots == 1)
         continue;

      assert(!var->data.location_frac);

      unsigned dwords = glsl_count_dword_slots(var->type, false);
      assert(!(dwords % slots));
      unsigned slot_size = dwords / slots;

      unsigned location = var->data.location;

      var->type = glsl_vector_type(
         glsl_get_base_type(glsl_without_array_or_matrix(var->type)),
         slot_size);
      dwords -= slot_size;
      for (unsigned u = 1; u < slots; ++u) {
         assert(!nir_find_variable_with_location(shader,
                                                 var->data.mode,
                                                 location + u));

         unsigned comps = dwords < slot_size ? dwords : slot_size;
         const glsl_type *type =
            glsl_vector_type(glsl_get_base_type(var->type), comps);
         dwords -= comps;

         nir_create_variable_with_location(shader,
                                           var->data.mode,
                                           location + u,
                                           type);
      }

      progress = true;
   }

   return nir_progress(progress,
                       nir_shader_get_entrypoint(shader),
                       nir_metadata_none);
}
