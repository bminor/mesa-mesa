/*
 * Copyright © 2025 Imagination Technologies Ltd.
 *
 * SPDX-License-Identifier: MIT
 */

/**
 * \file pvr_usc.c
 *
 * \brief USC internal shader generation.
 */

#include "nir/nir.h"
#include "nir/nir_builder.h"
#include "pco/pco.h"
#include "pco/pco_data.h"
#include "pco_uscgen_programs.h"
#include "pvr_usc.h"
#include "util/macros.h"

/**
 * Common function to build a NIR shader and export the binary.
 *
 * \param ctx PCO context.
 * \param nir NIR shader.
 * \param data Shader data.
 * \return The finalized PCO shader.
 */
static pco_shader *build_shader(pco_ctx *ctx, nir_shader *nir, pco_data *data)
{
   pco_preprocess_nir(ctx, nir);
   pco_lower_nir(ctx, nir, data);
   pco_postprocess_nir(ctx, nir, data);

   pco_shader *shader = pco_trans_nir(ctx, nir, data, NULL);
   ralloc_steal(shader, nir);
   pco_process_ir(ctx, shader);
   pco_encode_ir(ctx, shader);

   return shader;
}

/**
 * Generate a nop (empty) shader.
 *
 * \param ctx PCO context.
 * \param stage Shader stage.
+ * \return The nop shader.
 */
pco_shader *pvr_usc_nop(pco_ctx *ctx, mesa_shader_stage stage)
{
   nir_builder b =
      nir_builder_init_simple_shader(stage,
                                     pco_nir_options(),
                                     "nop (%s)",
                                     _mesa_shader_stage_to_string(stage));

   /* Just return. */
   nir_jump(&b, nir_jump_return);

   return build_shader(ctx, b.shader, &(pco_data){ 0 });
}

/**
 * Generate an end-of-tile shader.
 *
 * \param ctx PCO context.
 * \param props End of tile shader properties.
 * \return The end-of-tile shader.
 */
pco_shader *pvr_usc_eot(pco_ctx *ctx, struct pvr_eot_props *props)
{
   UNREACHABLE("finishme: pvr_usc_eot");
}

/**
 * Generate a transfer queue shader.
 *
 * \param ctx PCO context.
 * \param props Transfer queue shader properties.
 * \return The transfer queue shader.
 */
pco_shader *pvr_usc_tq(pco_ctx *ctx, struct pvr_tq_props *props)
{
   UNREACHABLE("finishme: pvr_usc_tq");
}
