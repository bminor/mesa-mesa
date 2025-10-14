/*
 * Copyright 2024 Advanced Micro Devices, Inc.
 *
 * SPDX-License-Identifier: MIT
 */

#include "si_build_pm4.h"

void si_cp_release_mem_pws(struct si_context *sctx, struct radeon_cmdbuf *cs,
                           unsigned event_type, unsigned gcr_cntl)
{
   ac_emit_cp_release_mem_pws(&cs->current, sctx->gfx_level,
                              sctx->is_gfx_queue ? AMD_IP_GFX : AMD_IP_COMPUTE,
                              event_type, gcr_cntl);
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
   ac_emit_cp_pfp_sync_me(&cs->current, false);
}
