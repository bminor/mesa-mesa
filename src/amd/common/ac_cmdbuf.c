/*
 * Copyright 2012 Advanced Micro Devices, Inc.
 * Copyright 2024 Valve Corporation
 *
 * SPDX-License-Identifier: MIT
 */

#include "ac_cmdbuf.h"
#include "ac_pm4.h"
#include "ac_shader_util.h"

#include "sid.h"

#include "util/u_math.h"

#define SI_GS_PER_ES 128

static void
gfx6_init_compute_preamble_state(const struct ac_preamble_state *state,
                                 struct ac_pm4_state *pm4)
{
   const struct radeon_info *info = pm4->info;
   const uint32_t compute_cu_en = S_00B858_SH0_CU_EN(info->spi_cu_en) |
                                  S_00B858_SH1_CU_EN(info->spi_cu_en);

   ac_pm4_set_reg(pm4, R_00B834_COMPUTE_PGM_HI, S_00B834_DATA(info->address32_hi >> 8));

   for (unsigned i = 0; i < 2; ++i)
      ac_pm4_set_reg(pm4, R_00B858_COMPUTE_STATIC_THREAD_MGMT_SE0 + i * 4,
                     i < info->max_se ? compute_cu_en : 0x0);

   if (info->gfx_level >= GFX7) {
      for (unsigned i = 2; i < 4; ++i)
         ac_pm4_set_reg(pm4, R_00B864_COMPUTE_STATIC_THREAD_MGMT_SE2 + (i - 2) * 4,
                        i < info->max_se ? compute_cu_en : 0x0);
   }

   if (info->gfx_level >= GFX9)
      ac_pm4_set_reg(pm4, R_0301EC_CP_COHER_START_DELAY, 0);

   /* Set the pointer to border colors. */
   if (info->gfx_level >= GFX7) {
      ac_pm4_set_reg(pm4, R_030E00_TA_CS_BC_BASE_ADDR, state->border_color_va >> 8);
      ac_pm4_set_reg(pm4, R_030E04_TA_CS_BC_BASE_ADDR_HI,
                     S_030E04_ADDRESS(state->border_color_va >> 40));
   } else if (info->gfx_level == GFX6) {
      ac_pm4_set_reg(pm4, R_00950C_TA_CS_BC_BASE_ADDR, state->border_color_va >> 8);
   }
}

static void
gfx10_init_compute_preamble_state(const struct ac_preamble_state *state,
                                  struct ac_pm4_state *pm4)
{
   const struct radeon_info *info = pm4->info;
   const uint32_t compute_cu_en = S_00B858_SH0_CU_EN(info->spi_cu_en) |
                                  S_00B858_SH1_CU_EN(info->spi_cu_en);

   if (info->gfx_level < GFX11)
      ac_pm4_set_reg(pm4, R_0301EC_CP_COHER_START_DELAY, 0x20);
   ac_pm4_set_reg(pm4, R_030E00_TA_CS_BC_BASE_ADDR, state->border_color_va >> 8);
   ac_pm4_set_reg(pm4, R_030E04_TA_CS_BC_BASE_ADDR_HI, S_030E04_ADDRESS(state->border_color_va >> 40));

   ac_pm4_set_reg(pm4, R_00B834_COMPUTE_PGM_HI, S_00B834_DATA(info->address32_hi >> 8));

   for (unsigned i = 0; i < 2; ++i)
      ac_pm4_set_reg(pm4, R_00B858_COMPUTE_STATIC_THREAD_MGMT_SE0 + i * 4,
                     i < info->max_se ? compute_cu_en : 0x0);

   for (unsigned i = 2; i < 4; ++i)
      ac_pm4_set_reg(pm4, R_00B864_COMPUTE_STATIC_THREAD_MGMT_SE2 + (i - 2) * 4,
                     i < info->max_se ? compute_cu_en : 0x0);

   ac_pm4_set_reg(pm4, R_00B890_COMPUTE_USER_ACCUM_0, 0);
   ac_pm4_set_reg(pm4, R_00B894_COMPUTE_USER_ACCUM_1, 0);
   ac_pm4_set_reg(pm4, R_00B898_COMPUTE_USER_ACCUM_2, 0);
   ac_pm4_set_reg(pm4, R_00B89C_COMPUTE_USER_ACCUM_3, 0);

   if (info->gfx_level >= GFX11) {
      for (unsigned i = 4; i < 8; ++i)
         ac_pm4_set_reg(pm4, R_00B8AC_COMPUTE_STATIC_THREAD_MGMT_SE4 + (i - 4) * 4,
                        i < info->max_se ? compute_cu_en : 0x0);

      /* How many threads should go to 1 SE before moving onto the next. Think of GL1 cache hits.
       * Only these values are valid: 0 (disabled), 64, 128, 256, 512
       * Recommendation: 64 = RT, 256 = non-RT (run benchmarks to be sure)
       */
      ac_pm4_set_reg(pm4, R_00B8BC_COMPUTE_DISPATCH_INTERLEAVE,
                     S_00B8BC_INTERLEAVE(state->gfx11.compute_dispatch_interleave));
   }

   ac_pm4_set_reg(pm4, R_00B9F4_COMPUTE_DISPATCH_TUNNEL, 0);
}

static void
gfx12_init_compute_preamble_state(const struct ac_preamble_state *state,
                                  struct ac_pm4_state *pm4)
{
   const struct radeon_info *info = pm4->info;
   const uint32_t compute_cu_en = S_00B858_SH0_CU_EN(info->spi_cu_en) |
                                  S_00B858_SH1_CU_EN(info->spi_cu_en);
   const uint32_t num_se = info->max_se;

   ac_pm4_set_reg(pm4, R_030E00_TA_CS_BC_BASE_ADDR, state->border_color_va >> 8);
   ac_pm4_set_reg(pm4, R_030E04_TA_CS_BC_BASE_ADDR_HI, S_030E04_ADDRESS(state->border_color_va >> 40));

   ac_pm4_set_reg(pm4, R_00B82C_COMPUTE_PERFCOUNT_ENABLE, 0);
   ac_pm4_set_reg(pm4, R_00B834_COMPUTE_PGM_HI, S_00B834_DATA(info->address32_hi >> 8));
   ac_pm4_set_reg(pm4, R_00B838_COMPUTE_DISPATCH_PKT_ADDR_LO, 0);
   ac_pm4_set_reg(pm4, R_00B83C_COMPUTE_DISPATCH_PKT_ADDR_HI, 0);
   ac_pm4_set_reg(pm4, R_00B858_COMPUTE_STATIC_THREAD_MGMT_SE0, compute_cu_en);
   ac_pm4_set_reg(pm4, R_00B85C_COMPUTE_STATIC_THREAD_MGMT_SE1, num_se > 1 ? compute_cu_en : 0);
   ac_pm4_set_reg(pm4, R_00B864_COMPUTE_STATIC_THREAD_MGMT_SE2, num_se > 2 ? compute_cu_en : 0);
   ac_pm4_set_reg(pm4, R_00B868_COMPUTE_STATIC_THREAD_MGMT_SE3, num_se > 3 ? compute_cu_en : 0);
   ac_pm4_set_reg(pm4, R_00B88C_COMPUTE_STATIC_THREAD_MGMT_SE8, num_se > 8 ? compute_cu_en : 0);
   ac_pm4_set_reg(pm4, R_00B890_COMPUTE_USER_ACCUM_0, 0);
   ac_pm4_set_reg(pm4, R_00B894_COMPUTE_USER_ACCUM_1, 0);
   ac_pm4_set_reg(pm4, R_00B898_COMPUTE_USER_ACCUM_2, 0);
   ac_pm4_set_reg(pm4, R_00B89C_COMPUTE_USER_ACCUM_3, 0);
   ac_pm4_set_reg(pm4, R_00B8AC_COMPUTE_STATIC_THREAD_MGMT_SE4, num_se > 4 ? compute_cu_en : 0);
   ac_pm4_set_reg(pm4, R_00B8B0_COMPUTE_STATIC_THREAD_MGMT_SE5, num_se > 5 ? compute_cu_en : 0);
   ac_pm4_set_reg(pm4, R_00B8B4_COMPUTE_STATIC_THREAD_MGMT_SE6, num_se > 6 ? compute_cu_en : 0);
   ac_pm4_set_reg(pm4, R_00B8B8_COMPUTE_STATIC_THREAD_MGMT_SE7, num_se > 7 ? compute_cu_en : 0);
   ac_pm4_set_reg(pm4, R_00B9F4_COMPUTE_DISPATCH_TUNNEL, 0);
}

static void
cdna_init_compute_preamble_state(const struct ac_preamble_state *state,
                                  struct ac_pm4_state *pm4)
{
   const struct radeon_info *info = pm4->info;
   const uint32_t compute_cu_en = S_00B858_SH0_CU_EN(info->spi_cu_en) |
                                  S_00B858_SH1_CU_EN(info->spi_cu_en);

