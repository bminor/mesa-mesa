/*
 * Copyright © 2025 Igalia S.L.
 * SPDX-License-Identifier: MIT
 */

#include "ir3.h"
#include "ir3_compiler.h"
#include "ir3_context.h"
#include "ir3_shader.h"

bool
ir3_imm_const_to_preamble(struct ir3 *ir, struct ir3_shader_variant *so)
{
   if (!ir->compiler->load_shader_consts_via_preamble) {
      return false;
   }

   const struct ir3_const_state *consts = ir3_const_state(so);
   struct ir3_imm_const_state *imms = &so->imm_state;

   if (imms->count == 0) {
      return false;
   }

   if (!ir3_has_preamble(ir)) {
      ir3_create_empty_preamble(ir);
   }

   assert(ir3_start_block(ir)->successors[0]);
   struct ir3_block *preamble_start =
      ir3_start_block(ir)->successors[0]->successors[0];
   assert(preamble_start);

   struct ir3_builder build = ir3_builder_at(ir3_before_block(preamble_start));

   for (unsigned i = 0; i < imms->count; i += 4) {
      unsigned components = MIN2(imms->count - i, 4);
      struct ir3_instruction *movs[4];

      for (unsigned c = 0; c < components; c++) {
         movs[c] = create_immed_shared(&build, imms->values[i + c], true);
      }

      struct ir3_instruction *src =
         ir3_create_collect(&build, movs, components);
      unsigned dst = ir3_const_imm_index_to_reg(consts, i);
      struct ir3_instruction *stc = ir3_store_const(so, &build, src, dst);

      /* We cannot run ir3_cp anymore as that would potentially lower more
       * immediates to const registers because we reset count to 0 below (which
       * is necessary to stop the driver from uploading the immediates). So we
       * have to manually propagate the stc immediate.
       */
      struct ir3_instruction *mov_imm = stc->srcs[0]->def->instr;
      assert(mov_imm->opc == OPC_MOV);
      assert(mov_imm->srcs[0]->flags & IR3_REG_IMMED);

      stc->srcs[0] = mov_imm->srcs[0];
      list_del(&mov_imm->node);
   }

   imms->count = 0;
   return true;
}
