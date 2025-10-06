/*
 * Copyright 2023 Alyssa Rosenzweig
 * Copyright 2023 Valve Corporation
 * SPDX-License-Identifier: MIT
 */

#include "compiler/libcl/libcl_vk.h"
#include "poly/geometry.h"
#include "poly/tessellator.h"
#include "util/macros.h"
#include "util/u_math.h"

/* Swap the two non-provoking vertices in odd triangles. This generates a vertex
 * ID list with a consistent winding order.
 *
 * Holding prim and flatshade_first constant, the map : [0, 1, 2] -> [0, 1, 2]
 * is its own inverse. It is hence used both vertex fetch and transform
 * feedback.
 */
static uint
map_vertex_in_tri_strip(uint prim, uint vert, bool flatshade_first)
{
   unsigned pv = flatshade_first ? 0 : 2;

   bool even = (prim & 1) == 0;
   bool provoking = vert == pv;

   return (provoking || even) ? vert : ((3 - pv) - vert);
}

static uint
vertex_id_for_line_loop(uint prim, uint vert, uint num_prims)
{
   /* (0, 1), (1, 2), (2, 0) */
   if (prim == (num_prims - 1) && vert == 1)
      return 0;
   else
      return prim + vert;
}

static uint
vertex_id_for_tri_fan(uint prim, uint vert, bool flatshade_first)
{
   /* Vulkan spec section 20.1.7 gives (i + 1, i + 2, 0) for a provoking
    * first. OpenGL instead wants (0, i + 1, i + 2) with a provoking last.
    * Piglit clipflat expects us to switch between these orders depending on
    * provoking vertex, to avoid trivializing the fan.
    *
    * Rotate accordingly.
    */
   if (flatshade_first) {
      vert = (vert == 2) ? 0 : (vert + 1);
   }

   /* The simpler form assuming last is provoking. */
   return (vert == 0) ? 0 : prim + vert;
}

static uint
vertex_id_for_tri_strip_adj(uint prim, uint vert, uint num_prims,
                            bool flatshade_first)
{
   /* See Vulkan spec section 20.1.11 "Triangle Strips With Adjancency".
    *
    * There are different cases for first/middle/last/only primitives and for
    * odd/even primitives.  Determine which case we're in.
    */
   bool last = prim == (num_prims - 1);
   bool first = prim == 0;
   bool even = (prim & 1) == 0;
   bool even_or_first = even || first;

   /* When the last vertex is provoking, we rotate the primitives
    * accordingly. This seems required for OpenGL.
    */
   if (!flatshade_first && !even_or_first) {
      vert = (vert + 4u) % 6u;
   }

   /* Offsets per the spec. The spec lists 6 cases with 6 offsets. Luckily,
    * there are lots of patterns we can exploit, avoiding a full 6x6 LUT.
    *
    * Here we assume the first vertex is provoking, the Vulkan default.
    */
   uint offsets[6] = {
      0,
      first ? 1 : (even ? -2 : 3),
      even_or_first ? 2 : 4,
      last ? 5 : 6,
      even_or_first ? 4 : 2,
      even_or_first ? 3 : -2,
   };

   /* Ensure NIR can see thru the local array */
   uint offset = 0;
   for (uint i = 1; i < 6; ++i) {
      if (i == vert)
         offset = offsets[i];
   }

   /* Finally add to the base of the primitive */
   return (prim * 2) + offset;
}

static uint
vertex_id_for_topology(enum mesa_prim mode, bool flatshade_first, uint prim,
                       uint vert, uint num_prims)
{
   switch (mode) {
   case MESA_PRIM_POINTS:
   case MESA_PRIM_LINES:
   case MESA_PRIM_TRIANGLES:
   case MESA_PRIM_LINES_ADJACENCY:
   case MESA_PRIM_TRIANGLES_ADJACENCY:
      /* Regular primitive: every N vertices defines a primitive */
      return (prim * mesa_vertices_per_prim(mode)) + vert;

   case MESA_PRIM_LINE_LOOP:
      return vertex_id_for_line_loop(prim, vert, num_prims);

   case MESA_PRIM_LINE_STRIP:
   case MESA_PRIM_LINE_STRIP_ADJACENCY:
      /* (i, i + 1) or (i, ..., i + 3) */
      return prim + vert;

   case MESA_PRIM_TRIANGLE_STRIP: {
      /* Order depends on the provoking vert.
       *
       * First: (0, 1, 2), (1, 3, 2), (2, 3, 4).
       * Last:  (0, 1, 2), (2, 1, 3), (2, 3, 4).
       *
       * Pull the (maybe swapped) vert from the corresponding primitive
       */
      return prim + map_vertex_in_tri_strip(prim, vert, flatshade_first);
   }

   case MESA_PRIM_TRIANGLE_FAN:
      return vertex_id_for_tri_fan(prim, vert, flatshade_first);

   case MESA_PRIM_TRIANGLE_STRIP_ADJACENCY:
      return vertex_id_for_tri_strip_adj(prim, vert, num_prims,
                                         flatshade_first);

   default:
      return 0;
   }
}

