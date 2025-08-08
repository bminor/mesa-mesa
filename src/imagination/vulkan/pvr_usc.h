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

#include "common/pvr_iface.h"
#include "compiler/shader_enums.h"
#include "pco/pco.h"
#include "pvr_private.h"
#include "usc/pvr_uscgen.h"

/* EOT shader generation. */
struct pvr_eot_props {
   unsigned emit_count;

   bool shared_words;
   union {
      const uint32_t *state_words;
      const unsigned *state_regs;
   };

   unsigned msaa_samples;
   unsigned num_output_regs;

   uint64_t tile_buffer_addrs[PVR_MAX_COLOR_ATTACHMENTS];
};

pco_shader *pvr_usc_eot(pco_ctx *ctx,
                        struct pvr_eot_props *props,
                        const struct pvr_device_info *dev_info);

/* Transfer queue shader generation. */
struct pvr_tq_props {};

pco_shader *pvr_usc_tq(pco_ctx *ctx, struct pvr_tq_props *props);

pco_shader *pvr_uscgen_tq(pco_ctx *ctx,
                          const struct pvr_tq_shader_properties *shader_props,
                          struct pvr_tq_frag_sh_reg_layout *sh_reg_layout);

pco_shader *pvr_uscgen_loadop(pco_ctx *ctx, struct pvr_load_op *load_op);

/* Clear attachment shader generation. */
struct pvr_clear_attach_props {
   unsigned dword_count;
   unsigned offset;
   bool uses_tile_buffer;
};

pco_shader *pvr_uscgen_clear_attach(pco_ctx *ctx,
                                    struct pvr_clear_attach_props *props);

#define INDEX(d_c, o, u_t_b, i)                           \
   if (props->dword_count == d_c && props->offset == o && \
       props->uses_tile_buffer == u_t_b)                  \
   return i

inline static unsigned
pvr_uscgen_clear_attach_index(struct pvr_clear_attach_props *props)
{
   INDEX(1, 0, false, 0);
   INDEX(1, 1, false, 1);
   INDEX(1, 2, false, 2);
   INDEX(1, 3, false, 3);
   INDEX(2, 0, false, 4);
   INDEX(2, 1, false, 5);
   INDEX(2, 2, false, 6);
   INDEX(3, 0, false, 7);
   INDEX(3, 1, false, 8);
   INDEX(4, 0, false, 9);

   INDEX(1, 0, true, 10);
   INDEX(1, 1, true, 11);
   INDEX(1, 2, true, 12);
   INDEX(1, 3, true, 13);
   INDEX(2, 0, true, 14);
   INDEX(2, 1, true, 15);
   INDEX(2, 2, true, 16);
   INDEX(3, 0, true, 17);
   INDEX(3, 1, true, 18);
   INDEX(4, 0, true, 19);

   UNREACHABLE("Invalid clear attachment shader properties.");
}
#undef INDEX

#define PVR_NUM_CLEAR_ATTACH_SHADERS 20U

#endif /* PVR_USC_H */
