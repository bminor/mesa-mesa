/*
 * Copyright © 2014 Valve Corporation
 * SPDX-License-Identifier: MIT
 */

#include "ir3.h"

#define XXH_INLINE_ALL
#include "util/xxhash.h"

/* This pass handles CSE'ing repeated expressions created in the process of
 * translating from NIR. Currently this is just collect's. Also, currently
 * this is intra-block only, to make it work over multiple block we'd need to
 * bring forward dominance calculation.
 */

#define HASH(hash, data) XXH32(&(data), sizeof(data), hash)

static uint32_t
hash_instr(const void *data)
{
   const struct ir3_instruction *instr = data;
   uint32_t hash = 0;

   hash = HASH(hash, instr->opc);
   hash = HASH(hash, instr->dsts[0]->flags);
   hash = HASH(hash, instr->dsts[0]->num);
   foreach_src (src, (struct ir3_instruction *)instr) {
      if (src->flags & IR3_REG_CONST) {
         if (src->flags & IR3_REG_RELATIV)
            hash = HASH(hash, src->array.offset);
         else
            hash = HASH(hash, src->num);
      } else if (src->flags & IR3_REG_IMMED) {
         hash = HASH(hash, src->uim_val);
      } else {
         if (src->flags & IR3_REG_ARRAY)
            hash = HASH(hash, src->array.offset);
         hash = HASH(hash, src->def);
      }
   }

   if (opc_cat(instr->opc) == 1) {
      hash = HASH(hash, instr->cat1.dst_type);
      hash = HASH(hash, instr->cat1.src_type);
      hash = HASH(hash, instr->cat1.round);
   }

   return hash;
}

static bool
instrs_equal(const struct ir3_instruction *i1, const struct ir3_instruction *i2)
{
   if (i1->opc != i2->opc)
      return false;

   if (i1->dsts_count != i2->dsts_count)
      return false;

   if (i1->srcs_count != i2->srcs_count)
      return false;

   if (i1->dsts[0]->flags != i2->dsts[0]->flags)
      return false;

   if (i1->dsts[0]->num != i2->dsts[0]->num)
      return false;

   for (unsigned i = 0; i < i1->srcs_count; i++) {
      const struct ir3_register *i1_reg = i1->srcs[i], *i2_reg = i2->srcs[i];

      if (i1_reg->flags != i2_reg->flags)
         return false;

      if (i1_reg->flags & IR3_REG_CONST) {
         if (i1_reg->flags & IR3_REG_RELATIV) {
            if (i1_reg->array.offset != i2_reg->array.offset)
               return false;
         } else {
            if (i1_reg->num != i2_reg->num)
               return false;
         }
      } else if (i1_reg->flags & IR3_REG_IMMED) {
         if (i1_reg->uim_val != i2_reg->uim_val)
            return false;
      } else {
         if (i1_reg->flags & IR3_REG_ARRAY) {
            if (i1_reg->array.offset != i2_reg->array.offset)
               return false;
         }
         if (i1_reg->def != i2_reg->def)
            return false;
      }
   }

   if (opc_cat(i1->opc) == 1) {
      if (i1->cat1.dst_type != i2->cat1.dst_type ||
          i1->cat1.src_type != i2->cat1.src_type ||
          i1->cat1.round != i2->cat1.round)
         return false;
   }

   return true;
}

static bool
instr_can_cse(const struct ir3_instruction *instr)
{
   if (instr->opc != OPC_META_COLLECT && instr->opc != OPC_MOV)
      return false;

   if (reg_num(instr->dsts[0]) == REG_A0)
      return true;

   if (!is_dest_gpr(instr->dsts[0]) || (instr->dsts[0]->flags & IR3_REG_ARRAY))
      return false;

   return true;
}

static bool
cmp_func(const void *data1, const void *data2)
{
   return instrs_equal(data1, data2);
}

bool
ir3_cse(struct ir3 *ir)
{
   struct set *instr_set = _mesa_set_create(NULL, hash_instr, cmp_func);
   foreach_block (block, &ir->block_list) {
      _mesa_set_clear(instr_set, NULL);

      foreach_instr (instr, &block->instr_list) {
         instr->data = NULL;

         if (!instr_can_cse(instr))
            continue;

         bool found;
         struct set_entry *entry =
            _mesa_set_search_or_add(instr_set, instr, &found);
         if (found)
            instr->data = (void *)entry->key;
      }
   }

   bool progress = false;
   foreach_block (block, &ir->block_list) {
      foreach_instr (instr, &block->instr_list) {
         foreach_src (src, instr) {
            if ((src->flags & IR3_REG_SSA) && src->def &&
                src->def->instr->data) {
               progress = true;
               struct ir3_instruction *instr = src->def->instr->data;
               src->def = instr->dsts[0];
            }
         }
      }
   }

   _mesa_set_destroy(instr_set, NULL);
   return progress;
}