KERNEL(1)
libagx_increment_ia(global uint32_t *ia_vertices,
                    global uint32_t *ia_primitives,
                    global uint32_t *vs_invocations, global uint32_t *c_prims,
                    global uint32_t *c_invs, constant uint32_t *draw,
                    enum mesa_prim prim, unsigned verts_per_patch)
{
   poly_increment_ia(ia_vertices, ia_primitives, vs_invocations, c_prims,
                     c_invs, draw, prim, verts_per_patch);
}

KERNEL(1024)
libagx_increment_ia_restart(global uint32_t *ia_vertices,
                            global uint32_t *ia_primitives,
                            global uint32_t *vs_invocations,
                            global uint32_t *c_prims, global uint32_t *c_invs,
                            constant uint32_t *draw, uint64_t index_buffer,
                            uint32_t index_buffer_range_el,
                            uint32_t restart_index, uint32_t index_size_B,
                            enum mesa_prim prim, unsigned verts_per_patch)
{
   uint tid = cl_global_id.x;
   unsigned count = draw[0];
   local uint scratch;

   uint start = draw[2];
   uint partial = 0;

   /* Count non-restart indices */
   for (uint i = tid; i < count; i += 1024) {
      uint index = poly_load_index(index_buffer, index_buffer_range_el,
                                   start + i, index_size_B);

      if (index != restart_index)
         partial++;
   }

   /* Accumulate the partials across the workgroup */
   scratch = 0;
   barrier(CLK_LOCAL_MEM_FENCE);
   atomic_add(&scratch, partial);
   barrier(CLK_LOCAL_MEM_FENCE);

   /* Elect a single thread from the workgroup to increment the counters */
   if (tid == 0) {
      poly_increment_counters(ia_vertices, vs_invocations, NULL,
                              scratch * draw[1]);
   }

   /* TODO: We should vectorize this */
   if ((ia_primitives || c_prims || c_invs) && tid == 0) {
      uint accum = 0;
      int last_restart = -1;
      for (uint i = 0; i < count; ++i) {
         uint index = poly_load_index(index_buffer, index_buffer_range_el,
                                      start + i, index_size_B);

         if (index == restart_index) {
            accum += poly_decomposed_prims_for_vertices_with_tess(
               prim, i - last_restart - 1, verts_per_patch);
            last_restart = i;
         }
      }

      {
         accum += poly_decomposed_prims_for_vertices_with_tess(
            prim, count - last_restart - 1, verts_per_patch);
      }

      poly_increment_counters(ia_primitives, c_prims, c_invs, accum * draw[1]);
   }
}

/*
 * Return the ID of the first thread in the workgroup where cond is true, or
 * 1024 if cond is false across the workgroup.
 */
static uint
first_true_thread_in_workgroup(bool cond, local uint *scratch)
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
static global void *
setup_unroll_for_draw(global struct poly_heap *heap, constant uint *in_draw,
                      global uint *out, enum mesa_prim mode, uint index_size_B)
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
   out[1] = in_draw[1];                       /* instance count */
   out[2] = old_heap_bottom_B / index_size_B; /* index offset */
   out[3] = in_draw[3];                       /* index bias */
   out[4] = in_draw[4];                       /* base instance */

   /* Return the index buffer we allocated */
   return (global uchar *)heap->base + old_heap_bottom_B;
}

