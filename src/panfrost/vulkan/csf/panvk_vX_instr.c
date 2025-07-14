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
                        struct panvk_cmd_buffer *cmdbuf,
                        const struct panvk_instr_end_args *const args)
{
   trace_end_barrier(&cmdbuf->utrace.uts[id], cmdbuf,
                     args->barrier.wait_sb_mask,
                     args->barrier.wait_subqueue_mask, args->barrier.l2,
                     args->barrier.lsc, args->barrier.other);
}

static void
panvk_instr_end_cmdbuf(enum panvk_subqueue_id id,
                       struct panvk_cmd_buffer *cmdbuf,
                       const struct panvk_instr_end_args *const args)
{
   trace_end_cmdbuf(&cmdbuf->utrace.uts[id], cmdbuf, args->cmdbuf.flags);
}

static void
panvk_instr_end_render(enum panvk_subqueue_id id,
                       struct panvk_cmd_buffer *cmdbuf,
                       const struct panvk_instr_end_args *const args)
{
   trace_end_render(&cmdbuf->utrace.uts[id], cmdbuf, args->render.flags,
                    args->render.fb);
}

static void
panvk_instr_end_dispatch(enum panvk_subqueue_id id,
                         struct panvk_cmd_buffer *cmdbuf,
                         const struct panvk_instr_end_args *const args)
{
   trace_end_dispatch(&cmdbuf->utrace.uts[id], cmdbuf,
                      args->dispatch.base_group_x, args->dispatch.base_group_y,
                      args->dispatch.base_group_z, args->dispatch.group_count_x,
                      args->dispatch.group_count_y,
                      args->dispatch.group_count_z, args->dispatch.group_size_x,
                      args->dispatch.group_size_y, args->dispatch.group_size_z);
}

static void
panvk_instr_end_dispatch_indirect(enum panvk_subqueue_id id,
                                  struct panvk_cmd_buffer *cmdbuf,
                                  const struct panvk_instr_end_args *const args)
{
   trace_end_dispatch_indirect(&cmdbuf->utrace.uts[id], cmdbuf,
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
   switch (work_type) {
   case PANVK_INSTR_WORK_TYPE_CMDBUF:
      trace_begin_cmdbuf(&cmdbuf->utrace.uts[id], cmdbuf);
      break;
   case PANVK_INSTR_WORK_TYPE_META:
      trace_begin_meta(&cmdbuf->utrace.uts[id], cmdbuf);
      break;
   case PANVK_INSTR_WORK_TYPE_RENDER:
      trace_begin_render(&cmdbuf->utrace.uts[id], cmdbuf);
      break;
   case PANVK_INSTR_WORK_TYPE_DISPATCH:
      trace_begin_dispatch(&cmdbuf->utrace.uts[id], cmdbuf);
      break;
   case PANVK_INSTR_WORK_TYPE_DISPATCH_INDIRECT:
      trace_begin_dispatch_indirect(&cmdbuf->utrace.uts[id], cmdbuf);
      break;
   case PANVK_INSTR_WORK_TYPE_BARRIER:
      trace_begin_barrier(&cmdbuf->utrace.uts[id], cmdbuf);
      break;
   case PANVK_INSTR_WORK_TYPE_SYNC_WAIT:
      trace_begin_sync_wait(&cmdbuf->utrace.uts[id], cmdbuf);
      break;
   default:
      unreachable("unsupported panvk_instr_work_type");
   }
}

void
panvk_per_arch(panvk_instr_end_work)(
   enum panvk_subqueue_id id, struct panvk_cmd_buffer *cmdbuf,
   enum panvk_instr_work_type work_type,
   const struct panvk_instr_end_args *const args)
{
   switch (work_type) {
   case PANVK_INSTR_WORK_TYPE_CMDBUF:
      panvk_instr_end_cmdbuf(id, cmdbuf, args);
      break;
   case PANVK_INSTR_WORK_TYPE_META:
      trace_end_meta(&cmdbuf->utrace.uts[id], cmdbuf);
      break;
   case PANVK_INSTR_WORK_TYPE_RENDER:
      panvk_instr_end_render(id, cmdbuf, args);
      break;
   case PANVK_INSTR_WORK_TYPE_DISPATCH:
      panvk_instr_end_dispatch(id, cmdbuf, args);
      break;
   case PANVK_INSTR_WORK_TYPE_DISPATCH_INDIRECT:
      panvk_instr_end_dispatch_indirect(id, cmdbuf, args);
      break;
   case PANVK_INSTR_WORK_TYPE_BARRIER:
      panvk_instr_end_barrier(id, cmdbuf, args);
      break;
   case PANVK_INSTR_WORK_TYPE_SYNC_WAIT:
      trace_end_sync_wait(&cmdbuf->utrace.uts[id], cmdbuf);
      break;
   default:
      unreachable("unsupported panvk_instr_work_type");
   }
}
