/*
 * Copyright © 2024 Imagination Technologies Ltd.
 *
 * SPDX-License-Identifier: MIT
 */

/**
 * \file pco_trans_nir.c
 *
 * \brief NIR translation functions.
 */

#include "compiler/glsl/list.h"
#include "compiler/shader_enums.h"
#include "pco.h"
#include "pco_builder.h"
#include "pco_internal.h"
#include "util/bitset.h"
#include "util/list.h"
#include "util/macros.h"
#include "util/ralloc.h"

#include <assert.h>
#include <stdio.h>

/** Translation context. */
typedef struct _trans_ctx {
   pco_ctx *pco_ctx; /** PCO compiler context. */
   pco_shader *shader; /** Current shader. */
   pco_func *func; /** Current function. */
   pco_builder b; /** Builder. */
   gl_shader_stage stage; /** Shader stage. */

   BITSET_WORD *float_types; /** NIR SSA float vars. */
   BITSET_WORD *int_types; /** NIR SSA int vars. */
} trans_ctx;

/* Forward declarations. */
static pco_block *trans_cf_nodes(trans_ctx *tctx,
                                 pco_cf_node *parent_cf_node,
                                 struct list_head *cf_node_list,
                                 struct exec_list *nir_cf_node_list);

/**
 * \brief Translates a NIR def into a PCO reference.
 *
 * \param[in] def The nir def.
 * \return The PCO reference.
 */
static inline pco_ref pco_ref_nir_def(const nir_def *def)
{
   return pco_ref_ssa(def->index, def->bit_size, def->num_components);
}

/**
 * \brief Translates a NIR src into a PCO reference.
 *
 * \param[in] src The nir src.
 * \return The PCO reference.
 */
static inline pco_ref pco_ref_nir_src(const nir_src *src)
{
   return pco_ref_nir_def(src->ssa);
}

/**
 * \brief Translates a NIR def into a PCO reference with type information.
 *
 * \param[in] def The nir def.
 * \param[in] tctx Translation context.
 * \return The PCO reference.
 */
static inline pco_ref pco_ref_nir_def_t(const nir_def *def, trans_ctx *tctx)
{
   pco_ref ref = pco_ref_nir_def(def);

   bool is_float = BITSET_TEST(tctx->float_types, def->index);
   bool is_int = BITSET_TEST(tctx->int_types, def->index);

   if (is_float)
      ref.dtype = PCO_DTYPE_FLOAT;
   else if (is_int)
      ref.dtype = PCO_DTYPE_UNSIGNED;

   return ref;
}

/**
 * \brief Translates a NIR src into a PCO reference with type information.
 *
 * \param[in] src The nir src.
 * \param[in] tctx Translation context.
 * \return The PCO reference.
 */
static inline pco_ref pco_ref_nir_src_t(const nir_src *src, trans_ctx *tctx)
{
   return pco_ref_nir_def_t(src->ssa, tctx);
}

/**
 * \brief Translates a NIR alu src into a PCO reference with type information,
 *        extracting and building vectors as needed.
 *
 * \param[in] src The nir src.
 * \param[in,out] tctx Translation context.
 * \return The PCO reference.
 */
