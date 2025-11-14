/*
 * Copyright 2023 Alyssa Rosenzweig
 * Copyright 2023 Valve Corporation
 * Copyright 2025 Collabora, Ltd.
 * SPDX-License-Identifier: MIT
 */

#include "compiler/libcl/libcl.h"
#include "poly/geometry.h"
#include "poly/prim.h"

#define POLY_DECL_UNROLL_RESTART_SCRATCH(__scratch, __wg_size) \
   local uint __scratch[MAX2(__wg_size / 32, sizeof(void *))]

/*
 * Return the ID of the first thread in the workgroup where cond is true, or
 * a value greater than or equal to the workgroup size if cond is false across
 * the workgroup.
 */
static inline uint
poly_work_group_first_true(bool cond, local uint *scratch)
{
   barrier(CLK_LOCAL_MEM_FENCE);
   scratch[get_sub_group_id()] = sub_group_ballot(cond)[0];
   barrier(CLK_LOCAL_MEM_FENCE);

   uint first_group =
      ctz(sub_group_ballot(scratch[get_sub_group_local_id()])[0]);
   uint off = ctz(first_group < 32 ? scratch[first_group] : 0);
   return (first_group * 32) + off;
}

/*
 * When unrolling the index buffer for a draw, we translate the old indirect
 * draws to new indirect draws. This routine allocates the new index buffer and
 * sets up most of the new draw descriptor.
 */
static inline global void *
poly_setup_unroll_for_draw(global struct poly_heap *heap,
                           constant uint *in_draw, global uint *out_draw,
                           enum mesa_prim mode, uint index_size_B)
{
   /* Determine an upper bound on the memory required for the index buffer.
    * Restarts only decrease the unrolled index buffer size, so the maximum size
    * is the unrolled size when the input has no restarts.
    */
   uint max_prims = u_decomposed_prims_for_vertices(mode, in_draw[0]);
   uint max_verts = max_prims * mesa_vertices_per_prim(mode);
   uint alloc_size = max_verts * index_size_B;

   /* Allocate unrolled index buffer.
    *
    * TODO: For multidraw, should be atomic. But multidraw+unroll isn't
    * currently wired up in any driver.
    */
   uint old_heap_bottom_B = poly_heap_alloc_nonatomic_offs(heap, alloc_size);

   /* Setup most of the descriptor. Count will be determined after unroll. */
   out_draw[1] = in_draw[1];                       /* instance count */
   out_draw[2] = old_heap_bottom_B / index_size_B; /* index offset */
   out_draw[3] = in_draw[3];                       /* index bias */
   out_draw[4] = in_draw[4];                       /* base instance */

   /* Return the index buffer we allocated */
   return (global uchar *)heap->base + old_heap_bottom_B;
}

static inline void
poly_unroll_restart(global uint32_t *out_draw,
                    global struct poly_heap *heap,
                    constant uint *in_draw,
                    uint64_t index_buffer,
                    uint32_t index_buffer_range_el,
                    uint32_t index_size_B,
                    uint32_t restart_index,
                    uint32_t flatshade_first,
                    enum mesa_prim mode,
                    local void *scratch)
{
   uint tid = cl_local_id.x;
   uint count = in_draw[0];

   uintptr_t out_ptr;
   if (tid == 0) {
      out_ptr = (uintptr_t)poly_setup_unroll_for_draw(heap, in_draw, out_draw,
                                                      mode, index_size_B);
      *(uintptr_t *)scratch = out_ptr;
   }

   barrier(CLK_LOCAL_MEM_FENCE);
   out_ptr = *(uintptr_t *)scratch;

   uintptr_t in_ptr = (uintptr_t)(poly_index_buffer(
      index_buffer, index_buffer_range_el, in_draw[2], index_size_B));

   uint out_prims = 0;
   uint needle = 0;
   uint per_prim = mesa_vertices_per_prim(mode);
   while (needle < count) {
      /* Search for next restart or the end. Lanes load in parallel. */
      uint next_restart = needle;
      for (;;) {
         uint idx = next_restart + tid;
         bool restart =
            idx >= count || poly_load_index(in_ptr, index_buffer_range_el, idx,
                                            index_size_B) == restart_index;

         uint next_offs = poly_work_group_first_true(restart, scratch);

         next_restart += next_offs;
         if (next_offs < 1024)
            break;
      }

      /* Emit up to the next restart. Lanes output in parallel */
      uint subcount = next_restart - needle;
      uint subprims = u_decomposed_prims_for_vertices(mode, subcount);
      uint out_prims_base = out_prims;
      for (uint i = tid; i < subprims; i += 1024) {
         for (uint vtx = 0; vtx < per_prim; ++vtx) {
            uint id =
               poly_vertex_id_for_topology(mode, flatshade_first, i, vtx, subprims);
            uint offset = needle + id;

            uint x = ((out_prims_base + i) * per_prim) + vtx;
            uint y = poly_load_index(in_ptr, index_buffer_range_el, offset,
                                     index_size_B);

            poly_store_index(out_ptr, index_size_B, x, y);
         }
      }

      out_prims += subprims;
      needle = next_restart + 1;
   }

   if (tid == 0)
      out_draw[0] = out_prims * per_prim;
}
