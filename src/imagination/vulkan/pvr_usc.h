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

pco_shader *
pvr_usc_zero_init_wg_mem(pco_ctx *ctx, unsigned start, unsigned count);

/* SPM load shader generation. */
struct pvr_spm_load_props {
   unsigned output_reg_count;
   unsigned tile_buffer_count;
   bool is_multisampled;
};

static inline unsigned pvr_uscgen_spm_buffer_data(unsigned buffer_index,
                                                  bool addr)
{
   switch (buffer_index) {
   case 0:
      return addr ? PVR_SPM_LOAD_DATA_BUF_ADDR_0 : PVR_SPM_LOAD_DATA_BUF_TEX_0;

   case 1:
      return addr ? PVR_SPM_LOAD_DATA_BUF_ADDR_1 : PVR_SPM_LOAD_DATA_BUF_TEX_1;

   case 2:
      return addr ? PVR_SPM_LOAD_DATA_BUF_ADDR_2 : PVR_SPM_LOAD_DATA_BUF_TEX_2;

   case 3:
      return addr ? PVR_SPM_LOAD_DATA_BUF_ADDR_3 : PVR_SPM_LOAD_DATA_BUF_TEX_3;

   case 4:
      return addr ? PVR_SPM_LOAD_DATA_BUF_ADDR_4 : PVR_SPM_LOAD_DATA_BUF_TEX_4;

   case 5:
      return addr ? PVR_SPM_LOAD_DATA_BUF_ADDR_5 : PVR_SPM_LOAD_DATA_BUF_TEX_5;

   case 6:
      return addr ? PVR_SPM_LOAD_DATA_BUF_ADDR_6 : PVR_SPM_LOAD_DATA_BUF_TEX_6;

   default:
      break;
   }

   UNREACHABLE("");
}

static inline unsigned
pvr_uscgen_spm_load_data_size(struct pvr_spm_load_props *props)
{
   return PVR_SPM_LOAD_DATA_BUF_TEX_0 +
          props->tile_buffer_count * (ROGUE_NUM_TEXSTATE_DWORDS +
                                      (sizeof(uint64_t) / sizeof(uint32_t)));
}

pco_shader *pvr_uscgen_spm_load(pco_ctx *ctx, struct pvr_spm_load_props *props);

#define INDEX(o_r_c, t_b_c, i_m, i)                                        \
   if (props->output_reg_count == o_r_c &&                                 \
       props->tile_buffer_count == t_b_c && props->is_multisampled == i_m) \
   return i

inline static unsigned
pvr_uscgen_spm_load_index(struct pvr_spm_load_props *props)
{
   INDEX(1, 0, false, 0);
   INDEX(2, 0, false, 1);
   INDEX(4, 0, false, 2);

   INDEX(4, 1, false, 3);
   INDEX(4, 2, false, 4);
   INDEX(4, 3, false, 5);
   INDEX(4, 4, false, 6);
   INDEX(4, 5, false, 7);
   INDEX(4, 6, false, 8);
   INDEX(4, 7, false, 9);

   INDEX(1, 0, true, 10);
   INDEX(2, 0, true, 11);
   INDEX(4, 0, true, 12);

   INDEX(4, 1, true, 13);
   INDEX(4, 2, true, 14);
   INDEX(4, 3, true, 15);
   INDEX(4, 4, true, 16);
   INDEX(4, 5, true, 17);
   INDEX(4, 6, true, 18);
   INDEX(4, 7, true, 19);

   UNREACHABLE("Invalid SPM load shader properties.");
}
#undef INDEX

#define PVR_NUM_SPM_LOAD_SHADERS 20U

#endif /* PVR_USC_H */
