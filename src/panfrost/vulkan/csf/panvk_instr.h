/*
 * Copyright 2025 Arm Ltd.
 *
 * SPDX-License-Identifier: MIT
 */

#include "panvk_cmd_buffer.h"
#include "panvk_queue.h"

enum panvk_instr_work_type {
   PANVK_INSTR_WORK_TYPE_CMDBUF,
   PANVK_INSTR_WORK_TYPE_META,
   PANVK_INSTR_WORK_TYPE_RENDER,
   PANVK_INSTR_WORK_TYPE_DISPATCH,
   PANVK_INSTR_WORK_TYPE_DISPATCH_INDIRECT,
   PANVK_INSTR_WORK_TYPE_BARRIER,
   PANVK_INSTR_WORK_TYPE_SYNC_WAIT,
};

struct panvk_instr_end_args {
   /* Depending on which work type is ended, one of the options below is valid. */
   union {
      struct {
         uint8_t wait_sb_mask;
         uint8_t wait_subqueue_mask;
         uint8_t l2;
         uint8_t lsc;
         uint8_t other;
      } barrier;

      struct {
         VkCommandBufferUsageFlags flags;
      } cmdbuf;

      struct {
         VkRenderingFlags flags;
         const struct pan_fb_info *fb;
      } render;

      struct {
         uint16_t base_group_x;
         uint16_t base_group_y;
         uint16_t base_group_z;
         uint16_t group_count_x;
         uint16_t group_count_y;
         uint16_t group_count_z;
         uint16_t group_size_x;
         uint16_t group_size_y;
         uint16_t group_size_z;
      } dispatch;

      struct {
         uint64_t buffer_gpu;
      } dispatch_indirect;
   };
};

void
   panvk_per_arch(panvk_instr_begin_work)(enum panvk_subqueue_id id,
                                          struct panvk_cmd_buffer *cmdbuf,
                                          enum panvk_instr_work_type work_type);

/**
 * Mark the end of synchronous work.
 */
void panvk_per_arch(panvk_instr_end_work)(
   enum panvk_subqueue_id id, struct panvk_cmd_buffer *cmdbuf,
   enum panvk_instr_work_type work_type,
   const struct panvk_instr_end_args *const args);

/**
 * Mark the end of async work with an immediate scoreboard mask.
 */
void panvk_per_arch(panvk_instr_end_work_async)(
   enum panvk_subqueue_id id, struct panvk_cmd_buffer *cmdbuf,
   enum panvk_instr_work_type work_type,
   const struct panvk_instr_end_args *const args, unsigned int wait_mask);
