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

   BITSET_WORD *float_types; /** NIR SSA float vars. */
   BITSET_WORD *int_types; /** NIR SSA int vars. */
} trans_ctx;

/* Forward declarations. */
static pco_block *trans_cf_nodes(trans_ctx *tctx,
                                 pco_cf_node *parent_cf_node,
                                 struct list_head *cf_node_list,
                                 struct exec_list *nir_cf_node_list);

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
   };

   nir_foreach_function_with_impl (func, impl, nir) {
      trans_func(&tctx, impl);
   }

   if (pco_should_print_shader(shader))
      pco_print_shader(shader, stdout, "before passes");

   return shader;
}
