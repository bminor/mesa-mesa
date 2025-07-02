/*
 * Copyright © 2023-2025 Amazon.com, Inc. or its affiliates.
 *
 * SPDX-License-Identifier: MIT
 */

#include "pan_afbc.h"
#include "util/perf/cpu_trace.h"

uint32_t
pan_afbc_payload_layout_packed(unsigned arch,
                               const struct pan_afbc_headerblock *headers,
                               struct pan_afbc_payload_extent *layout,
                               uint32_t nr_blocks, enum pipe_format format,
                               uint64_t modifier)
{
   MESA_TRACE_FUNC();

   /* XXX: It might be faster to copy the header from non-cacheable memory
    * into a cacheline sized chunk in cacheable memory in order to avoid too
    * many uncached transactions. Not sure though, so it needs testing. */

   uint32_t uncompressed_size =
      pan_afbc_payload_uncompressed_size(format, modifier);
   uint32_t body_size = 0;

   for (uint32_t i = 0; i < nr_blocks; i++) {
      uint32_t payload_size =
         pan_afbc_payload_size(arch, headers[i], uncompressed_size);
      layout[i].size = payload_size;
      layout[i].offset = body_size;
      body_size += payload_size;
   }

   return body_size;
}