   /* Compute registers. */
   /* Disable profiling on compute chips. */
   ac_pm4_set_reg(pm4, R_00B82C_COMPUTE_PERFCOUNT_ENABLE, 0);
   ac_pm4_set_reg(pm4, R_00B834_COMPUTE_PGM_HI, S_00B834_DATA(info->address32_hi >> 8));
   ac_pm4_set_reg(pm4, R_00B858_COMPUTE_STATIC_THREAD_MGMT_SE0, compute_cu_en);
   ac_pm4_set_reg(pm4, R_00B85C_COMPUTE_STATIC_THREAD_MGMT_SE1, compute_cu_en);
   ac_pm4_set_reg(pm4, R_00B864_COMPUTE_STATIC_THREAD_MGMT_SE2, compute_cu_en);
   ac_pm4_set_reg(pm4, R_00B868_COMPUTE_STATIC_THREAD_MGMT_SE3, compute_cu_en);
   ac_pm4_set_reg(pm4, R_00B878_COMPUTE_THREAD_TRACE_ENABLE, 0);

   if (info->family >= CHIP_GFX940) {
      ac_pm4_set_reg(pm4, R_00B89C_COMPUTE_TG_CHUNK_SIZE, 0);
      ac_pm4_set_reg(pm4, R_00B8B4_COMPUTE_PGM_RSRC3, 0);
   } else {
      ac_pm4_set_reg(pm4, R_00B894_COMPUTE_STATIC_THREAD_MGMT_SE4, compute_cu_en);
      ac_pm4_set_reg(pm4, R_00B898_COMPUTE_STATIC_THREAD_MGMT_SE5, compute_cu_en);
      ac_pm4_set_reg(pm4, R_00B89C_COMPUTE_STATIC_THREAD_MGMT_SE6, compute_cu_en);
      ac_pm4_set_reg(pm4, R_00B8A0_COMPUTE_STATIC_THREAD_MGMT_SE7, compute_cu_en);
   }

   ac_pm4_set_reg(pm4, R_0301EC_CP_COHER_START_DELAY, 0);

   /* Set the pointer to border colors. Only MI100 supports border colors. */
   if (info->family == CHIP_MI100) {
      ac_pm4_set_reg(pm4, R_030E00_TA_CS_BC_BASE_ADDR, state->border_color_va >> 8);
      ac_pm4_set_reg(pm4, R_030E04_TA_CS_BC_BASE_ADDR_HI,
                     S_030E04_ADDRESS(state->border_color_va >> 40));
   }
}

void
ac_init_compute_preamble_state(const struct ac_preamble_state *state,
                               struct ac_pm4_state *pm4)
{
   const struct radeon_info *info = pm4->info;

   if (!info->has_graphics) {
      cdna_init_compute_preamble_state(state, pm4);
   } else if (info->gfx_level >= GFX12) {
      gfx12_init_compute_preamble_state(state, pm4);
   } else if (info->gfx_level >= GFX10) {
      gfx10_init_compute_preamble_state(state, pm4);
   } else {
      gfx6_init_compute_preamble_state(state, pm4);
   }
}

static void
ac_set_grbm_gfx_index(const struct radeon_info *info, struct ac_pm4_state *pm4, unsigned value)
{
   const unsigned reg = info->gfx_level >= GFX7 ? R_030800_GRBM_GFX_INDEX : R_00802C_GRBM_GFX_INDEX;
   ac_pm4_set_reg(pm4, reg, value);
}

static void
ac_set_grbm_gfx_index_se(const struct radeon_info *info, struct ac_pm4_state *pm4, unsigned se)
{
   assert(se == ~0 || se < info->max_se);
   ac_set_grbm_gfx_index(info, pm4,
                         (se == ~0 ? S_030800_SE_BROADCAST_WRITES(1) : S_030800_SE_INDEX(se)) |
                            S_030800_SH_BROADCAST_WRITES(1) |
                            S_030800_INSTANCE_BROADCAST_WRITES(1));
}

static void
ac_write_harvested_raster_configs(const struct radeon_info *info, struct ac_pm4_state *pm4,
                                  unsigned raster_config, unsigned raster_config_1)
{
   const unsigned num_se = MAX2(info->max_se, 1);
   unsigned raster_config_se[4];
   unsigned se;

   ac_get_harvested_configs(info, raster_config, &raster_config_1, raster_config_se);

   for (se = 0; se < num_se; se++) {
      ac_set_grbm_gfx_index_se(info, pm4, se);
      ac_pm4_set_reg(pm4, R_028350_PA_SC_RASTER_CONFIG, raster_config_se[se]);
   }
   ac_set_grbm_gfx_index(info, pm4, ~0);

   if (info->gfx_level >= GFX7) {
      ac_pm4_set_reg(pm4, R_028354_PA_SC_RASTER_CONFIG_1, raster_config_1);
   }
}

static void
ac_set_raster_config(const struct radeon_info *info, struct ac_pm4_state *pm4)
{
   const unsigned num_rb = MIN2(info->max_render_backends, 16);
   const uint64_t rb_mask = info->enabled_rb_mask;
   unsigned raster_config, raster_config_1;

   ac_get_raster_config(info, &raster_config, &raster_config_1, NULL);

   if (!rb_mask || util_bitcount64(rb_mask) >= num_rb) {
      /* Always use the default config when all backends are enabled
       * (or when we failed to determine the enabled backends).
       */
      ac_pm4_set_reg(pm4, R_028350_PA_SC_RASTER_CONFIG, raster_config);
      if (info->gfx_level >= GFX7)
         ac_pm4_set_reg(pm4, R_028354_PA_SC_RASTER_CONFIG_1, raster_config_1);
   } else {
      ac_write_harvested_raster_configs(info, pm4, raster_config, raster_config_1);
   }
}

static void
gfx6_init_graphics_preamble_state(const struct ac_preamble_state *state,
                                  struct ac_pm4_state *pm4)
{
   const struct radeon_info *info = pm4->info;

   /* Graphics registers. */
   /* CLEAR_STATE doesn't restore these correctly. */
   ac_pm4_set_reg(pm4, R_028240_PA_SC_GENERIC_SCISSOR_TL, S_028240_WINDOW_OFFSET_DISABLE(1));
   ac_pm4_set_reg(pm4, R_028244_PA_SC_GENERIC_SCISSOR_BR,
                  S_028244_BR_X(16384) | S_028244_BR_Y(16384));

   ac_pm4_set_reg(pm4, R_028A18_VGT_HOS_MAX_TESS_LEVEL, fui(64));
   if (!info->has_clear_state)
      ac_pm4_set_reg(pm4, R_028A1C_VGT_HOS_MIN_TESS_LEVEL, fui(0));

   if (!info->has_clear_state) {
      ac_pm4_set_reg(pm4, R_028820_PA_CL_NANINF_CNTL, 0);
      ac_pm4_set_reg(pm4, R_028AC0_DB_SRESULTS_COMPARE_STATE0, 0x0);
      ac_pm4_set_reg(pm4, R_028AC4_DB_SRESULTS_COMPARE_STATE1, 0x0);
      ac_pm4_set_reg(pm4, R_028AC8_DB_PRELOAD_CONTROL, 0x0);
      ac_pm4_set_reg(pm4, R_028A8C_VGT_PRIMITIVEID_RESET, 0x0);
      ac_pm4_set_reg(pm4, R_028B98_VGT_STRMOUT_BUFFER_CONFIG, 0x0);
      ac_pm4_set_reg(pm4, R_028A5C_VGT_GS_PER_VS, 0x2);
      ac_pm4_set_reg(pm4, R_028AB8_VGT_VTX_CNT_EN, 0x0);
   }

   ac_pm4_set_reg(pm4, R_028080_TA_BC_BASE_ADDR, state->border_color_va >> 8);
   if (info->gfx_level >= GFX7)
      ac_pm4_set_reg(pm4, R_028084_TA_BC_BASE_ADDR_HI, S_028084_ADDRESS(state->border_color_va >> 40));

   if (info->gfx_level == GFX6) {
      ac_pm4_set_reg(pm4, R_008A14_PA_CL_ENHANCE,
                     S_008A14_NUM_CLIP_SEQ(3) | S_008A14_CLIP_VTX_REORDER_ENA(1));
   }

   if (info->gfx_level >= GFX7) {
      ac_pm4_set_reg(pm4, R_030A00_PA_SU_LINE_STIPPLE_VALUE, 0);
      ac_pm4_set_reg(pm4, R_030A04_PA_SC_LINE_STIPPLE_STATE, 0);
   } else {
      ac_pm4_set_reg(pm4, R_008A60_PA_SU_LINE_STIPPLE_VALUE, 0);
      ac_pm4_set_reg(pm4, R_008B10_PA_SC_LINE_STIPPLE_STATE, 0);
   }

   if (info->gfx_level <= GFX7 || !info->has_clear_state) {
      ac_pm4_set_reg(pm4, R_028C58_VGT_VERTEX_REUSE_BLOCK_CNTL, 14);
      ac_pm4_set_reg(pm4, R_028C5C_VGT_OUT_DEALLOC_CNTL, 16);

      /* CLEAR_STATE doesn't clear these correctly on certain generations.
       * I don't know why. Deduced by trial and error.
       */
      ac_pm4_set_reg(pm4, R_028204_PA_SC_WINDOW_SCISSOR_TL, S_028204_WINDOW_OFFSET_DISABLE(1));
      ac_pm4_set_reg(pm4, R_028030_PA_SC_SCREEN_SCISSOR_TL, 0);
   }

   if (info->gfx_level >= GFX7) {
      ac_pm4_set_reg_idx3(pm4, R_00B01C_SPI_SHADER_PGM_RSRC3_PS,
                          ac_apply_cu_en(S_00B01C_CU_EN(0xffffffff) |
                                         S_00B01C_WAVE_LIMIT_GFX7(0x3F),
                                         C_00B01C_CU_EN, 0, info));
   }

