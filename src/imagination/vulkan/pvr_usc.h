/*
 * Copyright © 2025 Imagination Technologies Ltd.
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef PVR_USC_H
#define PVR_USC_H

/**
 * \file pvr_usc.h
 *
 * \brief USC internal shader generation header.
 */

#include "compiler/shader_enums.h"
#include "pco/pco.h"
#include "pvr_private.h"
#include "usc/pvr_uscgen.h"

/* NOP shader generation. */
pco_shader *pvr_usc_nop(pco_ctx *ctx, mesa_shader_stage stage);

/* EOT shader generation. */
struct pvr_eot_props {
   unsigned emit_count;

   bool shared_words;
   union {
      const uint32_t *state_words;
      const unsigned *state_regs;
   };
};

pco_shader *pvr_usc_eot(pco_ctx *ctx, struct pvr_eot_props *props);

/* Transfer queue shader generation. */
struct pvr_tq_props {
};

pco_shader *pvr_usc_tq(pco_ctx *ctx, struct pvr_tq_props *props);

pco_shader *pvr_uscgen_tq(pco_ctx *ctx,
                          const struct pvr_tq_shader_properties *shader_props,
                          struct pvr_tq_frag_sh_reg_layout *sh_reg_layout);

pco_shader *pvr_uscgen_loadop(pco_ctx *ctx, struct pvr_load_op *load_op);

#endif /* PVR_USC_H */
