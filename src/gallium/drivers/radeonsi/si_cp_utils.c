/*
 * Copyright 2024 Advanced Micro Devices, Inc.
 *
 * SPDX-License-Identifier: MIT
 */

#include "si_build_pm4.h"

static bool is_ts_event(unsigned event_type)
{
   return event_type == V_028A90_CACHE_FLUSH_TS ||
          event_type == V_028A90_CACHE_FLUSH_AND_INV_TS_EVENT ||
          event_type == V_028A90_BOTTOM_OF_PIPE_TS ||
          event_type == V_028A90_FLUSH_AND_INV_DB_DATA_TS ||
          event_type == V_028A90_FLUSH_AND_INV_CB_DATA_TS;
}

/* Insert CS_DONE, PS_DONE, or a *_TS event into the pipeline, which will signal after the work
 * indicated by the event is complete, which optionally includes flushing caches using "gcr_cntl"
 * after the completion of the work. *_TS events are always signaled at the end of the pipeline,
 * while CS_DONE and PS_DONE are signaled when those shaders finish. This call only inserts
 * the event into the pipeline. It doesn't wait for anything and it doesn't execute anything
 * immediately. The only way to wait for the event completion is to call si_cp_acquire_mem_pws
 * with the same "event_type".
 */
void si_cp_release_mem_pws(struct si_context *sctx, struct radeon_cmdbuf *cs,
                           unsigned event_type, unsigned gcr_cntl)
{
   assert(sctx->gfx_level >= GFX11 && sctx->is_gfx_queue);
   bool ts = is_ts_event(event_type);
   /* Extract GCR_CNTL fields because the encoding is different in RELEASE_MEM. */
   assert(G_586_GLI_INV(gcr_cntl) == 0);
   assert(G_586_GL1_RANGE(gcr_cntl) == 0);
   unsigned glm_wb = G_586_GLM_WB(gcr_cntl);
   unsigned glm_inv = G_586_GLM_INV(gcr_cntl);
   unsigned glk_wb = G_586_GLK_WB(gcr_cntl);
   unsigned glk_inv = G_586_GLK_INV(gcr_cntl);
   unsigned glv_inv = G_586_GLV_INV(gcr_cntl);
   unsigned gl1_inv = G_586_GL1_INV(gcr_cntl);
   assert(G_586_GL2_US(gcr_cntl) == 0);
   assert(G_586_GL2_RANGE(gcr_cntl) == 0);
   assert(G_586_GL2_DISCARD(gcr_cntl) == 0);
   unsigned gl2_inv = G_586_GL2_INV(gcr_cntl);
   unsigned gl2_wb = G_586_GL2_WB(gcr_cntl);
   unsigned gcr_seq = G_586_SEQ(gcr_cntl);

   radeon_begin(cs);
   radeon_emit(PKT3(PKT3_RELEASE_MEM, 6, 0));
   radeon_emit(S_490_EVENT_TYPE(event_type) |
               S_490_EVENT_INDEX(ts ? 5 : 6) |
               S_490_GLM_WB(glm_wb) | S_490_GLM_INV(glm_inv) | S_490_GLV_INV(glv_inv) |
               S_490_GL1_INV(gl1_inv) | S_490_GL2_INV(gl2_inv) | S_490_GL2_WB(gl2_wb) |
               S_490_SEQ(gcr_seq) | S_490_GLK_WB(glk_wb) | S_490_GLK_INV(glk_inv) |
               S_490_PWS_ENABLE(1));
   radeon_emit(0); /* DST_SEL, INT_SEL, DATA_SEL */
   radeon_emit(0); /* ADDRESS_LO */
   radeon_emit(0); /* ADDRESS_HI */
   radeon_emit(0); /* DATA_LO */
   radeon_emit(0); /* DATA_HI */
   radeon_emit(0); /* INT_CTXID */
   radeon_end();
}

void si_cp_acquire_mem_pws(struct si_context *sctx, struct radeon_cmdbuf *cs,
                           unsigned event_type, unsigned stage_sel, unsigned gcr_cntl,
                           unsigned distance, unsigned sqtt_flush_flags)
{
   if (unlikely(sctx->sqtt_enabled))
      si_sqtt_describe_barrier_start(sctx, cs);

   ac_emit_cp_acquire_mem_pws(&cs->current, sctx->gfx_level,
                              sctx->is_gfx_queue ? AMD_IP_GFX : AMD_IP_COMPUTE,
                              event_type, stage_sel, distance, gcr_cntl);

   if (unlikely(sctx->sqtt_enabled))
      si_sqtt_describe_barrier_end(sctx, cs, sqtt_flush_flags);
}