   if (info->gfx_level <= GFX8) {
      ac_set_raster_config(info, pm4);

      /* FIXME calculate these values somehow ??? */
      ac_pm4_set_reg(pm4, R_028A54_VGT_GS_PER_ES, SI_GS_PER_ES);
      ac_pm4_set_reg(pm4, R_028A58_VGT_ES_PER_GS, 0x40);

      /* These registers, when written, also overwrite the CLEAR_STATE
       * context, so we can't rely on CLEAR_STATE setting them.
       * It would be an issue if there was another UMD changing them.
       */
      ac_pm4_set_reg(pm4, R_028400_VGT_MAX_VTX_INDX, ~0);
      ac_pm4_set_reg(pm4, R_028404_VGT_MIN_VTX_INDX, 0);
      ac_pm4_set_reg(pm4, R_028408_VGT_INDX_OFFSET, 0);
   }

   if (info->gfx_level == GFX9) {
      ac_pm4_set_reg(pm4, R_00B414_SPI_SHADER_PGM_HI_LS,
                     S_00B414_MEM_BASE(info->address32_hi >> 8));
      ac_pm4_set_reg(pm4, R_00B214_SPI_SHADER_PGM_HI_ES,
                     S_00B214_MEM_BASE(info->address32_hi >> 8));
   } else {
      ac_pm4_set_reg(pm4, R_00B524_SPI_SHADER_PGM_HI_LS,
                     S_00B524_MEM_BASE(info->address32_hi >> 8));
   }

   if (info->gfx_level >= GFX7 && info->gfx_level <= GFX8) {
      ac_pm4_set_reg(pm4, R_00B51C_SPI_SHADER_PGM_RSRC3_LS,
                     ac_apply_cu_en(S_00B51C_CU_EN(0xffff) | S_00B51C_WAVE_LIMIT(0x3F),
                                    C_00B51C_CU_EN, 0, info));
      ac_pm4_set_reg(pm4, R_00B41C_SPI_SHADER_PGM_RSRC3_HS, S_00B41C_WAVE_LIMIT(0x3F));
      ac_pm4_set_reg(pm4, R_00B31C_SPI_SHADER_PGM_RSRC3_ES,
                     ac_apply_cu_en(S_00B31C_CU_EN(0xffff) | S_00B31C_WAVE_LIMIT(0x3F),
                                    C_00B31C_CU_EN, 0, info));

      /* If this is 0, Bonaire can hang even if GS isn't being used.
       * Other chips are unaffected. These are suboptimal values,
       * but we don't use on-chip GS.
       */
      ac_pm4_set_reg(pm4, R_028A44_VGT_GS_ONCHIP_CNTL,
                     S_028A44_ES_VERTS_PER_SUBGRP(64) | S_028A44_GS_PRIMS_PER_SUBGRP(4));
   }

   if (info->gfx_level >= GFX8) {
      unsigned vgt_tess_distribution;

      if (info->gfx_level == GFX9) {
         vgt_tess_distribution = S_028B50_ACCUM_ISOLINE(12) |
                                 S_028B50_ACCUM_TRI(30) |
                                 S_028B50_ACCUM_QUAD(24) |
                                 S_028B50_DONUT_SPLIT_GFX9(24) |
                                 S_028B50_TRAP_SPLIT(6);
      } else {
         vgt_tess_distribution = S_028B50_ACCUM_ISOLINE(32) |
                                 S_028B50_ACCUM_TRI(11) |
                                 S_028B50_ACCUM_QUAD(11) |
                                 S_028B50_DONUT_SPLIT_GFX81(16);

         /* Testing with Unigine Heaven extreme tessellation yielded best results
          * with TRAP_SPLIT = 3.
          */
         if (info->family == CHIP_FIJI || info->family >= CHIP_POLARIS10)
            vgt_tess_distribution |= S_028B50_TRAP_SPLIT(3);
      }

      ac_pm4_set_reg(pm4, R_028B50_VGT_TESS_DISTRIBUTION, vgt_tess_distribution);
   }

   ac_pm4_set_reg(pm4, R_028AA0_VGT_INSTANCE_STEP_RATE_0, 1);

   if (info->gfx_level == GFX9) {
      ac_pm4_set_reg(pm4, R_030920_VGT_MAX_VTX_INDX, ~0);
      ac_pm4_set_reg(pm4, R_030924_VGT_MIN_VTX_INDX, 0);
      ac_pm4_set_reg(pm4, R_030928_VGT_INDX_OFFSET, 0);

      ac_pm4_set_reg(pm4, R_028060_DB_DFSM_CONTROL, S_028060_PUNCHOUT_MODE(V_028060_FORCE_OFF));

      ac_pm4_set_reg_idx3(pm4, R_00B41C_SPI_SHADER_PGM_RSRC3_HS,
                          ac_apply_cu_en(S_00B41C_CU_EN(0xffff) | S_00B41C_WAVE_LIMIT(0x3F),
                                         C_00B41C_CU_EN, 0, info));

      ac_pm4_set_reg(pm4, R_028C48_PA_SC_BINNER_CNTL_1,
                     S_028C48_MAX_ALLOC_COUNT(info->pbb_max_alloc_count - 1) |
                     S_028C48_MAX_PRIM_PER_BATCH(1023));

      ac_pm4_set_reg(pm4, R_028AAC_VGT_ESGS_RING_ITEMSIZE, 1);
      ac_pm4_set_reg(pm4, R_030968_VGT_INSTANCE_BASE_ID, 0);
   }
}

static void
gfx10_init_graphics_preamble_state(const struct ac_preamble_state *state,
                                  struct ac_pm4_state *pm4)
{
   const struct radeon_info *info = pm4->info;
   unsigned dcc_write_policy, dcc_read_policy, color_write_policy, color_read_policy;
   unsigned htile_write_policy, htile_read_policy, zs_write_policy, zs_read_policy;
   unsigned cache_no_alloc = info->gfx_level >= GFX11 ? V_02807C_CACHE_NOA_GFX11:
                                                        V_02807C_CACHE_NOA_GFX10;

   if (state->gfx10.cache_cb_gl2) {
      color_write_policy = V_028410_CACHE_LRU_WR;
      color_read_policy = V_028410_CACHE_LRU_RD;
      dcc_write_policy = V_02807C_CACHE_LRU_WR;
      dcc_read_policy = V_02807C_CACHE_LRU_RD;
   } else {
      color_write_policy = V_028410_CACHE_STREAM;
      color_read_policy = cache_no_alloc;

      /* Enable CMASK/DCC caching in L2 for small chips. */
      if (info->max_render_backends <= 4) {
         dcc_write_policy = V_02807C_CACHE_LRU_WR; /* cache writes */
         dcc_read_policy = V_02807C_CACHE_LRU_RD;  /* cache reads */
      } else {
         dcc_write_policy = V_02807C_CACHE_STREAM; /* write combine */
         dcc_read_policy = cache_no_alloc; /* don't cache reads that miss */
      }
   }

   if (state->gfx10.cache_db_gl2) {
      /* Enable caching Z/S surfaces in GL2. It improves performance for GpuTest/Plot3D
       * by 3.2% (no AA) and 3.9% (8x MSAA) on Navi31. This seems to be a good default.
       */
      zs_write_policy = V_028410_CACHE_LRU_WR;
      zs_read_policy = V_028410_CACHE_LRU_RD;
      htile_write_policy = V_028410_CACHE_LRU_WR;
      htile_read_policy = V_028410_CACHE_LRU_RD;
   } else {
      /* Disable caching Z/S surfaces in GL2. It improves performance for GpuTest/FurMark
       * by 1.9%, but not much else.
       */
      zs_write_policy = V_02807C_CACHE_STREAM;
      zs_read_policy = cache_no_alloc;
      htile_write_policy = V_02807C_CACHE_STREAM;
      htile_read_policy = cache_no_alloc;
   }

   const unsigned cu_mask_ps = info->gfx_level >= GFX10_3 ? ac_gfx103_get_cu_mask_ps(info) : ~0u;
   ac_pm4_set_reg_idx3(pm4, R_00B01C_SPI_SHADER_PGM_RSRC3_PS,
                       ac_apply_cu_en(S_00B01C_CU_EN(cu_mask_ps) |
                                      S_00B01C_WAVE_LIMIT_GFX7(0x3F) |
                                      S_00B01C_LDS_GROUP_SIZE_GFX11(info->gfx_level >= GFX11),
                                      C_00B01C_CU_EN, 0, info));
   ac_pm4_set_reg(pm4, R_00B0C0_SPI_SHADER_REQ_CTRL_PS,
                  S_00B0C0_SOFT_GROUPING_EN(1) |
                  S_00B0C0_NUMBER_OF_REQUESTS_PER_CU(4 - 1));
   ac_pm4_set_reg(pm4, R_00B0C8_SPI_SHADER_USER_ACCUM_PS_0, 0);
   ac_pm4_set_reg(pm4, R_00B0CC_SPI_SHADER_USER_ACCUM_PS_1, 0);
   ac_pm4_set_reg(pm4, R_00B0D0_SPI_SHADER_USER_ACCUM_PS_2, 0);
   ac_pm4_set_reg(pm4, R_00B0D4_SPI_SHADER_USER_ACCUM_PS_3, 0);

