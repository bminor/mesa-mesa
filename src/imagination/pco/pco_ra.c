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
#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>

/** Live range of an SSA variable. */
struct live_range {
   unsigned start;
   unsigned end;
};

/** Vector override information. */
struct vec_override {
   pco_ref ref;
   unsigned offset;
};

/**
 * \brief Checks if a vec has ssa sources that are referenced more than once.
 *
 * \param[in] vec Vec instruction.
 * \return True if the vec has ssa sources that are referenced more than once.
 */
static bool vec_has_repeated_ssas(pco_instr *vec)
{
   assert(vec->op == PCO_OP_VEC);

   pco_foreach_instr_src_ssa (psrc, vec) {
      pco_foreach_instr_src_ssa_from (psrc_inner, vec, psrc) {
         if (psrc_inner->val == psrc->val)
            return true;
      }
   }

   return false;
}

static void pco_extend_live_range(pco_ref origin,
                                  pco_ref current_ref,
                                  pco_instr *current_instr,
                                  struct hash_table_u64 *overrides,
                                  struct live_range *live_ranges)
{
   pco_foreach_instr_in_func_from (instr, current_instr) {
      pco_foreach_instr_src_ssa (psrc, instr) {
         if (current_ref.val != psrc->val)
            continue;

         pco_foreach_instr_dest_ssa (pdest, instr) {
            struct vec_override *override =
               _mesa_hash_table_u64_search(overrides, pdest->val);

            if (override) {
               live_ranges[origin.val].end =
                  MAX2(live_ranges[origin.val].end,
                       live_ranges[override->ref.val].end);
               break;
            }

            live_ranges[origin.val].end =
               MAX2(live_ranges[origin.val].end, instr->index);
         }

         break;
      }
   }
}

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

   /* TODO: loop lifetime extension.
    * TODO: track successors/predecessors.
    */

   unsigned num_ssas = func->next_ssa;
   unsigned num_vregs = func->next_vreg;
   unsigned num_vars = num_ssas + num_vregs;

   /* Collect used bit sizes. */
   uint8_t used_bits = 0;
   pco_foreach_instr_in_func (instr, func) {
      pco_foreach_instr_dest_ssa (pdest, instr) {
         used_bits |= (1 << pdest->bits);
      }
   }

   /* vregs are always 32x1. */
   if (num_vregs > 0) {
      used_bits |= (1 << PCO_BITS_32);
   }

   /* No registers to allocate. */
   if (!used_bits)
      return false;

   /* 64-bit vars should've been lowered by now. */
   assert(!(used_bits & (1 << PCO_BITS_64)));

   /* TODO: support multiple bit sizes. */
   bool only_32bit = used_bits == (1 << PCO_BITS_32);
   assert(only_32bit);

   struct ra_regs *ra_regs =
      ra_alloc_reg_set(func, allocable_temps, !only_32bit);

   BITSET_WORD *comps =
      rzalloc_array_size(ra_regs, sizeof(*comps), BITSET_WORDS(num_ssas));

   /* Overrides for vector coalescing. */
   struct hash_table_u64 *overrides = _mesa_hash_table_u64_create(ra_regs);
   pco_foreach_instr_in_func_rev (instr, func) {
      if (instr->op != PCO_OP_VEC)
         continue;

      /* Can't override vec ssa sources if they're referenced more than once. */
      if (vec_has_repeated_ssas(instr))
         continue;

      pco_ref dest = instr->dest[0];
      unsigned offset = 0;

      struct vec_override *src_override =
         _mesa_hash_table_u64_search(overrides, dest.val);

      if (src_override) {
         dest = src_override->ref;
         offset += src_override->offset;
      }

      pco_foreach_instr_src (psrc, instr) {
         /* TODO: skip if vector producer is used by multiple things in a way
          * that doesn't allow coalescing. */
         /* TODO: can NIR scalarise things so that the only remaining vectors
          * can be used in this way? */

         if (pco_ref_is_ssa(*psrc)) {
            /* Make sure this hasn't already been overridden somewhere else! */
            if (_mesa_hash_table_u64_search(overrides, psrc->val)) {
               BITSET_SET(comps, psrc->val);
               continue;
            }

            struct vec_override *src_override =
               rzalloc_size(overrides, sizeof(*src_override));
            src_override->ref = dest;
            src_override->offset = offset;

            _mesa_hash_table_u64_insert(overrides, psrc->val, src_override);
         }

         offset += pco_ref_get_chans(*psrc);
      }
   }

   /* Overrides for vector component uses. */
   pco_foreach_instr_in_func (instr, func) {
      if (instr->op != PCO_OP_COMP)
         continue;

      pco_ref dest = instr->dest[0];
      pco_ref src = instr->src[0];
      unsigned offset = pco_ref_get_imm(instr->src[1]);

      BITSET_SET(comps, dest.val);

      assert(pco_ref_is_ssa(src));
      assert(pco_ref_is_ssa(dest));

      struct vec_override *vec_override =
         _mesa_hash_table_u64_search(overrides, src.val);

      if (vec_override) {
         src = vec_override->ref;
         offset += vec_override->offset;
      }

      struct vec_override *src_override =
         rzalloc_size(overrides, sizeof(*src_override));
      src_override->ref = src;
      src_override->offset = offset;
      _mesa_hash_table_u64_insert(overrides, dest.val, src_override);
   }

   /* Allocate classes. */
   struct hash_table_u64 *ra_classes = _mesa_hash_table_u64_create(ra_regs);
   pco_foreach_instr_in_func (instr, func) {
      pco_foreach_instr_dest_ssa (pdest, instr) {
         unsigned chans = pco_ref_get_chans(*pdest);
         /* TODO: bitset instead of search? */
         if (_mesa_hash_table_u64_search(ra_classes, chans))
            continue;

         /* Skip if collated. */
         if (_mesa_hash_table_u64_search(overrides, pdest->val))
            continue;

         struct ra_class *ra_class = ra_alloc_contig_reg_class(ra_regs, chans);
         _mesa_hash_table_u64_insert(ra_classes, chans, ra_class);
      }
   }

   /* vregs are always 32x1. */
   if (num_vregs > 0) {
      if (!_mesa_hash_table_u64_search(ra_classes, 1)) {
         struct ra_class *ra_class = ra_alloc_contig_reg_class(ra_regs, 1);
         _mesa_hash_table_u64_insert(ra_classes, 1, ra_class);
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

   struct ra_graph *ra_graph = ra_alloc_interference_graph(ra_regs, num_vars);
   ralloc_steal(ra_regs, ra_graph);

   /* Allocate and calculate live ranges. */
   struct live_range *live_ranges =
      rzalloc_array_size(ra_regs, sizeof(*live_ranges), num_vars);

   for (unsigned u = 0; u < num_vars; ++u)
      live_ranges[u].start = ~0U;

   pco_foreach_instr_in_func (instr, func) {
      pco_foreach_instr_dest_ssa (pdest, instr) {
         pco_ref dest = *pdest;
         struct vec_override *override =
            _mesa_hash_table_u64_search(overrides, dest.val);

         if (override)
            dest = override->ref;

         live_ranges[dest.val].start =
            MIN2(live_ranges[dest.val].start, instr->index);

         if (override)
            continue;

         /* Set class if it hasn't already been set up in an override. */
         unsigned chans = pco_ref_get_chans(dest);
         struct ra_class *ra_class =
            _mesa_hash_table_u64_search(ra_classes, chans);
         assert(ra_class);

         ra_set_node_class(ra_graph, dest.val, ra_class);
      }

      pco_foreach_instr_src_ssa (psrc, instr) {
         pco_ref src = *psrc;
         struct vec_override *override =
            _mesa_hash_table_u64_search(overrides, src.val);

         if (override)
            src = override->ref;

         live_ranges[src.val].end =
            MAX2(live_ranges[src.val].end, instr->index);
      }

      pco_foreach_instr_dest_vreg (pdest, instr) {
         pco_ref dest = *pdest;

         /* Place vregs after ssa vars. */
         dest.val += num_ssas;

         live_ranges[dest.val].start =
            MIN2(live_ranges[dest.val].start, instr->index);

         /* Set class if it hasn't already been set up in an override. */
         unsigned chans = pco_ref_get_chans(dest);
         struct ra_class *ra_class =
            _mesa_hash_table_u64_search(ra_classes, chans);
         assert(ra_class);

         ra_set_node_class(ra_graph, dest.val, ra_class);
      }

      pco_foreach_instr_src_vreg (psrc, instr) {
         pco_ref src = *psrc;

         /* Place vregs after ssa vars. */
         src.val += num_ssas;

         live_ranges[src.val].end =
            MAX2(live_ranges[src.val].end, instr->index);
      }
   }

   /* Extend lifetimes of non-overriden vecs that have comp instructions. */
   pco_foreach_instr_in_func (instr, func) {
      if (instr->op != PCO_OP_COMP)
         continue;

      pco_ref dest = instr->dest[0];
      pco_ref src_vec = instr->src[0];

      struct vec_override *vec_override =
         _mesa_hash_table_u64_search(overrides, src_vec.val);

      /* Already taken care of. */
      if (vec_override) {
         assert(live_ranges[src_vec.val].start == ~0U &&
                live_ranges[src_vec.val].end == 0);
         continue;
      }

      pco_extend_live_range(src_vec, dest, instr, overrides, live_ranges);
   }

   /* Extend lifetimes of vars in loops. */
   pco_foreach_loop_in_func (loop, func) {
      pco_block *prologue_block =
         pco_cf_node_as_block(pco_cf_node_head(&loop->prologue));

      pco_block *epilogue_block =
         pco_cf_node_as_block(pco_cf_node_tail(&loop->epilogue));

      unsigned loop_start_index = pco_first_instr(prologue_block)->index;
      unsigned loop_end_index = pco_last_instr(epilogue_block)->index;

      /* If a var is defined before a loop and stops being used during it,
       * extend its lifetime to the end of the loop.
       */

      for (unsigned var = 0; var < num_vars; ++var) {
         if (live_ranges[var].start < loop_start_index &&
             live_ranges[var].end > loop_start_index &&
             live_ranges[var].end < loop_end_index) {
            live_ranges[var].end = loop_end_index;
         }
      }
   }

   /* Build interference graph from overlapping live ranges. */
   for (unsigned var0 = 0; var0 < num_vars; ++var0) {
      for (unsigned var1 = var0 + 1; var1 < num_vars; ++var1) {
         /* If the live ranges overlap, the register nodes interfere. */
         if ((live_ranges[var0].start != ~0U && live_ranges[var1].end != ~0U) &&
             !(live_ranges[var0].start >= live_ranges[var1].end ||
               live_ranges[var1].start >= live_ranges[var0].end)) {
            ra_add_node_interference(ra_graph, var0, var1);
         }
      }
   }

   pco_foreach_instr_in_func_rev (vec, func) {
      if (vec->op != PCO_OP_VEC)
         continue;

      pco_foreach_instr_src_ssa (psrc, vec) {
         ra_add_node_interference(ra_graph, vec->dest[0].val, psrc->val);
      }
   }

   bool allocated = ra_allocate(ra_graph);
   assert(allocated);
   /* TODO: spilling. */

   if (pco_should_print_shader(func->parent_shader) && PCO_DEBUG_PRINT(RA)) {
      printf("RA live ranges:\n");
      for (unsigned u = 0; u < num_vars; ++u)
         printf("  %c%u: %u, %u\n",
                u >= num_ssas ? '$' : '%',
                u >= num_ssas ? u - num_ssas : u,
                live_ranges[u].start,
                live_ranges[u].end);

      if (_mesa_hash_table_u64_num_entries(overrides)) {
         printf("RA overrides:\n");
         hash_table_u64_foreach (overrides, entry) {
            struct vec_override *override = entry.data;
            printf("  %%%" PRIu64 ": ref = ", entry.key);
            pco_print_ref(func->parent_shader, override->ref);
            printf(", offset = %u\n", override->offset);
         }
         printf("\n");
      }
   }

   /* Replace vars with allocated registers. */
   unsigned temps = 0;
   unsigned vtxins = 0;
   unsigned interns = 0;
   pco_foreach_instr_in_func_safe (instr, func) {
      /*
      if (pco_should_print_shader(func->parent_shader) && PCO_DEBUG_PRINT(RA))
         pco_print_shader(func->parent_shader, stdout, "ra debug");
         */

      /* Insert movs for scalar components of super vecs. */
      if (instr->op == PCO_OP_VEC) {
         pco_builder b =
            pco_builder_create(func, pco_cursor_before_instr(instr));

         struct vec_override *override =
            _mesa_hash_table_u64_search(overrides, instr->dest[0].val);

         unsigned offset = override ? override->offset : 0;

         unsigned temp_dest_base =
            override ? ra_get_node_reg(ra_graph, override->ref.val)
                     : ra_get_node_reg(ra_graph, instr->dest[0].val);

         pco_foreach_instr_src (psrc, instr) {
            if (!pco_ref_is_ssa(*psrc) ||
                !_mesa_hash_table_u64_search(overrides, psrc->val) ||
                BITSET_TEST(comps, psrc->val)) {
               unsigned chans = pco_ref_get_chans(*psrc);

               unsigned temp_src_base = ~0U;
               if (pco_ref_is_ssa(*psrc)) {
                  temp_src_base = ra_get_node_reg(ra_graph, psrc->val);

                  struct vec_override *src_override =
                     _mesa_hash_table_u64_search(overrides, psrc->val);
                  if (src_override) {
                     temp_src_base =
                        ra_get_node_reg(ra_graph, src_override->ref.val);
                     temp_src_base += src_override->offset;
                  }
               } else if (pco_ref_is_vreg(*psrc)) {
                  temp_src_base =
                     ra_get_node_reg(ra_graph, psrc->val + num_ssas);
               }

               enum pco_exec_cnd exec_cnd = pco_instr_get_exec_cnd(instr);
               for (unsigned u = 0; u < chans; ++u) {
                  pco_ref dest =
                     pco_ref_hwreg(temp_dest_base + offset, PCO_REG_CLASS_TEMP);
                  dest = pco_ref_offset(dest, u);

                  pco_ref src;
                  if (pco_ref_is_ssa(*psrc) || pco_ref_is_vreg(*psrc))
                     src = pco_ref_hwreg(temp_src_base, PCO_REG_CLASS_TEMP);
                  else
                     src = pco_ref_chans(*psrc, 1);

                  src = pco_ref_offset(src, u);

                  pco_ref_xfer_mods(&src, psrc, false);

                  if (!pco_refs_are_equal(src, dest, true)) {
                     if (pco_ref_is_reg(src) &&
                         pco_ref_get_reg_class(src) == PCO_REG_CLASS_SPEC) {
                        pco_movs1(&b, dest, src, .exec_cnd = exec_cnd);
                     } else {
                        pco_mbyp(&b, dest, src, .exec_cnd = exec_cnd);
                     }
                  }
               }

               temps = MAX2(temps, temp_dest_base + offset + chans);
            }

            offset += pco_ref_get_chans(*psrc);
         }

         pco_instr_delete(instr);
         continue;
      } else if (instr->op == PCO_OP_COMP) {
         pco_instr_delete(instr);
         continue;
      }

      pco_foreach_instr_dest_ssa (pdest, instr) {
         struct vec_override *override =
            _mesa_hash_table_u64_search(overrides, pdest->val);

         unsigned val = ra_get_node_reg(ra_graph, pdest->val);
         unsigned dest_temps = val + pco_ref_get_chans(*pdest);
         if (override) {
            val = ra_get_node_reg(ra_graph, override->ref.val);
            dest_temps = val + pco_ref_get_chans(override->ref);
            val += override->offset;
         }

         pdest->type = PCO_REF_TYPE_REG;
         pdest->reg_class = PCO_REG_CLASS_TEMP;
         pdest->val = val;
         temps = MAX2(temps, dest_temps);
      }

      pco_foreach_instr_src_ssa (psrc, instr) {
         struct vec_override *override =
            _mesa_hash_table_u64_search(overrides, psrc->val);

         unsigned val =
            override
               ? ra_get_node_reg(ra_graph, override->ref.val) + override->offset
               : ra_get_node_reg(ra_graph, psrc->val);

         psrc->type = PCO_REF_TYPE_REG;
         psrc->reg_class = PCO_REG_CLASS_TEMP;
         psrc->val = val;
      }

      pco_foreach_instr_dest_vreg (pdest, instr) {
         unsigned val = ra_get_node_reg(ra_graph, pdest->val + num_ssas);
         unsigned dest_temps = val + 1;

         pdest->type = PCO_REF_TYPE_REG;
         pdest->reg_class = PCO_REG_CLASS_TEMP;
         pdest->val = val;
         temps = MAX2(temps, dest_temps);
      }

      pco_foreach_instr_src_vreg (psrc, instr) {
         unsigned val = ra_get_node_reg(ra_graph, psrc->val + num_ssas);

         psrc->type = PCO_REF_TYPE_REG;
         psrc->reg_class = PCO_REG_CLASS_TEMP;
         psrc->val = val;
      }

      /* Drop no-ops. */
      if (instr->op == PCO_OP_MBYP &&
          (pco_ref_is_ssa(instr->src[0]) || pco_ref_is_vreg(instr->src[0])) &&
          pco_refs_are_equal(instr->src[0], instr->dest[0], true)) {
         pco_instr_delete(instr);
      }
   }

   ralloc_free(ra_regs);

   func->temps = temps;

   if (pco_should_print_shader(func->parent_shader) && PCO_DEBUG_PRINT(RA)) {
      printf(
         "RA allocated %u temps, %u vtxins, %u interns from %u SSA vars, %u vregs.\n",
         temps,
         vtxins,
         interns,
         num_ssas,
         num_vregs);
   }

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

   /* Instruction indices need to be ordered for live ranges. */
   pco_index(shader, false);

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

      shader->data.common.temps = MAX2(shader->data.common.temps, func->temps);
   }

   return progress;
}
