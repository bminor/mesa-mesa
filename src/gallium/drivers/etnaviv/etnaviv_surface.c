/*
 * Copyright (c) 2012-2013 Etnaviv Project
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sub license,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the
 * next paragraph) shall be included in all copies or substantial portions
 * of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NON-INFRINGEMENT. IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 *
 * Authors:
 *    Wladimir J. van der Laan <laanwj@gmail.com>
 */

#include "etnaviv_surface.h"
#include "etnaviv_screen.h"

#include "etnaviv_clear_blit.h"
#include "etnaviv_context.h"
#include "etnaviv_translate.h"
#include "pipe/p_defines.h"
#include "pipe/p_state.h"
#include "util/u_inlines.h"
#include "util/u_math.h"
#include "util/u_memory.h"

#include "drm-uapi/drm_fourcc.h"

static struct etna_resource *
etna_render_handle_incompatible(struct pipe_context *pctx,
                                struct pipe_resource *prsc)
{
   struct etna_context *ctx = etna_context(pctx);
   struct etna_screen *screen = ctx->screen;
   struct etna_resource *res = etna_resource(prsc);
   bool need_multitiled = screen->specs.pe_multitiled;
   bool want_supertiled = screen->specs.can_supertile;

   if (etna_resource_is_render_compatible(pctx->screen, res))
      return res;

   if (!res->render) {
      struct pipe_resource templat = *prsc;
      unsigned layout = ETNA_LAYOUT_TILED;
      if (need_multitiled)
         layout |= ETNA_LAYOUT_BIT_MULTI;
      if (want_supertiled)
         layout |= ETNA_LAYOUT_BIT_SUPER;

      templat.bind &= (PIPE_BIND_DEPTH_STENCIL | PIPE_BIND_RENDER_TARGET |
                        PIPE_BIND_BLENDABLE);
      res->render =
         etna_resource_alloc(pctx->screen, layout,
                             DRM_FORMAT_MOD_LINEAR, &templat);
      assert(res->render);
   }
   return etna_resource(res->render);
}

static struct pipe_surface *
etna_create_surface(struct pipe_context *pctx, struct pipe_resource *prsc,
                    const struct pipe_surface *templat)
{
   struct etna_context *ctx = etna_context(pctx);
   struct etna_screen *screen = ctx->screen;
   unsigned layer = templat->first_layer;
   unsigned level = templat->level;
   struct etna_resource *rsc = etna_render_handle_incompatible(pctx, prsc);
   struct etna_resource_level *lev = &rsc->levels[level];
   struct etna_surface *surf = CALLOC_STRUCT(etna_surface);

   if (!surf)
      return NULL;

   assert(templat->first_layer == templat->last_layer);
   assert(layer <= util_max_layer(prsc, level));

   surf->base.context = pctx;

   pipe_reference_init(&surf->base.reference, 1);
   pipe_resource_reference(&surf->base.texture, &rsc->base);
   pipe_resource_reference(&surf->prsc, prsc);

   surf->base.format = templat->format;
   surf->base.first_layer = templat->first_layer;
   surf->base.last_layer = templat->last_layer;
   surf->level = lev;

   return &surf->base;
}

static void
etna_surface_destroy(struct pipe_context *pctx, struct pipe_surface *psurf)
{
   pipe_resource_reference(&psurf->texture, NULL);
   pipe_resource_reference(&etna_surface(psurf)->prsc, NULL);
   FREE(psurf);
}

void
etna_surface_init(struct pipe_context *pctx)
{
   pctx->create_surface = etna_create_surface;
   pctx->surface_destroy = etna_surface_destroy;
}