   if (info->gfx_level < GFX11) {
      /* Shader registers - VS. */
      ac_pm4_set_reg_idx3(pm4, R_00B104_SPI_SHADER_PGM_RSRC4_VS,
                          ac_apply_cu_en(S_00B104_CU_EN(0xffff), /* CUs 16-31 */
                                         C_00B104_CU_EN, 16, info));
      ac_pm4_set_reg(pm4, R_00B1C0_SPI_SHADER_REQ_CTRL_VS, 0);
      ac_pm4_set_reg(pm4, R_00B1C8_SPI_SHADER_USER_ACCUM_VS_0, 0);
      ac_pm4_set_reg(pm4, R_00B1CC_SPI_SHADER_USER_ACCUM_VS_1, 0);
      ac_pm4_set_reg(pm4, R_00B1D0_SPI_SHADER_USER_ACCUM_VS_2, 0);
      ac_pm4_set_reg(pm4, R_00B1D4_SPI_SHADER_USER_ACCUM_VS_3, 0);

      /* Shader registers - PS. */
      unsigned cu_mask_ps = info->gfx_level >= GFX10_3 ? ac_gfx103_get_cu_mask_ps(info) : ~0u;
      ac_pm4_set_reg_idx3(pm4, R_00B004_SPI_SHADER_PGM_RSRC4_PS,
                          ac_apply_cu_en(S_00B004_CU_EN(cu_mask_ps >> 16), /* CUs 16-31 */
                                            C_00B004_CU_EN, 16, info));

      /* Shader registers - HS. */
      ac_pm4_set_reg_idx3(pm4, R_00B404_SPI_SHADER_PGM_RSRC4_HS,
                          ac_apply_cu_en(S_00B404_CU_EN(0xffff), /* CUs 16-31 */
                                         C_00B404_CU_EN, 16, info));
   }

   /* Shader registers - GS. */
   ac_pm4_set_reg(pm4, R_00B2C8_SPI_SHADER_USER_ACCUM_ESGS_0, 0);
   ac_pm4_set_reg(pm4, R_00B2CC_SPI_SHADER_USER_ACCUM_ESGS_1, 0);
   ac_pm4_set_reg(pm4, R_00B2D0_SPI_SHADER_USER_ACCUM_ESGS_2, 0);
   ac_pm4_set_reg(pm4, R_00B2D4_SPI_SHADER_USER_ACCUM_ESGS_3, 0);
   ac_pm4_set_reg(pm4, R_00B324_SPI_SHADER_PGM_HI_ES,
                  S_00B324_MEM_BASE(info->address32_hi >> 8));

   ac_pm4_set_reg_idx3(pm4, R_00B41C_SPI_SHADER_PGM_RSRC3_HS,
                       ac_apply_cu_en(S_00B41C_CU_EN(0xffff) | S_00B41C_WAVE_LIMIT(0x3F),
                                      C_00B41C_CU_EN, 0, info));
   ac_pm4_set_reg(pm4, R_00B4C8_SPI_SHADER_USER_ACCUM_LSHS_0, 0);
   ac_pm4_set_reg(pm4, R_00B4CC_SPI_SHADER_USER_ACCUM_LSHS_1, 0);
   ac_pm4_set_reg(pm4, R_00B4D0_SPI_SHADER_USER_ACCUM_LSHS_2, 0);
   ac_pm4_set_reg(pm4, R_00B4D4_SPI_SHADER_USER_ACCUM_LSHS_3, 0);
   ac_pm4_set_reg(pm4, R_00B524_SPI_SHADER_PGM_HI_LS,
                  S_00B524_MEM_BASE(info->address32_hi >> 8));

   /* Context registers. */
   if (info->gfx_level >= GFX11) {
      /* These are set by CLEAR_STATE on gfx10. We don't use CLEAR_STATE on gfx11. */
      ac_pm4_set_reg(pm4, R_028030_PA_SC_SCREEN_SCISSOR_TL, 0);
      ac_pm4_set_reg(pm4, R_028240_PA_SC_GENERIC_SCISSOR_TL, S_028240_WINDOW_OFFSET_DISABLE(1));
      ac_pm4_set_reg(pm4, R_028244_PA_SC_GENERIC_SCISSOR_BR, S_028244_BR_X(16384) | S_028244_BR_Y(16384));
      ac_pm4_set_reg(pm4, R_02835C_PA_SC_TILE_STEERING_OVERRIDE, info->pa_sc_tile_steering_override);
      ac_pm4_set_reg(pm4, R_0283E4_PA_SC_VRS_RATE_CACHE_CNTL, 0);
      ac_pm4_set_reg(pm4, R_028428_CB_COVERAGE_OUT_CONTROL, 0);
      ac_pm4_set_reg(pm4, R_0286DC_SPI_BARYC_SSAA_CNTL, 0);
      ac_pm4_set_reg(pm4, R_0287D4_PA_CL_POINT_X_RAD, 0);
      ac_pm4_set_reg(pm4, R_0287D8_PA_CL_POINT_Y_RAD, 0);
      ac_pm4_set_reg(pm4, R_0287DC_PA_CL_POINT_SIZE, 0);
      ac_pm4_set_reg(pm4, R_0287E0_PA_CL_POINT_CULL_RAD, 0);
      ac_pm4_set_reg(pm4, R_028820_PA_CL_NANINF_CNTL, 0);
      ac_pm4_set_reg(pm4, R_028824_PA_SU_LINE_STIPPLE_CNTL, 0);
      ac_pm4_set_reg(pm4, R_02883C_PA_SU_OVER_RASTERIZATION_CNTL, 0);
      ac_pm4_set_reg(pm4, R_028840_PA_STEREO_CNTL, 0);
      ac_pm4_set_reg(pm4, R_028A50_VGT_ENHANCE, 0);
      ac_pm4_set_reg(pm4, R_028A8C_VGT_PRIMITIVEID_RESET, 0);
      ac_pm4_set_reg(pm4, R_028AB4_VGT_REUSE_OFF, 0);
      ac_pm4_set_reg(pm4, R_028C40_PA_SC_SHADER_CONTROL, 0);
   }

   if (info->gfx_level < GFX11) {
      ac_pm4_set_reg(pm4, R_028038_DB_DFSM_CONTROL, S_028038_PUNCHOUT_MODE(V_028038_FORCE_OFF));
   }

   ac_pm4_set_reg(pm4, R_02807C_DB_RMI_L2_CACHE_CONTROL,
                  S_02807C_Z_WR_POLICY(zs_write_policy) |
                  S_02807C_S_WR_POLICY(zs_write_policy) |
                  S_02807C_HTILE_WR_POLICY(htile_write_policy) |
                  S_02807C_ZPCPSD_WR_POLICY(V_02807C_CACHE_STREAM) | /* occlusion query writes */
                  S_02807C_Z_RD_POLICY(zs_read_policy) |
                  S_02807C_S_RD_POLICY(zs_read_policy) |
                  S_02807C_HTILE_RD_POLICY(htile_read_policy));
   ac_pm4_set_reg(pm4, R_028080_TA_BC_BASE_ADDR, state->border_color_va >> 8);
   ac_pm4_set_reg(pm4, R_028084_TA_BC_BASE_ADDR_HI, S_028084_ADDRESS(state->border_color_va >> 40));

   ac_pm4_set_reg(pm4, R_028410_CB_RMI_GL2_CACHE_CONTROL,
                  (info->gfx_level >= GFX11 ?
                      S_028410_COLOR_WR_POLICY_GFX11(color_write_policy) |
                      S_028410_COLOR_RD_POLICY(color_read_policy) |
                      S_028410_DCC_WR_POLICY_GFX11(dcc_write_policy) |
                      S_028410_DCC_RD_POLICY(dcc_read_policy)
                    :
                      S_028410_COLOR_WR_POLICY_GFX10(color_write_policy) |
                      S_028410_COLOR_RD_POLICY(color_read_policy)) |
                      S_028410_FMASK_WR_POLICY(color_write_policy) |
                      S_028410_FMASK_RD_POLICY(color_read_policy) |
                      S_028410_CMASK_WR_POLICY(dcc_write_policy) |
                      S_028410_CMASK_RD_POLICY(dcc_read_policy) |
                      S_028410_DCC_WR_POLICY_GFX10(dcc_write_policy) |
                      S_028410_DCC_RD_POLICY(dcc_read_policy));

   if (info->gfx_level >= GFX10_3)
      ac_pm4_set_reg(pm4, R_028750_SX_PS_DOWNCONVERT_CONTROL, 0xff);

   ac_pm4_set_reg(pm4, R_028830_PA_SU_SMALL_PRIM_FILTER_CNTL,
                  S_028830_SMALL_PRIM_FILTER_ENABLE(1));

   ac_pm4_set_reg(pm4, R_028A18_VGT_HOS_MAX_TESS_LEVEL, fui(64));
   if (info->gfx_level >= GFX11) /* cleared by CLEAR_STATE on gfx10 */
      ac_pm4_set_reg(pm4, R_028A1C_VGT_HOS_MIN_TESS_LEVEL, fui(0));
   ac_pm4_set_reg(pm4, R_028AAC_VGT_ESGS_RING_ITEMSIZE, 1);
   ac_pm4_set_reg(pm4, R_028B50_VGT_TESS_DISTRIBUTION,
                  info->gfx_level >= GFX11 ?
                     S_028B50_ACCUM_ISOLINE(128) |
                     S_028B50_ACCUM_TRI(128) |
                     S_028B50_ACCUM_QUAD(128) |
                     S_028B50_DONUT_SPLIT_GFX9(24) |
                     S_028B50_TRAP_SPLIT(6)
                   :
                     S_028B50_ACCUM_ISOLINE(12) |
                     S_028B50_ACCUM_TRI(30) |
                     S_028B50_ACCUM_QUAD(24) |
                     S_028B50_DONUT_SPLIT_GFX9(24) |
                     S_028B50_TRAP_SPLIT(6));

