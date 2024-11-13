/*
 * Copyright © 2024 Imagination Technologies Ltd.
 *
 * SPDX-License-Identifier: MIT
 */

/**
 * \file pco_ra.c
 *
 * \brief PCO register allocator.
 */

#include "hwdef/rogue_hw_utils.h"
#include "pco.h"
#include "pco_builder.h"
#include "pco_internal.h"
#include "util/bitset.h"
#include "util/hash_table.h"
#include "util/macros.h"
#include "util/register_allocate.h"
#include "util/sparse_array.h"
#include "util/u_dynarray.h"

#include <assert.h>
#include <stdbool.h>
#include <stdint.h>

/** Live range of an SSA variable. */
struct live_range {
   unsigned start;
   unsigned end;
};

/** Vector user. */
struct vec_user {
   pco_instr *instr;
   unsigned src;
   pco_instr *vec;
};

/**
 * \brief Performs register allocation on a function.
 *
 * \param[in,out] func PCO shader.
 * \param[in] allocable_temps Number of allocatable temp registers.
 * \param[in] allocable_vtxins Number of allocatable vertex input registers.
 * \param[in] allocable_interns Number of allocatable internal registers.
 * \return True if registers were allocated.
 */
static bool pco_ra_func(pco_func *func,
                        unsigned allocable_temps,
                        unsigned allocable_vtxins,
                        unsigned allocable_interns)
{
   /* TODO: support multiple functions and calls. */
   assert(func->type == PCO_FUNC_TYPE_ENTRYPOINT);

   /* No registers to allocate. */
   if (!func->next_ssa)
      return false;

   /* TODO: loop lifetime extension.
    * TODO: track successors/predecessors.
    */

   /* Collect used bit sizes. */
   uint8_t ssa_bits = 0;
   pco_foreach_instr_in_func (instr, func) {
      pco_foreach_instr_dest_ssa (pdest, instr) {
         ssa_bits |= (1 << pdest->bits);
      }
   }

   /* 64-bit SSA should've been lowered by now. */
   assert(!(ssa_bits & (1 << PCO_BITS_64)));

   /* TODO: support multiple bit sizes. */
   bool only_32bit = ssa_bits == (1 << PCO_BITS_32);
   assert(only_32bit);

   struct ra_regs *ra_regs =
      ra_alloc_reg_set(func, allocable_temps, !only_32bit);

   /* Allocate classes. */
   struct hash_table_u64 *ra_classes = _mesa_hash_table_u64_create(ra_regs);
   pco_foreach_instr_in_func (instr, func) {
      pco_foreach_instr_dest_ssa (pdest, instr) {
         unsigned chans = pco_ref_get_chans(*pdest);
         if (_mesa_hash_table_u64_search(ra_classes, chans))
            continue;

         struct ra_class *ra_class = ra_alloc_contig_reg_class(ra_regs, chans);
         _mesa_hash_table_u64_insert(ra_classes, chans, ra_class);
      }
   }

   /* Assign registers to classes. */
   hash_table_u64_foreach (ra_classes, entry) {
      const unsigned stride = entry.key;
      struct ra_class *ra_class = entry.data;

      for (unsigned t = 0; t < allocable_temps - (stride - 1); ++t)
         ra_class_add_reg(ra_class, t);
   }

   ra_set_finalize(ra_regs, NULL);

   struct ra_graph *ra_graph =
      ra_alloc_interference_graph(ra_regs, func->next_ssa);
   ralloc_steal(ra_regs, ra_graph);

   /* Allocate and calculate live ranges. */
   struct live_range *live_ranges =
      rzalloc_array_size(ra_regs, sizeof(*live_ranges), func->next_ssa);

   for (unsigned u = 0; u < func->next_ssa; ++u) {
      live_ranges[u].start = ~0U;
   }

   pco_foreach_instr_in_func (instr, func) {
      pco_foreach_instr_dest_ssa (pdest, instr) {
         assert(live_ranges[pdest->val].start == ~0U);
         live_ranges[pdest->val].start = instr->index;

         unsigned chans = pco_ref_get_chans(*pdest);
         struct ra_class *ra_class =
            _mesa_hash_table_u64_search(ra_classes, chans);
         assert(ra_class);

         ra_set_node_class(ra_graph, pdest->val, ra_class);
      }

      pco_foreach_instr_src_ssa (psrc, instr) {
         live_ranges[psrc->val].end =
            MAX2(live_ranges[psrc->val].end, instr->index);
      }
   }

   /* Build interference graph from overlapping live ranges. */
   for (unsigned ssa0 = 0; ssa0 < func->next_ssa; ++ssa0) {
      for (unsigned ssa1 = ssa0 + 1; ssa1 < func->next_ssa; ++ssa1) {
         /* If the live ranges overlap, the register nodes interfere. */
         if (!(live_ranges[ssa0].start >= live_ranges[ssa1].end ||
               live_ranges[ssa1].start >= live_ranges[ssa0].end)) {
            ra_add_node_interference(ra_graph, ssa0, ssa1);
         }
      }
   }

   bool allocated = ra_allocate(ra_graph);
   assert(allocated);
   /* TODO: spilling. */

   /* Collect info on users of vec ops. */
   struct util_dynarray vec_users;
   struct util_dynarray vecs;
   BITSET_WORD *instrs_using_vecs =
      rzalloc_array_size(ra_regs,
                         sizeof(*instrs_using_vecs),
                         BITSET_WORDS(func->next_instr));
   BITSET_WORD *instrs_using_multi_vecs =
      rzalloc_array_size(ra_regs,
                         sizeof(*instrs_using_multi_vecs),
                         BITSET_WORDS(func->next_instr));

   util_dynarray_init(&vec_users, ra_regs);
   util_dynarray_init(&vecs, ra_regs);

   pco_foreach_instr_in_func (vec, func) {
      if (vec->op != PCO_OP_VEC)
         continue;

      util_dynarray_append(&vecs, pco_instr *, vec);

      const pco_ref vec_dest = vec->dest[0];
      assert(pco_ref_is_ssa(vec_dest));

      pco_foreach_instr_in_func_from (instr, vec) {
         pco_foreach_instr_src_ssa (psrc, instr) {
            if (psrc->val != vec_dest.val)
               continue;

            /* TODO: for now we're just supporting instructions producing
             * scalars (or with no outputs).
             * */
            assert(!instr->num_dests ||
                   (instr->num_dests == 1 &&
                    pco_ref_get_chans(instr->dest[0]) == 1));

            if (BITSET_TEST(instrs_using_vecs, instr->index))
               BITSET_SET(instrs_using_multi_vecs, instr->index);

            BITSET_SET(instrs_using_vecs, instr->index);

            struct vec_user vec_user = {
               .instr = instr,
               .src = psrc - instr->src,
               .vec = vec,
            };
            util_dynarray_append(&vec_users, struct vec_user, vec_user);
         }
      }
   }

   /* TODO: support this. */
   assert(__bitset_is_empty(instrs_using_multi_vecs,
                            BITSET_WORDS(func->next_instr)));

   /* Replace SSA regs with allocated temps. */
   unsigned temps = 0;
   pco_foreach_instr_in_func_safe (instr, func) {
      pco_foreach_instr_dest_ssa (pdest, instr) {
         pdest->type = PCO_REF_TYPE_REG;
         pdest->reg_class = PCO_REG_CLASS_TEMP;
         pdest->val = ra_get_node_reg(ra_graph, pdest->val);
         temps = MAX2(temps, pdest->val + pco_ref_get_chans(*pdest));
      }

      pco_foreach_instr_src_ssa (psrc, instr) {
         psrc->type = PCO_REF_TYPE_REG;
         psrc->reg_class = PCO_REG_CLASS_TEMP;
         psrc->val = ra_get_node_reg(ra_graph, psrc->val);
      }
   }

   /* Scalarize the users of any vec ops that haven't been consumed in
    * other passes; no point wasting regs with copies unless it's unavoidable.
    */
   /* TODO: distinguish between and support instructions that need vecs/can't be
    * scalarized, e.g. sample data words.
    */
   /* TODO: try and do this in a separate earlier pass, taking into account the
    * cost/benefit analysis of scalarizing.
    */
   util_dynarray_foreach (&vec_users, struct vec_user, vec_user) {
      pco_instr *instr = vec_user->instr;
      pco_instr *vec = vec_user->vec;

      switch (instr->op) {
      case PCO_OP_UVSW_WRITE: {
         assert(vec_user->src == 0);
         assert(pco_instr_get_rpt(instr) == vec->num_srcs);

         pco_builder b =
            pco_builder_create(func, pco_cursor_after_instr(instr));
         uint8_t vtxout_base_addr = pco_ref_get_imm(instr->src[1]);

         for (unsigned s = 0; s < vec->num_srcs; ++s)
            pco_uvsw_write(&b, vec->src[s], pco_ref_val8(vtxout_base_addr + s));

         break;
      }

      default:
         unreachable();
      }

      pco_instr_delete(instr);
   }

   /* TODO: process/fold comp ops as well? */

   /* Drop vec ops. */
   util_dynarray_foreach (&vecs, pco_instr *, vec) {
      pco_instr_delete(*vec);
   }

   ralloc_free(ra_regs);

   func->temps = temps;

   return true;
}

/**
 * \brief Register allocation pass.
 *
 * \param[in,out] shader PCO shader.
 * \return True if the pass made progress.
 */
bool pco_ra(pco_shader *shader)
{
   assert(!shader->is_grouped);

   unsigned hw_temps = rogue_get_temps(shader->ctx->dev_info);
   /* TODO:
    * unsigned opt_temps = rogue_get_optimal_temps(shader->ctx->dev_info);
    */

   /* TODO: different number of temps available if preamble/phase change. */
   /* TODO: different number of temps available if barriers are in use. */
   /* TODO: support for internal and vtxin registers. */
   unsigned allocable_temps = hw_temps;
   unsigned allocable_vtxins = 0;
   unsigned allocable_interns = 0;

   /* Perform register allocation for each function. */
   bool progress = false;
   pco_foreach_func_in_shader (func, shader) {
      progress |= pco_ra_func(func,
                              allocable_temps,
                              allocable_vtxins,
                              allocable_interns);
   }

   return progress;
}