KERNEL(1024)
libagx_unroll_restart(global struct poly_heap *heap, uint64_t index_buffer,
                      constant uint *in_draw, global uint32_t *out_draw,
                      uint32_t max_draws, uint32_t restart_index,
                      uint32_t index_buffer_size_el, uint32_t index_size_log2,
                      uint32_t flatshade_first, uint mode__11)
{
   uint32_t index_size_B = 1 << index_size_log2;
   enum mesa_prim mode = poly_uncompact_prim(mode__11);
   uint tid = cl_local_id.x;
   uint count = in_draw[0];

   local uintptr_t out_ptr;
   if (tid == 0) {
      out_ptr = (uintptr_t)setup_unroll_for_draw(heap, in_draw, out_draw, mode,
                                                 index_size_B);
   }

   barrier(CLK_LOCAL_MEM_FENCE);

   uintptr_t in_ptr = (uintptr_t)(poly_index_buffer(
      index_buffer, index_buffer_size_el, in_draw[2], index_size_B));

   local uint scratch[32];

   uint out_prims = 0;
   uint needle = 0;
   uint per_prim = mesa_vertices_per_prim(mode);
   while (needle < count) {
      /* Search for next restart or the end. Lanes load in parallel. */
      uint next_restart = needle;
      for (;;) {
         uint idx = next_restart + tid;
         bool restart =
            idx >= count || poly_load_index(in_ptr, index_buffer_size_el, idx,
                                            index_size_B) == restart_index;

         uint next_offs = first_true_thread_in_workgroup(restart, scratch);

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
               vertex_id_for_topology(mode, flatshade_first, i, vtx, subprims);
            uint offset = needle + id;

            uint x = ((out_prims_base + i) * per_prim) + vtx;
            uint y = poly_load_index(in_ptr, index_buffer_size_el, offset,
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

KERNEL(1)
libagx_gs_setup_indirect(
   uint64_t index_buffer, constant uint *draw,
   global uintptr_t *vertex_buffer /* output */,
   global struct poly_ia_state *ia /* output */,
   global struct poly_geometry_params *p /* output */,
   global struct poly_heap *heap,
   uint64_t vs_outputs /* Vertex (TES) output mask */,
   uint32_t index_size_B /* 0 if no index bffer */,
   uint32_t index_buffer_range_el,
   uint32_t prim /* Input primitive type, enum mesa_prim */,
   int is_prefix_summing, uint max_indices, enum poly_gs_shape shape)
{
   poly_gs_setup_indirect(index_buffer, draw, vertex_buffer, ia, p, heap,
                          vs_outputs, index_size_B, index_buffer_range_el, prim,
                          is_prefix_summing, max_indices, shape);
}

KERNEL(1024)
libagx_prefix_sum_geom(constant struct poly_geometry_params *p)
{
   local uint scratch[32];
   poly_prefix_sum(scratch, p->count_buffer, p->input_primitives,
                   p->count_buffer_stride / 4, cl_group_id.x, 1024);
}

KERNEL(1024)
libagx_prefix_sum_tess(global struct poly_tess_args *p, global uint *c_prims,
                       global uint *c_invs, uint increment_stats__2)
{
   local uint scratch[32];
   poly_prefix_sum(scratch, p->counts, p->nr_patches, 1 /* words */,
                   0 /* word */, 1024);

   /* After prefix summing, we know the total # of indices, so allocate the
    * index buffer now. Elect a thread for the allocation.
    */
   barrier(CLK_LOCAL_MEM_FENCE);
   if (cl_local_id.x != 0)
      return;

   /* The last element of an inclusive prefix sum is the total sum */
   uint total = p->nr_patches > 0 ? p->counts[p->nr_patches - 1] : 0;

   /* Allocate 4-byte indices */
   uint32_t elsize_B = sizeof(uint32_t);
   uint32_t size_B = total * elsize_B;
   uint alloc_B = poly_heap_alloc_nonatomic_offs(p->heap, size_B);
   p->index_buffer = (global uint32_t *)(((uintptr_t)p->heap->base) + alloc_B);

   /* ...and now we can generate the API indexed draw */
   global uint32_t *desc = p->out_draws;

   desc[0] = total;              /* count */
   desc[1] = 1;                  /* instance_count */
   desc[2] = alloc_B / elsize_B; /* start */
   desc[3] = 0;                  /* index_bias */
   desc[4] = 0;                  /* start_instance */

   /* If necessary, increment clipper statistics too. This is only used when
    * there's no geometry shader following us. See poly_nir_lower_gs.c for more
    * info on the emulation. We just need to calculate the # of primitives
    * tessellated.
    */
   if (increment_stats__2) {
      uint prims = p->points_mode ? total
                   : p->isolines  ? (total / 2)
                                  : (total / 3);

      poly_increment_counters(c_prims, c_invs, NULL, prims);
   }
}