   /* GFX11+ shouldn't subtract 1 from pbb_max_alloc_count.  */
   unsigned gfx10_one = info->gfx_level < GFX11;
   ac_pm4_set_reg(pm4, R_028C48_PA_SC_BINNER_CNTL_1,
                  S_028C48_MAX_ALLOC_COUNT(info->pbb_max_alloc_count - gfx10_one) |
                  S_028C48_MAX_PRIM_PER_BATCH(1023));
   if (info->gfx_level >= GFX11) {
      ac_pm4_set_reg(pm4, R_028C54_PA_SC_BINNER_CNTL_2,
                     S_028C54_ENABLE_PING_PONG_BIN_ORDER(info->gfx_level >= GFX11_5));
   }

   /* Break up a pixel wave if it contains deallocs for more than
    * half the parameter cache.
    *
    * To avoid a deadlock where pixel waves aren't launched
    * because they're waiting for more pixels while the frontend
    * is stuck waiting for PC space, the maximum allowed value is
    * the size of the PC minus the largest possible allocation for
    * a single primitive shader subgroup.
    */
   ac_pm4_set_reg(pm4, R_028C50_PA_SC_NGG_MODE_CNTL,
                  S_028C50_MAX_DEALLOCS_IN_WAVE(info->gfx_level >= GFX11 ? 16 : 512));
   if (info->gfx_level < GFX11)
      ac_pm4_set_reg(pm4, R_028C58_VGT_VERTEX_REUSE_BLOCK_CNTL, 14); /* Reuse for legacy (non-NGG) only. */

   /* Uconfig registers. */
   ac_pm4_set_reg(pm4, R_030924_GE_MIN_VTX_INDX, 0);
   ac_pm4_set_reg(pm4, R_030928_GE_INDX_OFFSET, 0);
   if (info->gfx_level >= GFX11) {
      /* This is changed by draws for indexed draws, but we need to set DISABLE_FOR_AUTO_INDEX
       * here, which disables primitive restart for all non-indexed draws, so that those draws
       * won't have to set this state.
       */
      ac_pm4_set_reg(pm4, R_03092C_GE_MULTI_PRIM_IB_RESET_EN, S_03092C_DISABLE_FOR_AUTO_INDEX(1));
   }
   ac_pm4_set_reg(pm4, R_030964_GE_MAX_VTX_INDX, ~0);
   ac_pm4_set_reg(pm4, R_030968_VGT_INSTANCE_BASE_ID, 0);
   ac_pm4_set_reg(pm4, R_03097C_GE_STEREO_CNTL, 0);
   ac_pm4_set_reg(pm4, R_030988_GE_USER_VGPR_EN, 0);

   ac_pm4_set_reg(pm4, R_030A00_PA_SU_LINE_STIPPLE_VALUE, 0);
   ac_pm4_set_reg(pm4, R_030A04_PA_SC_LINE_STIPPLE_STATE, 0);

   if (info->gfx_level >= GFX11) {
      uint64_t rb_mask = BITFIELD64_MASK(info->max_render_backends);

      ac_pm4_cmd_add(pm4, PKT3(PKT3_EVENT_WRITE, 2, 0));
      ac_pm4_cmd_add(pm4, EVENT_TYPE(V_028A90_PIXEL_PIPE_STAT_CONTROL) | EVENT_INDEX(1));
      ac_pm4_cmd_add(pm4, PIXEL_PIPE_STATE_CNTL_COUNTER_ID(0) |
                          PIXEL_PIPE_STATE_CNTL_STRIDE(2) |
                          PIXEL_PIPE_STATE_CNTL_INSTANCE_EN_LO(rb_mask));
      ac_pm4_cmd_add(pm4, PIXEL_PIPE_STATE_CNTL_INSTANCE_EN_HI(rb_mask));
   }
}

static void
gfx12_init_graphics_preamble_state(const struct ac_preamble_state *state,
                                  struct ac_pm4_state *pm4)
{
   const struct radeon_info *info = pm4->info;
   unsigned color_write_policy, color_read_policy;
   enum gfx12_store_temporal_hint color_write_temporal_hint, zs_write_temporal_hint;
   enum gfx12_load_temporal_hint color_read_temporal_hint, zs_read_temporal_hint;

   if (state->gfx10.cache_cb_gl2) {
      color_write_policy = V_028410_CACHE_LRU_WR;
      color_read_policy = V_028410_CACHE_LRU_RD;
      color_write_temporal_hint = gfx12_store_regular_temporal;
      color_read_temporal_hint = gfx12_load_regular_temporal;
   } else {
      color_write_policy = V_028410_CACHE_STREAM;
      color_read_policy = V_02807C_CACHE_NOA_GFX11;
      color_write_temporal_hint = gfx12_store_near_non_temporal_far_regular_temporal;
      color_read_temporal_hint = gfx12_load_near_non_temporal_far_regular_temporal;
   }

   if (state->gfx10.cache_db_gl2) {
      zs_write_temporal_hint = gfx12_store_regular_temporal;
      zs_read_temporal_hint = gfx12_load_regular_temporal;
   } else {
      zs_write_temporal_hint = gfx12_store_near_non_temporal_far_regular_temporal;
      zs_read_temporal_hint = gfx12_load_near_non_temporal_far_regular_temporal;
   }

   /* Shader registers - PS */
   ac_pm4_set_reg_idx3(pm4, R_00B018_SPI_SHADER_PGM_RSRC3_PS,
                       ac_apply_cu_en(S_00B018_CU_EN(0xffff),
                                      C_00B018_CU_EN, 0, info));
   ac_pm4_set_reg(pm4, R_00B0C0_SPI_SHADER_REQ_CTRL_PS,
                  S_00B0C0_SOFT_GROUPING_EN(1) |
                  S_00B0C0_NUMBER_OF_REQUESTS_PER_CU(4 - 1));
   ac_pm4_set_reg(pm4, R_00B0C8_SPI_SHADER_USER_ACCUM_PS_0, 0);
   ac_pm4_set_reg(pm4, R_00B0CC_SPI_SHADER_USER_ACCUM_PS_1, 0);
   ac_pm4_set_reg(pm4, R_00B0D0_SPI_SHADER_USER_ACCUM_PS_2, 0);
   ac_pm4_set_reg(pm4, R_00B0D4_SPI_SHADER_USER_ACCUM_PS_3, 0);

   /* Shader registers - GS */
   ac_pm4_set_reg(pm4, R_00B218_SPI_SHADER_PGM_HI_ES,
                  S_00B324_MEM_BASE(info->address32_hi >> 8));
   ac_pm4_set_reg_idx3(pm4, R_00B21C_SPI_SHADER_PGM_RSRC3_GS,
                       ac_apply_cu_en(0xfffffdfd, 0, 0, info));
   ac_pm4_set_reg(pm4, R_00B2C8_SPI_SHADER_USER_ACCUM_ESGS_0, 0);
   ac_pm4_set_reg(pm4, R_00B2CC_SPI_SHADER_USER_ACCUM_ESGS_1, 0);
   ac_pm4_set_reg(pm4, R_00B2D0_SPI_SHADER_USER_ACCUM_ESGS_2, 0);
   ac_pm4_set_reg(pm4, R_00B2D4_SPI_SHADER_USER_ACCUM_ESGS_3, 0);

   /* Shader registers - HS */
   ac_pm4_set_reg(pm4, R_00B418_SPI_SHADER_PGM_HI_LS,
                  S_00B524_MEM_BASE(info->address32_hi >> 8));
   ac_pm4_set_reg_idx3(pm4, R_00B41C_SPI_SHADER_PGM_RSRC3_HS,
                       ac_apply_cu_en(0xffffffff, 0, 0, info));
   ac_pm4_set_reg(pm4, R_00B4C8_SPI_SHADER_USER_ACCUM_LSHS_0, 0);
   ac_pm4_set_reg(pm4, R_00B4CC_SPI_SHADER_USER_ACCUM_LSHS_1, 0);
   ac_pm4_set_reg(pm4, R_00B4D0_SPI_SHADER_USER_ACCUM_LSHS_2, 0);
   ac_pm4_set_reg(pm4, R_00B4D4_SPI_SHADER_USER_ACCUM_LSHS_3, 0);

   /* Shader registers - PS */
   ac_pm4_set_reg(pm4, R_00B024_SPI_SHADER_PGM_HI_PS,
                  S_00B024_MEM_BASE(info->address32_hi >> 8));

