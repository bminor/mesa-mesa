/*
 * Copyright 2025 Arm Ltd.
 *
 * SPDX-License-Identifier: MIT
 */

#include "panvk_instr.h"
#include "panvk_tracepoints.h"
#include "panvk_utrace.h"

#include "util/macros.h"

static void
panvk_instr_end_barrier(enum panvk_subqueue_id id,
                        struct panvk_utrace_cs_info *cs_info,
                        const struct panvk_instr_end_args *const args)
{
   trace_end_barrier(&cs_info->cmdbuf->utrace.uts[id], cs_info,
                     args->barrier.wait_sb_mask,
                     args->barrier.wait_subqueue_mask, args->barrier.l2,
                     args->barrier.lsc, args->barrier.other);
}

static void
panvk_instr_end_cmdbuf(enum panvk_subqueue_id id,
                       struct panvk_utrace_cs_info *cs_info,
                       const struct panvk_instr_end_args *const args)
{
   trace_end_cmdbuf(&cs_info->cmdbuf->utrace.uts[id], cs_info, args->cmdbuf.flags);
}

static void
panvk_instr_end_render(enum panvk_subqueue_id id,
                       struct panvk_utrace_cs_info *cs_info,
                       const struct panvk_instr_end_args *const args)
{
   trace_end_render(&cs_info->cmdbuf->utrace.uts[id], cs_info, args->render.flags,
                    args->render.fb);
}

static void
panvk_instr_end_dispatch(enum panvk_subqueue_id id,
                         struct panvk_utrace_cs_info *cs_info,
                         const struct panvk_instr_end_args *const args)
{
   trace_end_dispatch(&cs_info->cmdbuf->utrace.uts[id], cs_info,
                      args->dispatch.base_group_x, args->dispatch.base_group_y,
                      args->dispatch.base_group_z, args->dispatch.group_count_x,
                      args->dispatch.group_count_y,
                      args->dispatch.group_count_z, args->dispatch.group_size_x,
                      args->dispatch.group_size_y, args->dispatch.group_size_z);
}

static void
panvk_instr_end_dispatch_indirect(enum panvk_subqueue_id id,
                                  struct panvk_utrace_cs_info *cs_info,
                                  const struct panvk_instr_end_args *const args)
{
   trace_end_dispatch_indirect(&cs_info->cmdbuf->utrace.uts[id], cs_info,
                               (struct u_trace_address){
                                  .bo = NULL,
                                  .offset = args->dispatch_indirect.buffer_gpu,
                               });
}

void
panvk_per_arch(panvk_instr_begin_work)(enum panvk_subqueue_id id,
                                       struct panvk_cmd_buffer *cmdbuf,
                                       enum panvk_instr_work_type work_type)
{
   struct panvk_utrace_cs_info cs_info = {
      .cmdbuf = cmdbuf,
      /* For the begin marker, the caller should wait for dependencies before
         calling begin. */
      .ts_wait_mask = 0,
   };

   switch (work_type) {
   case PANVK_INSTR_WORK_TYPE_CMDBUF:
      trace_begin_cmdbuf(&cmdbuf->utrace.uts[id], &cs_info);
      break;
   case PANVK_INSTR_WORK_TYPE_META:
      trace_begin_meta(&cmdbuf->utrace.uts[id], &cs_info);
      break;
   case PANVK_INSTR_WORK_TYPE_RENDER:
      trace_begin_render(&cmdbuf->utrace.uts[id], &cs_info);
      break;
   case PANVK_INSTR_WORK_TYPE_DISPATCH:
      trace_begin_dispatch(&cmdbuf->utrace.uts[id], &cs_info);
      break;
   case PANVK_INSTR_WORK_TYPE_DISPATCH_INDIRECT:
      trace_begin_dispatch_indirect(&cmdbuf->utrace.uts[id], &cs_info);
      break;
   case PANVK_INSTR_WORK_TYPE_BARRIER:
      trace_begin_barrier(&cmdbuf->utrace.uts[id], &cs_info);
      break;
   case PANVK_INSTR_WORK_TYPE_SYNC_WAIT:
      trace_begin_sync_wait(&cmdbuf->utrace.uts[id], &cs_info);
      break;
   default:
      UNREACHABLE("unsupported panvk_instr_work_type");
   }
}

void
panvk_per_arch(panvk_instr_end_work)(
   enum panvk_subqueue_id id, struct panvk_cmd_buffer *cmdbuf,
   enum panvk_instr_work_type work_type,
   const struct panvk_instr_end_args *const args)
{
   panvk_per_arch(panvk_instr_end_work_async)(id, cmdbuf, work_type, args, 0);
}

void
panvk_per_arch(panvk_instr_end_work_async)(
   enum panvk_subqueue_id id, struct panvk_cmd_buffer *cmdbuf,
   enum panvk_instr_work_type work_type,
   const struct panvk_instr_end_args *const args, unsigned int wait_mask)
{
   struct panvk_utrace_cs_info cs_info = {
      .cmdbuf = cmdbuf,
      .ts_wait_mask = wait_mask,
   };

   switch (work_type) {
   case PANVK_INSTR_WORK_TYPE_CMDBUF:
      panvk_instr_end_cmdbuf(id, &cs_info, args);
      break;
   case PANVK_INSTR_WORK_TYPE_META:
      trace_end_meta(&cmdbuf->utrace.uts[id], &cs_info);
      break;
   case PANVK_INSTR_WORK_TYPE_RENDER:
      panvk_instr_end_render(id, &cs_info, args);
      break;
   case PANVK_INSTR_WORK_TYPE_DISPATCH:
      panvk_instr_end_dispatch(id, &cs_info, args);
      break;
   case PANVK_INSTR_WORK_TYPE_DISPATCH_INDIRECT:
      panvk_instr_end_dispatch_indirect(id, &cs_info, args);
      break;
   case PANVK_INSTR_WORK_TYPE_BARRIER:
      panvk_instr_end_barrier(id, &cs_info, args);
      break;
   case PANVK_INSTR_WORK_TYPE_SYNC_WAIT:
      trace_end_sync_wait(&cmdbuf->utrace.uts[id], &cs_info);
      break;
   default:
      UNREACHABLE("unsupported panvk_instr_work_type");
   }
}