void si_cp_release_acquire_mem_pws(struct si_context *sctx, struct radeon_cmdbuf *cs,
                                   unsigned event_type, unsigned gcr_cntl, unsigned stage_sel,
                                   unsigned sqtt_flush_flags)
{
   si_cp_release_mem_pws(sctx, cs, event_type, gcr_cntl);
   si_cp_acquire_mem_pws(sctx, cs, event_type, stage_sel, 0, 0, sqtt_flush_flags);
}

/* Execute plain ACQUIRE_MEM that just flushes caches. This optionally waits for idle on older
 * chips. "engine" determines whether to sync in PFP or ME.
 */
void si_cp_acquire_mem(struct si_context *sctx, struct radeon_cmdbuf *cs, unsigned gcr_cntl,
                       unsigned engine)
{
   assert(engine == V_580_CP_PFP || engine == V_580_CP_ME);
   assert(gcr_cntl);

   if (sctx->gfx_level >= GFX10) {
      /* ACQUIRE_MEM in PFP is implemented as ACQUIRE_MEM in ME + PFP_SYNC_ME. */
      unsigned engine_flag = engine == V_580_CP_ME ? BITFIELD_BIT(31) : 0;

      /* Flush caches. This doesn't wait for idle. */
      radeon_begin(cs);
      radeon_emit(PKT3(PKT3_ACQUIRE_MEM, 6, 0));
      radeon_emit(engine_flag);   /* which engine to use */
      radeon_emit(0xffffffff);    /* CP_COHER_SIZE */
      radeon_emit(0x01ffffff);    /* CP_COHER_SIZE_HI */
      radeon_emit(0);             /* CP_COHER_BASE */
      radeon_emit(0);             /* CP_COHER_BASE_HI */
      radeon_emit(0x0000000A);    /* POLL_INTERVAL */
      radeon_emit(gcr_cntl);      /* GCR_CNTL */
      radeon_end();
   } else {
      bool compute_ib = !sctx->is_gfx_queue;

      /* This seems problematic with GFX7 (see #4764) */
      if (sctx->gfx_level != GFX7)
         gcr_cntl |= 1u << 31; /* don't sync PFP, i.e. execute the sync in ME */

      if (sctx->gfx_level == GFX9 || compute_ib) {
         /* Flush caches and wait for the caches to assert idle. */
         radeon_begin(cs);
         radeon_emit(PKT3(PKT3_ACQUIRE_MEM, 5, 0));
         radeon_emit(gcr_cntl);      /* CP_COHER_CNTL */
         radeon_emit(0xffffffff);    /* CP_COHER_SIZE */
         radeon_emit(0xffffff);      /* CP_COHER_SIZE_HI */
         radeon_emit(0);             /* CP_COHER_BASE */
         radeon_emit(0);             /* CP_COHER_BASE_HI */
         radeon_emit(0x0000000A);    /* POLL_INTERVAL */
         radeon_end();
      } else {
         /* ACQUIRE_MEM is only required on the compute ring. */
         radeon_begin(cs);
         radeon_emit(PKT3(PKT3_SURFACE_SYNC, 3, 0));
         radeon_emit(gcr_cntl);      /* CP_COHER_CNTL */
         radeon_emit(0xffffffff);    /* CP_COHER_SIZE */
         radeon_emit(0);             /* CP_COHER_BASE */
         radeon_emit(0x0000000A);    /* POLL_INTERVAL */
         radeon_end();
      }

      /* ACQUIRE_MEM & SURFACE_SYNC roll the context if the current context is busy. */
      if (!compute_ib)
         sctx->context_roll = true;

      if (engine == V_580_CP_PFP)
         si_cp_pfp_sync_me(cs);
   }
}

void si_cp_pfp_sync_me(struct radeon_cmdbuf *cs)
{
   radeon_begin(cs);
   radeon_emit(PKT3(PKT3_PFP_SYNC_ME, 0, 0));
   radeon_emit(0);
   radeon_end();
}