   /* Context registers */
   ac_pm4_set_reg(pm4, R_028040_DB_GL1_INTERFACE_CONTROL, 0);
   ac_pm4_set_reg(pm4, R_028048_DB_MEM_TEMPORAL,
                  S_028048_Z_TEMPORAL_READ(zs_read_temporal_hint) |
                  S_028048_Z_TEMPORAL_WRITE(zs_write_temporal_hint) |
                  S_028048_STENCIL_TEMPORAL_READ(zs_read_temporal_hint) |
                  S_028048_STENCIL_TEMPORAL_WRITE(zs_write_temporal_hint) |
                  S_028048_OCCLUSION_TEMPORAL_WRITE(gfx12_store_regular_temporal));
   ac_pm4_set_reg(pm4, R_028064_DB_VIEWPORT_CONTROL, 0);
   ac_pm4_set_reg(pm4, R_028068_DB_SPI_VRS_CENTER_LOCATION, 0);
   ac_pm4_set_reg(pm4, R_028080_TA_BC_BASE_ADDR, state->border_color_va >> 8);
   ac_pm4_set_reg(pm4, R_028084_TA_BC_BASE_ADDR_HI, S_028084_ADDRESS(state->border_color_va >> 40));
   ac_pm4_set_reg(pm4, R_02808C_DB_STENCIL_OPVAL, S_02808C_OPVAL(1) | S_02808C_OPVAL_BF(1));
   ac_pm4_set_reg(pm4, R_0280F8_SC_MEM_TEMPORAL,
                  S_0280F8_VRS_TEMPORAL_READ(gfx12_load_regular_temporal) |
                  S_0280F8_VRS_TEMPORAL_WRITE(gfx12_store_regular_temporal) |
                  S_0280F8_HIZ_TEMPORAL_READ(gfx12_load_regular_temporal) |
                  S_0280F8_HIZ_TEMPORAL_WRITE(gfx12_store_regular_temporal) |
                  S_0280F8_HIS_TEMPORAL_READ(gfx12_load_regular_temporal) |
                  S_0280F8_HIS_TEMPORAL_WRITE(gfx12_store_regular_temporal));
   ac_pm4_set_reg(pm4, R_0280FC_SC_MEM_SPEC_READ,
                  S_0280FC_VRS_SPECULATIVE_READ(gfx12_spec_read_force_on) |
                  S_0280FC_HIZ_SPECULATIVE_READ(gfx12_spec_read_force_on) |
                  S_0280FC_HIS_SPECULATIVE_READ(gfx12_spec_read_force_on));

   /* We don't need to initialize PA_SC_VPORT_* because we don't enable
    * IMPLICIT_VPORT_SCISSOR_ENABLE, but it might be useful for Vulkan.
    *
    * If you set IMPLICIT_VPORT_SCISSOR_ENABLE, PA_SC_VPORT_* will take effect and allows
    * setting a scissor that covers the whole viewport. If you set VPORT_SCISSOR_ENABLE,
    * PA_SC_VPORT_SCISSOR_* will take effect and allows setting a user scissor. If you set
    * both enable bits, the hw will use the intersection of both. It allows separating implicit
    * viewport scissors from user scissors.
    */
   ac_pm4_set_reg(pm4, R_028180_PA_SC_SCREEN_SCISSOR_TL, 0);
   ac_pm4_set_reg(pm4, R_028184_PA_SC_SCREEN_SCISSOR_BR,
                  S_028184_BR_X(65535) | S_028184_BR_Y(65535)); /* inclusive bounds */
   ac_pm4_set_reg(pm4, R_028204_PA_SC_WINDOW_SCISSOR_TL, 0);
   ac_pm4_set_reg(pm4, R_028240_PA_SC_GENERIC_SCISSOR_TL, 0);
   ac_pm4_set_reg(pm4, R_028244_PA_SC_GENERIC_SCISSOR_BR,
                  S_028244_BR_X(65535) | S_028244_BR_Y(65535)); /* inclusive bounds */
   ac_pm4_set_reg(pm4, R_028358_PA_SC_SCREEN_EXTENT_CONTROL, 0);
   ac_pm4_set_reg(pm4, R_02835C_PA_SC_TILE_STEERING_OVERRIDE,
                  info->pa_sc_tile_steering_override);
   ac_pm4_set_reg(pm4, R_0283E0_PA_SC_VRS_INFO, 0);

   ac_pm4_set_reg(pm4, R_028410_CB_RMI_GL2_CACHE_CONTROL,
                  S_028410_COLOR_WR_POLICY_GFX11(color_write_policy) |
                  S_028410_COLOR_RD_POLICY(color_read_policy));
   ac_pm4_set_reg(pm4, R_0286E4_SPI_BARYC_SSAA_CNTL, S_0286E4_COVERED_CENTROID_IS_CENTER(1));
   ac_pm4_set_reg(pm4, R_028750_SX_PS_DOWNCONVERT_CONTROL, 0xff);
   ac_pm4_set_reg(pm4, R_0287D4_PA_CL_POINT_X_RAD, 0);
   ac_pm4_set_reg(pm4, R_0287D8_PA_CL_POINT_Y_RAD, 0);
   ac_pm4_set_reg(pm4, R_0287DC_PA_CL_POINT_SIZE, 0);
   ac_pm4_set_reg(pm4, R_0287E0_PA_CL_POINT_CULL_RAD, 0);
   ac_pm4_set_reg(pm4, R_028820_PA_CL_NANINF_CNTL, 0);
   ac_pm4_set_reg(pm4, R_028824_PA_SU_LINE_STIPPLE_CNTL, 0);
   ac_pm4_set_reg(pm4, R_028828_PA_SU_LINE_STIPPLE_SCALE, 0);
   ac_pm4_set_reg(pm4, R_028830_PA_SU_SMALL_PRIM_FILTER_CNTL,
                  S_028830_SMALL_PRIM_FILTER_ENABLE(1) |
                  S_028830_SC_1XMSAA_COMPATIBLE_DISABLE(1) /* use sample locations even for MSAA 1x */);
   ac_pm4_set_reg(pm4, R_02883C_PA_SU_OVER_RASTERIZATION_CNTL, 0);
   ac_pm4_set_reg(pm4, R_028840_PA_STEREO_CNTL, S_028840_STEREO_MODE(1));

   ac_pm4_set_reg(pm4, R_028A18_VGT_HOS_MAX_TESS_LEVEL, fui(64));
   ac_pm4_set_reg(pm4, R_028A1C_VGT_HOS_MIN_TESS_LEVEL, fui(0));
   ac_pm4_set_reg(pm4, R_028A50_GE_SE_ENHANCE, 0);
   ac_pm4_set_reg(pm4, R_028A70_GE_IA_ENHANCE, 0);
   ac_pm4_set_reg(pm4, R_028A80_GE_WD_ENHANCE, 0);
   ac_pm4_set_reg(pm4, R_028A9C_VGT_REUSE_OFF, 0);
   ac_pm4_set_reg(pm4, R_028AA0_VGT_DRAW_PAYLOAD_CNTL, 0);
   ac_pm4_set_reg(pm4, R_028ABC_DB_HTILE_SURFACE, 0);

   ac_pm4_set_reg(pm4, R_028B50_VGT_TESS_DISTRIBUTION,
                  S_028B50_ACCUM_ISOLINE(128) |
                  S_028B50_ACCUM_TRI(128) |
                  S_028B50_ACCUM_QUAD(128) |
                  S_028B50_DONUT_SPLIT_GFX9(24) |
                  S_028B50_TRAP_SPLIT(6));
   ac_pm4_set_reg(pm4, R_028BC0_PA_SC_HISZ_RENDER_OVERRIDE, 0);

   ac_pm4_set_reg(pm4, R_028C40_PA_SC_BINNER_OUTPUT_TIMEOUT_COUNTER, 0x800);
   ac_pm4_set_reg(pm4, R_028C48_PA_SC_BINNER_CNTL_1,
                  S_028C48_MAX_ALLOC_COUNT(254) |
                  S_028C48_MAX_PRIM_PER_BATCH(511));
   ac_pm4_set_reg(pm4, R_028C4C_PA_SC_BINNER_CNTL_2, S_028C4C_ENABLE_PING_PONG_BIN_ORDER(1));
   ac_pm4_set_reg(pm4, R_028C50_PA_SC_NGG_MODE_CNTL, S_028C50_MAX_DEALLOCS_IN_WAVE(64));
   ac_pm4_set_reg(pm4, R_028C58_PA_SC_SHADER_CONTROL,
                  S_028C58_REALIGN_DQUADS_AFTER_N_WAVES(1));

   for (unsigned i = 0; i < 8; i++) {
      ac_pm4_set_reg(pm4, R_028F00_CB_MEM0_INFO + i * 4,
                     S_028F00_TEMPORAL_READ(color_read_temporal_hint) |
                     S_028F00_TEMPORAL_WRITE(color_write_temporal_hint));
   }

   /* Uconfig registers. */
   ac_pm4_set_reg(pm4, R_030924_GE_MIN_VTX_INDX, 0);
   ac_pm4_set_reg(pm4, R_030928_GE_INDX_OFFSET, 0);
   /* This is changed by draws for indexed draws, but we need to set DISABLE_FOR_AUTO_INDEX
    * here, which disables primitive restart for all non-indexed draws, so that those draws
    * won't have to set this state.
    */
   ac_pm4_set_reg(pm4, R_03092C_GE_MULTI_PRIM_IB_RESET_EN, S_03092C_DISABLE_FOR_AUTO_INDEX(1));
   ac_pm4_set_reg(pm4, R_030950_GE_GS_THROTTLE,
                  S_030950_T0(0x1) |
                  S_030950_T1(0x4) |
                  S_030950_T2(0x3) |
                  S_030950_STALL_CYCLES(0x40) |
                  S_030950_FACTOR1(0x2) |
                  S_030950_FACTOR2(0x3) |
                  S_030950_ENABLE_THROTTLE(0) |
                  S_030950_NUM_INIT_GRPS(0xff));
   ac_pm4_set_reg(pm4, R_030964_GE_MAX_VTX_INDX, ~0);
   ac_pm4_set_reg(pm4, R_030968_VGT_INSTANCE_BASE_ID, 0);
   ac_pm4_set_reg(pm4, R_03097C_GE_STEREO_CNTL, 0);
   ac_pm4_set_reg(pm4, R_030980_GE_USER_VGPR_EN, 0);
   ac_pm4_set_reg(pm4, R_0309B4_VGT_PRIMITIVEID_RESET, 0);
   ac_pm4_set_reg(pm4, R_03098C_GE_VRS_RATE, 0);
   ac_pm4_set_reg(pm4, R_030A00_PA_SU_LINE_STIPPLE_VALUE, 0);
   ac_pm4_set_reg(pm4, R_030A04_PA_SC_LINE_STIPPLE_STATE, 0);

