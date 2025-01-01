/*
 * Copyright © 2025 Imagination Technologies Ltd.
 *
 * SPDX-License-Identifier: MIT
 */

/**
 * \file pco_cf.c
 *
 * \brief PCO control-flow passes.
 */

#include "compiler/list.h"
#include "pco.h"
#include "pco_builder.h"
#include "util/bitscan.h"
#include "util/bitset.h"
#include "util/macros.h"
#include "util/ralloc.h"
#include "util/u_dynarray.h"

#include <assert.h>
#include <stdbool.h>

static pco_ref emc_ref(pco_func *func, pco_builder *b)
{
   if (pco_ref_is_null(func->emc)) {
      /* Allocate and initialize the emc. */
      func->emc = pco_ref_new_vreg(func);

      pco_cndst(b,
                pco_ref_pred(PCO_PRED_PE),
                func->emc,
                pco_zero,
                pco_ref_imm8(1),
                .exec_cnd = PCO_EXEC_CND_EX_ZX,
                .cnd = PCO_CND_ALWAYS);
   }

   return func->emc;
}

static inline bool can_pred_exec(pco_if *pif)
{
   /* TODO */
   return false;
}

static inline void
lower_if_pred_exec(pco_if *pif, bool has_else, bool invert_cond)
{
   /* TODO */
}

static inline pco_block *cf_section_create(pco_func *func,
                                           pco_cf_node *parent_cf_node,
                                           struct exec_list *cf_node_list,
                                           enum pco_cf_node_flag flag)
{
   assert(flag == PCO_CF_NODE_FLAG_PROLOGUE ||
          flag == PCO_CF_NODE_FLAG_INTERLOGUE ||
          flag == PCO_CF_NODE_FLAG_EPILOGUE);

   pco_block *block = pco_block_create(func);
   block->cf_node.parent = parent_cf_node;
   block->cf_node.flag = flag;
   exec_list_push_tail(cf_node_list, &block->cf_node.node);

   return block;
}

static inline void lower_if(pco_if *pif, bool has_else, bool invert_cond)
{
   pco_func *func = pif->parent_func;

   pco_block *prologue = cf_section_create(func,
                                           &pif->cf_node,
                                           &pif->prologue,
                                           PCO_CF_NODE_FLAG_PROLOGUE);

   pco_block *interlogue = has_else
                              ? cf_section_create(func,
                                                  &pif->cf_node,
                                                  &pif->interlogue,
                                                  PCO_CF_NODE_FLAG_INTERLOGUE)
                              : NULL;

   pco_block *epilogue = cf_section_create(func,
                                           &pif->cf_node,
                                           &pif->epilogue,
                                           PCO_CF_NODE_FLAG_EPILOGUE);

   /* Setup the prologue. */
   pco_builder b = pco_builder_create(func, pco_cursor_after_block(prologue));
   pco_ref emc = emc_ref(func, &b);

   /* TODO: see if the cond producer can set p0 directly. */
   pco_tstz(&b,
            pco_ref_null(),
            pco_ref_pred(PCO_PRED_P0),
            pif->cond,
            .tst_type_main = PCO_TST_TYPE_MAIN_U32);

   pco_cndst(&b,
             pco_ref_pred(PCO_PRED_PE),
             emc,
             emc,
             pco_ref_imm8(1),
             .exec_cnd = PCO_EXEC_CND_EX_ZX,
             .cnd = invert_cond ? PCO_CND_P0_TRUE : PCO_CND_P0_FALSE);

   pco_br(&b,
          has_else ? &interlogue->cf_node : &epilogue->cf_node,
          .branch_cnd = PCO_BRANCH_CND_ALLINST);

   /* Setup the interlogue (if needed). */
   if (has_else) {
      b.cursor = pco_cursor_after_block(interlogue);

      pco_cndef(&b,
                pco_ref_pred(PCO_PRED_PE),
                emc,
                emc,
                pco_ref_imm8(1),
                .exec_cnd = PCO_EXEC_CND_EX_ZX,
                .cnd = PCO_CND_ALWAYS);

      pco_br(&b, &epilogue->cf_node, .branch_cnd = PCO_BRANCH_CND_ALLINST);
   }

   /* Setup the epilogue. */
   b.cursor = pco_cursor_after_block(epilogue);

   pco_cndend(&b,
              pco_ref_pred(PCO_PRED_PE),
              emc,
              emc,
              pco_ref_imm8(1),
              .exec_cnd = PCO_EXEC_CND_EX_ZX);

   pif->cond = pco_ref_null();
}

static inline bool lower_ifs(pco_func *func)
{
   bool progress = false;

   pco_foreach_if_in_func (pif, func) {
      assert(!pco_ref_is_null(pif->cond));

      assert(exec_list_is_empty(&pif->prologue));
      assert(exec_list_is_empty(&pif->interlogue));
      assert(exec_list_is_empty(&pif->epilogue));

      bool has_then = !exec_list_is_empty(&pif->then_body);
      bool has_else = !exec_list_is_empty(&pif->else_body);
      assert(has_then || has_else);

      /* If we only have an else body, invert the condition and bodies. */
      bool invert_cond = false;
      if (!has_then && has_else) {
         struct exec_list temp;
         memcpy(&temp, &pif->then_body, sizeof(pif->then_body));
         memcpy(&pif->then_body, &pif->else_body, sizeof(pif->else_body));
         memcpy(&pif->else_body, &pif->then_body, sizeof(pif->then_body));
         invert_cond = true;

         has_then = true;
         has_else = false;
      }

      assert(has_then);

      if (can_pred_exec(pif))
         lower_if_pred_exec(pif, has_else, invert_cond);
      else
         lower_if(pif, has_else, invert_cond);

      progress = true;
   }

   return progress;
}

static inline bool pco_lower_cf(pco_func *func)
{
   bool progress = false;

   progress |= lower_ifs(func);
   /* TODO: lower_loops(func); */

   return progress;
}

/**
 * \brief Control-flow pass.
 *
 * \param[in,out] shader PCO shader.
 * \return True if the pass made progress.
 */
bool pco_cf(pco_shader *shader)
{
   bool progress = false;

   pco_foreach_func_in_shader (func, shader) {
      progress |= pco_lower_cf(func);
   }

   return progress;
}