static inline pco_ref
pco_ref_nir_alu_src_t(const nir_alu_instr *alu, unsigned src, trans_ctx *tctx)
{
   const nir_alu_src *alu_src = &alu->src[src];
   /* unsigned chans = nir_src_num_components(alu_src->src); */
   unsigned chans = nir_ssa_alu_instr_src_components(alu, src);

   bool seq_comps =
      nir_is_sequential_comp_swizzle((uint8_t *)alu_src->swizzle, chans);
   pco_ref ref = pco_ref_nir_src_t(&alu_src->src, tctx);
   unsigned swizzle0 = alu_src->swizzle[0];

   /* Multiple channels, but referencing the entire vector; return as-is. */
   if (!swizzle0 && seq_comps && chans == nir_src_num_components(alu_src->src))
      return ref;

   /* One channel; just extract it. */
   pco_ref var = pco_ref_new_ssa(tctx->func, pco_ref_get_bits(ref), chans);
   if (chans == 1) {
      pco_ref comp = pco_ref_val16(swizzle0);
      pco_comp(&tctx->b, var, ref, comp);
      return var;
   }

   /* Multiple channels; extract each into a vec. */
   pco_ref chan_comps[NIR_MAX_VEC_COMPONENTS] = { 0 };
   for (unsigned u = 0; u < chans; ++u) {
      pco_ref comp = pco_ref_val16(alu_src->swizzle[u]);
      chan_comps[u] = pco_ref_new_ssa(tctx->func, pco_ref_get_bits(ref), 1);
      pco_comp(&tctx->b, chan_comps[u], ref, comp);
   }

   pco_vec(&tctx->b, var, chans, chan_comps);
   return var;
}

/**
 * \brief Translates a NIR vs load_input intrinsic into PCO.
 *
 * \param[in,out] tctx Translation context.
 * \param[in] intr load_input intrinsic.
 * \param[in] dest Instruction destination.
 * \return The translated PCO instruction.
 */
static pco_instr *
trans_load_input_vs(trans_ctx *tctx, nir_intrinsic_instr *intr, pco_ref dest)
{
   puts("finishme: trans_load_input_vs");

   unsigned base = nir_intrinsic_base(intr);
   unsigned component = nir_intrinsic_component(intr);
   unsigned chans = pco_ref_get_chans(dest);

   const nir_src offset = intr->src[0];
   assert(nir_src_as_uint(offset) == 0);

   /* TODO NEXT: Wrong! Do properly! */
   unsigned vtxin_offset = (4 * base) + component;
   pco_ref src = pco_ref_hwreg_vec(vtxin_offset, PCO_REG_CLASS_VTXIN, chans);

   return pco_mov(&tctx->b, dest, src, .rpt = chans);
}

/**
 * \brief Translates a NIR vs store_output intrinsic into PCO.
 *
 * \param[in,out] tctx Translation context.
 * \param[in] intr store_output intrinsic.
 * \param[in] src Instruction source.
 * \return The translated PCO instruction.
 */
static pco_instr *
trans_store_output_vs(trans_ctx *tctx, nir_intrinsic_instr *intr, pco_ref src)
{
   puts("finishme: trans_store_output_vs");

   unsigned base = nir_intrinsic_base(intr);
   unsigned component = nir_intrinsic_component(intr);
   unsigned chans = pco_ref_get_chans(src);

   const nir_src offset = intr->src[1];
   assert(nir_src_as_uint(offset) == 0);

   /* TODO NEXT: Wrong! Do properly! */
   pco_ref vtxout_addr = pco_ref_val8((4 * base) + component);

   return pco_uvsw_write(&tctx->b, src, vtxout_addr, .rpt = chans);
}

/**
 * \brief Translates a NIR fs load_input intrinsic into PCO.
 *
 * \param[in,out] tctx Translation context.
 * \param[in] intr load_input intrinsic.
 * \param[in] dest Instruction destination.
 * \return The translated PCO instruction.
 */
static pco_instr *
trans_load_input_fs(trans_ctx *tctx, nir_intrinsic_instr *intr, pco_ref dest)
{
   puts("finishme: trans_load_input_fs");

   UNUSED unsigned base = nir_intrinsic_base(intr);
   UNUSED unsigned component = nir_intrinsic_component(intr);
   unsigned chans = pco_ref_get_chans(dest);

   const nir_src offset = intr->src[0];
   assert(nir_src_as_uint(offset) == 0);

   /* TODO NEXT: Wrong! Do properly! */
   unsigned coeffs_index =
      4 * ((nir_intrinsic_io_semantics(intr).location - VARYING_SLOT_VAR0) + 1);
   pco_ref coeffs = pco_ref_hwreg_vec(coeffs_index, PCO_REG_CLASS_COEFF, 4);
   pco_ref wcoeffs = pco_ref_hwreg_vec(0, PCO_REG_CLASS_COEFF, 4);
   pco_ref itr_count = pco_ref_val16(chans);

   return pco_fitrp(&tctx->b,
                    dest,
                    pco_ref_drc(PCO_DRC_0),
                    coeffs,
                    wcoeffs,
                    itr_count,
                    .itr_mode = PCO_ITR_MODE_PIXEL);
}