   /* On GFX12, this seems to behave slightly differently. Programming the
    * EXCLUSION fields to TRUE causes zero-area triangles to not pass the
    * primitive clipping stage.
    */
   ac_pm4_set_reg(pm4, R_02882C_PA_SU_PRIM_FILTER_CNTL, 0);

   ac_pm4_set_reg(pm4, R_031128_SPI_GRP_LAUNCH_GUARANTEE_ENABLE,
                  S_031128_ENABLE(1) |
                  S_031128_GS_ASSIST_EN(1) |
                  S_031128_MRT_ASSIST_EN(1) |
                  S_031128_GFX_NUM_LOCK_WGP(2) |
                  S_031128_CS_NUM_LOCK_WGP(2) |
                  S_031128_LOCK_PERIOD(1) |
                  S_031128_LOCK_MAINT_COUNT(1));
   ac_pm4_set_reg(pm4, R_03112C_SPI_GRP_LAUNCH_GUARANTEE_CTRL,
                  S_03112C_NUM_MRT_THRESHOLD(3) |
                  S_03112C_GFX_PENDING_THRESHOLD(4) |
                  S_03112C_PRIORITY_LOST_THRESHOLD(4) |
                  S_03112C_ALLOC_SUCCESS_THRESHOLD(4) |
                  S_03112C_CS_WAVE_THRESHOLD_HIGH(8));

   uint64_t rb_mask = BITFIELD64_MASK(info->max_render_backends);

   ac_pm4_cmd_add(pm4, PKT3(PKT3_EVENT_WRITE, 2, 0));
   ac_pm4_cmd_add(pm4, EVENT_TYPE(V_028A90_PIXEL_PIPE_STAT_CONTROL) | EVENT_INDEX(1));
   ac_pm4_cmd_add(pm4, PIXEL_PIPE_STATE_CNTL_COUNTER_ID(0) |
                       PIXEL_PIPE_STATE_CNTL_STRIDE(2) |
                       PIXEL_PIPE_STATE_CNTL_INSTANCE_EN_LO(rb_mask));
   ac_pm4_cmd_add(pm4, PIXEL_PIPE_STATE_CNTL_INSTANCE_EN_HI(rb_mask));
}

void
ac_init_graphics_preamble_state(const struct ac_preamble_state *state,
                               struct ac_pm4_state *pm4)
{
   const struct radeon_info *info = pm4->info;

   if (info->gfx_level >= GFX12) {
      gfx12_init_graphics_preamble_state(state, pm4);
   } else if (info->gfx_level >= GFX10) {
      gfx10_init_graphics_preamble_state(state, pm4);
   } else {
      gfx6_init_graphics_preamble_state(state, pm4);
   }
}

void
ac_emit_cond_exec(struct ac_cmdbuf *cs, enum amd_gfx_level gfx_level,
                  uint64_t va, uint32_t count)
{
   ac_cmdbuf_begin(cs);
   if (gfx_level >= GFX7) {
      ac_cmdbuf_emit(PKT3(PKT3_COND_EXEC, 3, 0));
      ac_cmdbuf_emit(va);
      ac_cmdbuf_emit(va >> 32);
      ac_cmdbuf_emit(0);
      ac_cmdbuf_emit(count);
   } else {
      ac_cmdbuf_emit(PKT3(PKT3_COND_EXEC, 2, 0));
      ac_cmdbuf_emit(va);
      ac_cmdbuf_emit(va >> 32);
      ac_cmdbuf_emit(count);
   }
   ac_cmdbuf_end();
}

void
ac_emit_write_data_imm(struct ac_cmdbuf *cs, unsigned engine_sel, uint64_t va, uint32_t value)
{
   ac_cmdbuf_begin(cs);
   ac_cmdbuf_emit(PKT3(PKT3_WRITE_DATA, 3, 0));
   ac_cmdbuf_emit(S_370_DST_SEL(V_370_MEM) | S_370_WR_CONFIRM(1) | S_370_ENGINE_SEL(engine_sel));
   ac_cmdbuf_emit(va);
   ac_cmdbuf_emit(va >> 32);
   ac_cmdbuf_emit(value);
   ac_cmdbuf_end();
}

void
ac_emit_cp_wait_mem(struct ac_cmdbuf *cs, uint64_t va, uint32_t ref,
                    uint32_t mask, unsigned flags)
{
   ac_cmdbuf_begin(cs);
   ac_cmdbuf_emit(PKT3(PKT3_WAIT_REG_MEM, 5, 0));
   ac_cmdbuf_emit(WAIT_REG_MEM_MEM_SPACE(1) | flags);
   ac_cmdbuf_emit(va);
   ac_cmdbuf_emit(va >> 32);
   ac_cmdbuf_emit(ref);  /* reference value */
   ac_cmdbuf_emit(mask); /* mask */
   ac_cmdbuf_emit(4);    /* poll interval */
   ac_cmdbuf_end();
}

static bool
is_ts_event(unsigned event_type)
{
   return event_type == V_028A90_CACHE_FLUSH_TS ||
          event_type == V_028A90_CACHE_FLUSH_AND_INV_TS_EVENT ||
          event_type == V_028A90_BOTTOM_OF_PIPE_TS ||
          event_type == V_028A90_FLUSH_AND_INV_DB_DATA_TS ||
          event_type == V_028A90_FLUSH_AND_INV_CB_DATA_TS;
}

/* This will wait or insert into the pipeline a wait for a previous
 * RELEASE_MEM PWS event.
 *
 * "event_type" must be the same as the RELEASE_MEM PWS event.
 *
 * "stage_sel" determines when the waiting happens. It can be CP_PFP, CP_ME,
 * PRE_SHADER, PRE_DEPTH, or PRE_PIX_SHADER, allowing to wait later in the
 * pipeline instead of completely idling the hw at the frontend.
 *
 * "gcr_cntl" must be 0 if not waiting in PFP or ME. When waiting later in the
 * pipeline, any cache flushes must be part of RELEASE_MEM, not ACQUIRE_MEM.
 *
 * "distance" determines how many RELEASE_MEM PWS events ago it should wait
 * for, minus one (starting from 0). There are 3 event types: PS_DONE,
 * CS_DONE, and TS events. The distance counter increments separately for each
 * type, so 0 with PS_DONE means wait for the last PS_DONE event, while 0 with
 * *_TS means wait for the last TS event (even if it's a different TS event
 * because all TS events share the same counter).
 *
 * PRE_SHADER waits before the first shader that has IMAGE_OP=1, while
 * PRE_PIX_SHADER waits before PS if it has IMAGE_OP=1 (IMAGE_OP should really
 * be called SYNC_ENABLE) PRE_DEPTH waits before depth/stencil tests.
 *
 * PRE_COLOR also exists but shouldn't be used because it can hang. It's
 * recommended to use PRE_PIX_SHADER instead, which means all PS that have
 * color exports with enabled color buffers, non-zero colormask, and non-zero
 * sample mask must have IMAGE_OP=1 to enable the sync before PS.
 *
 * Waiting for a PWS fence that was generated by a previous IB is valid, but
 * if there is an IB from another process in between and that IB also inserted
 * a PWS fence, the hw will wait for the newer fence instead because the PWS
 * counter was incremented.
 */
void
ac_emit_cp_acquire_mem_pws(struct ac_cmdbuf *cs, ASSERTED enum amd_gfx_level gfx_level,
                           ASSERTED enum amd_ip_type ip_type, uint32_t event_type,
                           uint32_t stage_sel, uint32_t count,
                           uint32_t gcr_cntl)
{
   assert(gfx_level >= GFX11 && ip_type == AMD_IP_GFX);

   const bool ts = is_ts_event(event_type);
   const bool ps_done = event_type == V_028A90_PS_DONE;
   const bool cs_done = event_type == V_028A90_CS_DONE;
   const uint32_t counter_sel = ts ? V_580_TS_SELECT : ps_done ? V_580_PS_SELECT : V_580_CS_SELECT;

   assert((int)ts + (int)cs_done + (int)ps_done == 1);
   assert(!gcr_cntl || stage_sel == V_580_CP_PFP || stage_sel == V_580_CP_ME);
   assert(stage_sel != V_580_PRE_COLOR);

   ac_cmdbuf_begin(cs);
   ac_cmdbuf_emit(PKT3(PKT3_ACQUIRE_MEM, 6, 0));
   ac_cmdbuf_emit(S_580_PWS_STAGE_SEL(stage_sel) |
                  S_580_PWS_COUNTER_SEL(counter_sel) |
                  S_580_PWS_ENA2(1) |
                  S_580_PWS_COUNT(count));
   ac_cmdbuf_emit(0xffffffff); /* GCR_SIZE */
   ac_cmdbuf_emit(0x01ffffff); /* GCR_SIZE_HI */
   ac_cmdbuf_emit(0);          /* GCR_BASE_LO */
   ac_cmdbuf_emit(0);          /* GCR_BASE_HI */
   ac_cmdbuf_emit(S_585_PWS_ENA(1));
   ac_cmdbuf_emit(gcr_cntl); /* GCR_CNTL (this has no effect if PWS_STAGE_SEL isn't PFP or ME) */
   ac_cmdbuf_end();
}

