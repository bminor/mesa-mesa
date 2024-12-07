/*
 * Copyright © 2010-2012 Intel Corporation
 * SPDX-License-Identifier: MIT
 */

#include "brw_analysis.h"
#include "brw_cfg.h"
#include "brw_shader.h"

/* Calculates the immediate dominator of each block, according to "A Simple,
 * Fast Dominance Algorithm" by Keith D. Cooper, Timothy J. Harvey, and Ken
 * Kennedy.
 *
 * The authors claim that for control flow graphs of sizes normally encountered
 * (less than 1000 nodes) that this algorithm is significantly faster than
 * others like Lengauer-Tarjan.
 */
brw_idom_tree::brw_idom_tree(const brw_shader *s) :
   num_parents(s->cfg->num_blocks),
   parents(new bblock_t *[num_parents]())
{
   bool changed;

   parents[0] = s->cfg->blocks[0];

   do {
      changed = false;

      foreach_block(block, s->cfg) {
         if (block->num == 0)
            continue;

         bblock_t *new_idom = NULL;
         foreach_list_typed(bblock_link, parent_link, link, &block->parents) {
            if (parent(parent_link->block)) {
               new_idom = (new_idom ? intersect(new_idom, parent_link->block) :
                           parent_link->block);
            }
         }

         if (parent(block) != new_idom) {
            parents[block->num] = new_idom;
            changed = true;
         }
      }
   } while (changed);
}

brw_idom_tree::~brw_idom_tree()
{
   delete[] parents;
}

bblock_t *
brw_idom_tree::intersect(bblock_t *b1, bblock_t *b2) const
{
   /* Note, the comparisons here are the opposite of what the paper says
    * because we index blocks from beginning -> end (i.e. reverse post-order)
    * instead of post-order like they assume.
    */
   while (b1->num != b2->num) {
      while (b1->num > b2->num)
         b1 = parent(b1);
      while (b2->num > b1->num)
         b2 = parent(b2);
   }
   assert(b1);
   return b1;
}

void
brw_idom_tree::dump(FILE *file) const
{
   fprintf(file, "digraph DominanceTree {\n");
   for (unsigned i = 0; i < num_parents; i++)
      fprintf(file, "\t%d -> %d\n", parents[i]->num, i);
   fprintf(file, "}\n");
}

brw_register_pressure::brw_register_pressure(const brw_shader *v)
{
   const brw_live_variables &live = v->live_analysis.require();
   const unsigned num_instructions = v->cfg->num_blocks ?
      v->cfg->blocks[v->cfg->num_blocks - 1]->end_ip + 1 : 0;

   regs_live_at_ip = new unsigned[num_instructions]();

   for (unsigned reg = 0; reg < v->alloc.count; reg++) {
      for (int ip = live.vgrf_start[reg]; ip <= live.vgrf_end[reg]; ip++)
         regs_live_at_ip[ip] += v->alloc.sizes[reg];
   }

   const unsigned payload_count = v->first_non_payload_grf;

   int *payload_last_use_ip = new int[payload_count];
   v->calculate_payload_ranges(true, payload_count, payload_last_use_ip);

   for (unsigned reg = 0; reg < payload_count; reg++) {
      for (int ip = 0; ip < payload_last_use_ip[reg]; ip++)
         ++regs_live_at_ip[ip];
   }

   delete[] payload_last_use_ip;
}

brw_register_pressure::~brw_register_pressure()
{
   delete[] regs_live_at_ip;
}