/**
 * \brief Translates a NIR fs store_output intrinsic into PCO.
 *
 * \param[in,out] tctx Translation context.
 * \param[in] intr store_output intrinsic.
 * \param[in] src Instruction source.
 * \return The translated PCO instruction.
 */
static pco_instr *
trans_store_output_fs(trans_ctx *tctx, nir_intrinsic_instr *intr, pco_ref src)
{
   assert(pco_ref_is_scalar(src));
   puts("finishme: trans_store_output_fs");

   bool is_reg_store = nir_src_is_const(intr->src[1]);
   unsigned base = nir_intrinsic_base(intr);

   if (is_reg_store) {
      /* TODO NEXT: Wrong! Do properly! */
      pco_ref dest = pco_ref_hwreg(base, PCO_REG_CLASS_PIXOUT);
      /* TODO NEXT: optimize this to be propagated (backwards?) */
      /* return pco_mbyp0(&tctx->b, dest, src, .olchk = true); */
      return pco_mov(&tctx->b, dest, src, .olchk = true);
   }

   unreachable();
}

/**
 * \brief Translates a NIR intrinsic instruction into PCO.
 *
 * \param[in,out] tctx Translation context.
 * \param[in] intr The nir intrinsic instruction.
 * \return The PCO instruction.
 */
static pco_instr *trans_intr(trans_ctx *tctx, nir_intrinsic_instr *intr)
{
   const nir_intrinsic_info *info = &nir_intrinsic_infos[intr->intrinsic];

   pco_ref dest = info->has_dest ? pco_ref_nir_def_t(&intr->def, tctx)
                                 : pco_ref_null();

   pco_ref src[NIR_MAX_VEC_COMPONENTS] = { 0 };
   for (unsigned s = 0; s < info->num_srcs; ++s)
      src[s] = pco_ref_nir_src_t(&intr->src[s], tctx);

   switch (intr->intrinsic) {
   case nir_intrinsic_load_input:
      if (tctx->stage == MESA_SHADER_VERTEX)
         return trans_load_input_vs(tctx, intr, dest);
      else if (tctx->stage == MESA_SHADER_FRAGMENT)
         return trans_load_input_fs(tctx, intr, dest);
      break;

   case nir_intrinsic_store_output:
      if (tctx->stage == MESA_SHADER_VERTEX)
         return trans_store_output_vs(tctx, intr, src[0]);
      else if (tctx->stage == MESA_SHADER_FRAGMENT)
         return trans_store_output_fs(tctx, intr, src[0]);
      break;

   default:
      break;
   }

   printf("Unsupported intrinsic: \"");
   nir_print_instr(&intr->instr, stdout);
   printf("\"\n");
   unreachable();
}

/**
 * \brief Translates a NIR alu instruction into PCO.
 *
 * \param[in] tctx Translation context.
 * \param[in] alu The nir alu instruction.
 * \return The PCO instruction.
 */