/* Insert CS_DONE, PS_DONE, or a *_TS event into the pipeline, which will
 * signal after the work indicated by the event is complete, which optionally
 * includes flushing caches using "gcr_cntl" after the completion of the work.
 * *_TS events are always signaled at the end of the pipeline, while CS_DONE
 * and PS_DONE are signaled when those shaders finish. This call only inserts
 * the event into the pipeline. It doesn't wait for anything and it doesn't
 * execute anything immediately. The only way to wait for the event completion
 * is to call si_cp_acquire_mem_pws with the same "event_type".
 */
void
ac_emit_cp_release_mem_pws(struct ac_cmdbuf *cs, ASSERTED enum amd_gfx_level gfx_level,
                           ASSERTED enum amd_ip_type ip_type, uint32_t event_type,
                           uint32_t gcr_cntl)
{
   assert(gfx_level >= GFX11 && ip_type == AMD_IP_GFX);

   /* Extract GCR_CNTL fields because the encoding is different in RELEASE_MEM. */
   assert(G_586_GLI_INV(gcr_cntl) == 0);
   assert(G_586_GL1_RANGE(gcr_cntl) == 0);
   const uint32_t glm_wb = G_586_GLM_WB(gcr_cntl);
   const uint32_t glm_inv = G_586_GLM_INV(gcr_cntl);
   const uint32_t glk_wb = G_586_GLK_WB(gcr_cntl);
   const uint32_t glk_inv = G_586_GLK_INV(gcr_cntl);
   const uint32_t glv_inv = G_586_GLV_INV(gcr_cntl);
   const uint32_t gl1_inv = G_586_GL1_INV(gcr_cntl);
   assert(G_586_GL2_US(gcr_cntl) == 0);
   assert(G_586_GL2_RANGE(gcr_cntl) == 0);
   assert(G_586_GL2_DISCARD(gcr_cntl) == 0);
   const uint32_t gl2_inv = G_586_GL2_INV(gcr_cntl);
   const uint32_t gl2_wb = G_586_GL2_WB(gcr_cntl);
   const uint32_t gcr_seq = G_586_SEQ(gcr_cntl);
   const bool ts = is_ts_event(event_type);

   ac_cmdbuf_begin(cs);
   ac_cmdbuf_emit(PKT3(PKT3_RELEASE_MEM, 6, 0));
   ac_cmdbuf_emit(S_490_EVENT_TYPE(event_type) |
                   S_490_EVENT_INDEX(ts ? 5 : 6) |
                   S_490_GLM_WB(glm_wb) |
                   S_490_GLM_INV(glm_inv) |
                   S_490_GLV_INV(glv_inv) |
                   S_490_GL1_INV(gl1_inv) |
                   S_490_GL2_INV(gl2_inv) |
                   S_490_GL2_WB(gl2_wb) |
                   S_490_SEQ(gcr_seq) |
                   S_490_GLK_WB(glk_wb) |
                   S_490_GLK_INV(glk_inv) |
                   S_490_PWS_ENABLE(1));
   ac_cmdbuf_emit(0); /* DST_SEL, INT_SEL, DATA_SEL */
   ac_cmdbuf_emit(0); /* ADDRESS_LO */
   ac_cmdbuf_emit(0); /* ADDRESS_HI */
   ac_cmdbuf_emit(0); /* DATA_LO */
   ac_cmdbuf_emit(0); /* DATA_HI */
   ac_cmdbuf_emit(0); /* INT_CTXID */
   ac_cmdbuf_end();
}

void
ac_emit_cp_copy_data(struct ac_cmdbuf *cs, uint32_t src_sel, uint32_t dst_sel,
                     uint64_t src_va, uint64_t dst_va,
                     enum ac_cp_copy_data_flags flags)
{
   uint32_t dword0 = COPY_DATA_SRC_SEL(src_sel) |
                     COPY_DATA_DST_SEL(dst_sel);

   if (flags & AC_CP_COPY_DATA_WR_CONFIRM)
      dword0 |= COPY_DATA_WR_CONFIRM;
   if (flags & AC_CP_COPY_DATA_COUNT_SEL)
      dword0 |= COPY_DATA_COUNT_SEL;
   if (flags & AC_CP_COPY_DATA_ENGINE_PFP)
      dword0 |= COPY_DATA_ENGINE_PFP;

   ac_cmdbuf_begin(cs);
   ac_cmdbuf_emit(PKT3(PKT3_COPY_DATA, 4, 0));
   ac_cmdbuf_emit(dword0);
   ac_cmdbuf_emit(src_va);
   ac_cmdbuf_emit(src_va >> 32);
   ac_cmdbuf_emit(dst_va);
   ac_cmdbuf_emit(dst_va >> 32);
   ac_cmdbuf_end();
}

void
ac_emit_cp_pfp_sync_me(struct ac_cmdbuf *cs, bool predicate)
{
   ac_cmdbuf_begin(cs);
   ac_cmdbuf_emit(PKT3(PKT3_PFP_SYNC_ME, 0, predicate));
   ac_cmdbuf_emit(0);
   ac_cmdbuf_end();
}

void
ac_emit_cp_set_predication(struct ac_cmdbuf *cs, enum amd_gfx_level gfx_level,
                           uint64_t va, uint32_t op)
{
   ac_cmdbuf_begin(cs);
   if (gfx_level >= GFX9) {
      ac_cmdbuf_emit(PKT3(PKT3_SET_PREDICATION, 2, 0));
      ac_cmdbuf_emit(op);
      ac_cmdbuf_emit(va);
      ac_cmdbuf_emit(va >> 32);
   } else {
      ac_cmdbuf_emit(PKT3(PKT3_SET_PREDICATION, 1, 0));
      ac_cmdbuf_emit(va);
      ac_cmdbuf_emit(op | ((va >> 32) & 0xFF));
   }
   ac_cmdbuf_end();
}

void
ac_emit_cp_gfx11_ge_rings(struct ac_cmdbuf *cs, const struct radeon_info *info,
                         uint64_t attr_ring_va, bool enable_gfx12_partial_hiz_wa)
{
   assert(info->gfx_level >= GFX11);
   assert((attr_ring_va >> 32) == info->address32_hi);

   ac_cmdbuf_begin(cs);

   ac_cmdbuf_set_uconfig_reg_seq(R_031110_SPI_GS_THROTTLE_CNTL1, 4);
   ac_cmdbuf_emit(0x12355123);
   ac_cmdbuf_emit(0x1544D);
   ac_cmdbuf_emit(attr_ring_va >> 16);
   ac_cmdbuf_emit(S_03111C_MEM_SIZE((info->attribute_ring_size_per_se >> 16) - 1) |
                  S_03111C_BIG_PAGE(info->discardable_allows_big_page) |
                  S_03111C_L1_POLICY(1));

   if (info->gfx_level >= GFX12) {
      const uint64_t pos_va = attr_ring_va + info->pos_ring_offset;
      const uint64_t prim_va = attr_ring_va + info->prim_ring_offset;

      /* When one of these 4 registers is updated, all 4 must be updated. */
      ac_cmdbuf_set_uconfig_reg_seq(R_0309A0_GE_POS_RING_BASE, 4);
      ac_cmdbuf_emit(pos_va >> 16);
      ac_cmdbuf_emit(S_0309A4_MEM_SIZE(info->pos_ring_size_per_se >> 5));
      ac_cmdbuf_emit(prim_va >> 16);
      ac_cmdbuf_emit(S_0309AC_MEM_SIZE(info->prim_ring_size_per_se >> 5) |
                     S_0309AC_SCOPE(gfx12_scope_device) |
                     S_0309AC_PAF_TEMPORAL(gfx12_store_high_temporal_stay_dirty) |
                     S_0309AC_PAB_TEMPORAL(gfx12_load_last_use_discard) |
                     S_0309AC_SPEC_DATA_READ(gfx12_spec_read_auto) |
                     S_0309AC_FORCE_SE_SCOPE(1) |
                     S_0309AC_PAB_NOFILL(1));

      if (info->gfx_level == GFX12 && info->pfp_fw_version >= 2680) {
         /* Mitigate the HiZ GPU hang by increasing a timeout when
          * BOTTOM_OF_PIPE_TS is used as the workaround. This must be emitted
          * when the gfx queue is idle.
          */
         const uint32_t timeout = enable_gfx12_partial_hiz_wa ? 0xfff : 0;

         ac_cmdbuf_emit(PKT3(PKT3_UPDATE_DB_SUMMARIZER_TIMEOUT, 0, 0));
         ac_cmdbuf_emit(S_EF1_SUMM_CNTL_EVICT_TIMEOUT(timeout));
      }
   }

   ac_cmdbuf_end();
}
