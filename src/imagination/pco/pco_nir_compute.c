/*
 * Copyright © 2025 Imagination Technologies Ltd.
 *
 * SPDX-License-Identifier: MIT
 */

/**
 * \file pco_nir_compute.c
 *
 * \brief PCO NIR compute-specific passes.
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

#define INST_CHK_FUNC "@pco_inst_chk"

/**
 * \brief Inserts the instance check.
 *
 * \param[in,out] shader NIR shader.
 */
static void insert_instance_check(nir_shader *shader)
{
   /* Get original entrypoint. */
   nir_function *orig_entrypoint = nir_shader_get_entrypoint(shader)->function;

   /* Create a function for the instance check which will serve as the new
    * entrypoint.
    */
   nir_function *inst_chk_func = nir_function_create(shader, INST_CHK_FUNC);

   inst_chk_func->is_entrypoint = true;
   orig_entrypoint->is_entrypoint = false;

   nir_builder b = nir_builder_create(nir_function_impl_create(inst_chk_func));
   b.cursor = nir_after_cf_list(&b.impl->body);

   /* If the current instance index is greater than the total workgroup size,
    * we don't execute.
    */
   nir_def *local_size = nir_load_workgroup_size(&b);
   nir_def *size_x = nir_channel(&b, local_size, 0);
   nir_def *size_y = nir_channel(&b, local_size, 1);
   nir_def *size_z = nir_channel(&b, local_size, 2);
   nir_def *flat_size = nir_imul(&b, nir_imul(&b, size_x, size_y), size_z);

   nir_def *flat_id = nir_load_local_invocation_index(&b);

   nir_def *cond_inst_valid = nir_ilt(&b, flat_id, flat_size);
   nir_if *nif = nir_push_if(&b, cond_inst_valid);
   {
      nir_call(&b, orig_entrypoint);
   }
   nir_pop_if(&b, nif);
   nir_jump(&b, nir_jump_return);
}

/**
 * \brief Inserts an instance check for compute shaders.
 *
 * \param[in,out] shader NIR shader.
 * \return True if the pass made progress.
 */
bool pco_nir_compute_instance_check(nir_shader *shader)
{
   assert(shader->info.stage == MESA_SHADER_COMPUTE);

   if (shader->info.internal)
      return false;

   /* Check we haven't already done this. */
   nir_foreach_function (function, shader) {
      if (function->name && !strcmp(function->name, INST_CHK_FUNC))
         return false;
   }

   insert_instance_check(shader);

   /* Re-inline. */
   NIR_PASS(_, shader, nir_lower_variable_initializers, nir_var_function_temp);
   NIR_PASS(_, shader, nir_lower_returns);
   NIR_PASS(_, shader, nir_inline_functions);
   NIR_PASS(_, shader, nir_copy_prop);
   NIR_PASS(_, shader, nir_opt_deref);
   nir_remove_non_entrypoints(shader);
   NIR_PASS(_, shader, nir_lower_variable_initializers, ~0);

   return true;
}
