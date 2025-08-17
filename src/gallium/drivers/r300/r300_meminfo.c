/*
 * Copyright 2017 Advanced Micro Devices, Inc.
 * Copyright 2025 Brais Solla <brais.1997@gmail.com<
 *
 * SPDX-License-Identifier: MIT
 */


#include "r300_screen.h"
#include "r300_meminfo.h"


void r300_query_memory_info(struct pipe_screen *pscreen, struct pipe_memory_info *info)
{
   struct r300_screen *rscreen = (struct r300_screen*) pscreen;
   struct radeon_winsys *ws     = rscreen->rws;

   info->total_device_memory   = rscreen->info.vram_size_kb;
   info->total_staging_memory  = rscreen->info.gart_size_kb;

   /* The real TTM memory usage is somewhat random, because:
    *
    * 1) TTM delays freeing memory, because it can only free it after
    *    fences expire.
    *
    * 2) The memory usage can be really low if big VRAM evictions are
    *    taking place, but the real usage is well above the size of VRAM.
    *
    * Instead, return statistics of this process.
    */
   unsigned vram_used = ws->query_value(ws, RADEON_VRAM_USAGE) / 1024;
   unsigned gtt_used  = ws->query_value(ws, RADEON_GTT_USAGE)  / 1024;

   info->avail_device_memory   = (vram_used > info->total_device_memory) ? 0 : info->total_device_memory  - vram_used;
   info->avail_staging_memory  = (gtt_used > info->total_staging_memory) ? 0 : info->total_staging_memory - gtt_used;
   info->device_memory_evicted = ws->query_value(ws, RADEON_NUM_BYTES_MOVED) / 1024;
   info->nr_device_memory_evictions = ws->query_value(ws, RADEON_NUM_EVICTIONS);
   
}