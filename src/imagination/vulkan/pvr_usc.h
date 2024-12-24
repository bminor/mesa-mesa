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

/* NOP shader generation. */
pco_shader *pvr_usc_nop(pco_ctx *ctx, mesa_shader_stage stage);

/* EOT shader generation. */
struct pvr_eot_props {
};

pco_shader *pvr_usc_eot(pco_ctx *ctx, struct pvr_eot_props *props);

/* Transfer queue shader generation. */
struct pvr_tq_props {
};

pco_shader *pvr_usc_tq(pco_ctx *ctx, struct pvr_tq_props *props);

#endif /* PVR_USC_H */