static pco_instr *trans_alu(trans_ctx *tctx, nir_alu_instr *alu)
{
   const nir_op_info *info = &nir_op_infos[alu->op];
   unsigned num_srcs = info->num_inputs;

   pco_ref dest = pco_ref_nir_def_t(&alu->def, tctx);
   UNUSED unsigned chans = pco_ref_get_chans(dest);

   pco_ref src[NIR_MAX_VEC_COMPONENTS] = { 0 };
   for (unsigned s = 0; s < num_srcs; ++s)
      src[s] = pco_ref_nir_alu_src_t(alu, s, tctx);

   switch (alu->op) {
   case nir_op_fneg:
      return pco_mov(&tctx->b, dest, pco_ref_neg(src[0]));

   case nir_op_fadd:
      return pco_fadd(&tctx->b, dest, src[0], src[1]);

   case nir_op_fmul:
      return pco_fmul(&tctx->b, dest, src[0], src[1]);

   case nir_op_ffma:
      return pco_fmad(&tctx->b, dest, src[0], src[1], src[2]);

   case nir_op_pack_unorm_4x8:
      return pco_pck(&tctx->b,
                     dest,
                     src[0],
                     .rpt = 4,
                     .pck_fmt = PCO_PCK_FMT_U8888,
                     .scale = true);

   case nir_op_vec2:
   case nir_op_vec3:
   case nir_op_vec4:
   case nir_op_vec5:
   case nir_op_vec8:
   case nir_op_vec16:
      return pco_vec(&tctx->b, dest, num_srcs, src);

   default:
      break;
   }

   printf("Unsupported alu instruction: \"");
   nir_print_instr(&alu->instr, stdout);
   printf("\"\n");
   unreachable();
}

/**
 * \brief Translates a NIR load constant instruction into PCO.
 *
 * \param[in] tctx Translation context.
 * \param[in] nconst The nir load constant instruction.
 * \return The PCO instruction.
 */
static pco_instr *trans_const(trans_ctx *tctx, nir_load_const_instr *nconst)
{
   unsigned num_bits = nconst->def.bit_size;
   unsigned comps = nconst->def.num_components;

   /* TODO: support more bit sizes/components. */
   assert(num_bits == 32);
   assert(comps == 1);
   uint64_t val = nir_const_value_as_uint(nconst->value[0], num_bits);

   pco_ref dest = pco_ref_nir_def_t(&nconst->def, tctx);
   pco_ref imm = pco_ref_imm(val, pco_bits(num_bits), pco_ref_get_dtype(dest));

   return pco_movi32(&tctx->b, dest, imm);
}

/**
 * \brief Translates a NIR instruction into PCO.
 *
 * \param[in] tctx Translation context.
 * \param[in] ninstr The nir instruction.
 * \return The PCO instruction.
 */
static pco_instr *trans_instr(trans_ctx *tctx, nir_instr *ninstr)
{
   switch (ninstr->type) {
   case nir_instr_type_intrinsic:
      return trans_intr(tctx, nir_instr_as_intrinsic(ninstr));

   case nir_instr_type_load_const:
      return trans_const(tctx, nir_instr_as_load_const(ninstr));

   case nir_instr_type_alu:
      return trans_alu(tctx, nir_instr_as_alu(ninstr));

   default:
      break;
   }

   unreachable();
}

/**
 * \brief Translates a NIR block into PCO.
 *
 * \param[in] tctx Translation context.
 * \param[in] nblock The nir block.
 * \return The PCO block.
 */
static pco_block *trans_block(trans_ctx *tctx, nir_block *nblock)
{
   pco_block *block = pco_block_create(tctx->func);
   tctx->b = pco_builder_create(tctx->func, pco_cursor_after_block(block));

   nir_foreach_instr (ninstr, nblock) {
      trans_instr(tctx, ninstr);
   }

   return block;
}

/**
 * \brief Translates a NIR if into PCO.
 *
 * \param[in] tctx Translation context.
 * \param[in] nif The nir if.
 * \return The PCO if.
 */
static pco_if *trans_if(trans_ctx *tctx, nir_if *nif)
{
   pco_if *pif = pco_if_create(tctx->func);

   unreachable("finishme: trans_if");

   return pif;
}

/**
 * \brief Translates a NIR loop into PCO.
 *
 * \param[in] tctx Translation context.
 * \param[in] nloop The nir loop.
 * \return The PCO loop.
 */
static pco_loop *trans_loop(trans_ctx *tctx, nir_loop *nloop)
{
   pco_loop *loop = pco_loop_create(tctx->func);

   unreachable("finishme: trans_loop");

   return loop;
}

/**
 * \brief Translates a NIR function into PCO.
 *
 * \param[in] tctx Translation context.
 * \param[in] impl The nir function impl.
 * \return The PCO function.
 */
static pco_func *trans_func(trans_ctx *tctx, nir_function_impl *impl)
{
   nir_function *nfunc = impl->function;
   enum pco_func_type func_type = PCO_FUNC_TYPE_CALLABLE;

   if (nfunc->is_preamble)
      func_type = PCO_FUNC_TYPE_PREAMBLE;
   else if (nfunc->is_entrypoint)
      func_type = PCO_FUNC_TYPE_ENTRYPOINT;

   pco_func *func = pco_func_create(tctx->shader, func_type, nfunc->num_params);
   tctx->func = func;

   func->name = ralloc_strdup(func, nfunc->name);
   func->next_ssa = impl->ssa_alloc;

   /* TODO: Function parameter support. */
   assert(func->num_params == 0 && func->params == NULL);

   /* Gather types. */
   tctx->float_types =
      rzalloc_array(NULL, BITSET_WORD, BITSET_WORDS(impl->ssa_alloc));
   tctx->int_types =
      rzalloc_array(NULL, BITSET_WORD, BITSET_WORDS(impl->ssa_alloc));
   nir_gather_types(impl, tctx->float_types, tctx->int_types);

   trans_cf_nodes(tctx, &func->cf_node, &func->body, &impl->body);

   ralloc_free(tctx->float_types);
   ralloc_free(tctx->int_types);

   return func;
}

/**
 * \brief Translates NIR control flow nodes into PCO.
 *
 * \param[in] tctx Translation context.
 * \param[in] parent_cf_node The parent cf node.
 * \param[in] cf_node_list The PCO cf node list.
 * \param[in,out] nir_cf_node_list The NIR cf node list.
 * \return The first block from the cf nodes.
 */
static pco_block *trans_cf_nodes(trans_ctx *tctx,
                                 pco_cf_node *parent_cf_node,
                                 struct list_head *cf_node_list,
                                 struct exec_list *nir_cf_node_list)
{
   pco_block *start_block = NULL;

   pco_cf_node *cf_node;
   foreach_list_typed (nir_cf_node, ncf_node, node, nir_cf_node_list) {
      switch (ncf_node->type) {
      case nir_cf_node_block: {
         pco_block *block = trans_block(tctx, nir_cf_node_as_block(ncf_node));
         cf_node = &block->cf_node;

         if (!start_block)
            start_block = block;
         break;
      }

      case nir_cf_node_if: {
         pco_if *pif = trans_if(tctx, nir_cf_node_as_if(ncf_node));
         cf_node = &pif->cf_node;
         break;
      }

      case nir_cf_node_loop: {
         pco_loop *loop = trans_loop(tctx, nir_cf_node_as_loop(ncf_node));
         cf_node = &loop->cf_node;
         break;
      }

      default:
         unreachable();
      }

      cf_node->parent = parent_cf_node;
      list_addtail(&cf_node->link, cf_node_list);
   }

   return start_block;
}

/**
 * \brief Translates a NIR shader into a PCO shader.
 *
 * \param[in] ctx PCO compiler context.
 * \param[in] nir NIR shader.
 * \param[in] mem_ctx Ralloc memory allocation context.
 * \return The PCO shader.
 */
pco_shader *pco_trans_nir(pco_ctx *ctx, nir_shader *nir, void *mem_ctx)
{
   pco_shader *shader = pco_shader_create(ctx, nir, mem_ctx);
   trans_ctx tctx = {
      .pco_ctx = ctx,
      .shader = shader,
      .stage = shader->stage,
   };

   nir_foreach_function_with_impl (func, impl, nir) {
      trans_func(&tctx, impl);
   }

   if (pco_should_print_shader(shader))
      pco_print_shader(shader, stdout, "before passes");

   return shader;
}
