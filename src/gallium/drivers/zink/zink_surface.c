/*
 * Copyright 2018 Collabora Ltd.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * on the rights to use, copy, modify, merge, publish, distribute, sub
 * license, and/or sell copies of the Software, and to permit persons to whom
 * the Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the next
 * paragraph) shall be included in all copies or substantial portions of the
 * Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NON-INFRINGEMENT. IN NO EVENT SHALL
 * THE AUTHOR(S) AND/OR THEIR SUPPLIERS BE LIABLE FOR ANY CLAIM,
 * DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR
 * OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE
 * USE OR OTHER DEALINGS IN THE SOFTWARE.
 */

#include "zink_context.h"
#include "zink_format.h"
#include "zink_resource.h"
#include "zink_screen.h"
#include "zink_surface.h"
#include "zink_kopper.h"

#include "util/format/u_format.h"
#include "util/u_inlines.h"
#include "util/u_memory.h"

VkImageViewCreateInfo
create_ivci(struct zink_screen *screen,
            struct zink_resource *res,
            const struct pipe_surface *templ,
            enum pipe_texture_target target)
{
   VkImageViewCreateInfo ivci;
   /* zero holes since this is hashed */
   memset(&ivci, 0, sizeof(VkImageViewCreateInfo));
   ivci.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
   ivci.image = res->obj->image;

   switch (target) {
   case PIPE_TEXTURE_1D:
      ivci.viewType = res->need_2D ? VK_IMAGE_VIEW_TYPE_2D : VK_IMAGE_VIEW_TYPE_1D;
      break;

   case PIPE_TEXTURE_1D_ARRAY:
      ivci.viewType = res->need_2D ? VK_IMAGE_VIEW_TYPE_2D_ARRAY : VK_IMAGE_VIEW_TYPE_1D_ARRAY;
      break;

   case PIPE_TEXTURE_2D:
   case PIPE_TEXTURE_RECT:
      ivci.viewType = VK_IMAGE_VIEW_TYPE_2D;
      break;

   case PIPE_TEXTURE_2D_ARRAY:
      ivci.viewType = VK_IMAGE_VIEW_TYPE_2D_ARRAY;
      break;

   case PIPE_TEXTURE_CUBE:
      ivci.viewType = VK_IMAGE_VIEW_TYPE_CUBE;
      break;

   case PIPE_TEXTURE_CUBE_ARRAY:
      ivci.viewType = VK_IMAGE_VIEW_TYPE_CUBE_ARRAY;
      break;

   case PIPE_TEXTURE_3D:
      ivci.viewType = VK_IMAGE_VIEW_TYPE_3D;
      break;

   default:
      unreachable("unsupported target");
   }

   ivci.format = res->base.b.format == PIPE_FORMAT_A8_UNORM ? res->format : zink_get_format(screen, templ->format);
   assert(ivci.format != VK_FORMAT_UNDEFINED);

   /* TODO: it's currently illegal to use non-identity swizzles for framebuffer attachments,
    * but if that ever changes, this will be useful
   const struct util_format_description *desc = util_format_description(templ->format);
   ivci.components.r = zink_component_mapping(zink_clamp_void_swizzle(desc, PIPE_SWIZZLE_X));
   ivci.components.g = zink_component_mapping(zink_clamp_void_swizzle(desc, PIPE_SWIZZLE_Y));
   ivci.components.b = zink_component_mapping(zink_clamp_void_swizzle(desc, PIPE_SWIZZLE_Z));
   ivci.components.a = zink_component_mapping(zink_clamp_void_swizzle(desc, PIPE_SWIZZLE_W));
   */
   ivci.components.r = VK_COMPONENT_SWIZZLE_R;
   ivci.components.g = VK_COMPONENT_SWIZZLE_G;
   ivci.components.b = VK_COMPONENT_SWIZZLE_B;
   ivci.components.a = VK_COMPONENT_SWIZZLE_A;

   ivci.subresourceRange.aspectMask = res->aspect;
   ivci.subresourceRange.baseMipLevel = templ->level;
   ivci.subresourceRange.levelCount = 1;
   ivci.subresourceRange.baseArrayLayer = templ->first_layer;
   ivci.subresourceRange.layerCount = 1 + templ->last_layer - templ->first_layer;
   assert(ivci.viewType != VK_IMAGE_VIEW_TYPE_3D || ivci.subresourceRange.baseArrayLayer == 0);
   assert(ivci.viewType != VK_IMAGE_VIEW_TYPE_3D || ivci.subresourceRange.layerCount == 1);
   /* ensure cube image types get clamped to 2D/2D_ARRAY as expected for partial views */
   ivci.viewType = zink_surface_clamp_viewtype(ivci.viewType, templ->first_layer, templ->last_layer, res->base.b.array_size);

   return ivci;
}

static void
init_pipe_surface_info(struct pipe_context *pctx, struct pipe_surface *psurf, const struct pipe_surface *templ, const struct pipe_resource *pres)
{
   unsigned int level = templ->level;
   psurf->reference.count = 10000000;
   psurf->context = pctx;
   psurf->format = templ->format;
   psurf->nr_samples = templ->nr_samples;
   psurf->level = level;
   psurf->first_layer = templ->first_layer;
   psurf->last_layer = templ->last_layer;
}

static void
apply_view_usage_for_format(struct zink_screen *screen, struct zink_surface *surface, enum pipe_format format, VkImageViewCreateInfo *ivci, VkImageViewUsageCreateInfo *usage_info)
{
   struct zink_resource *res = zink_resource(surface->base.texture);
   VkFormatFeatureFlags feats = res->linear ?
                                zink_get_format_props(screen, format)->linearTilingFeatures :
                                zink_get_format_props(screen, format)->optimalTilingFeatures;
   VkImageUsageFlags attachment = (VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_INPUT_ATTACHMENT_BIT);
   usage_info->usage = res->obj->vkusage & ~attachment;
   if (res->obj->modifier_aspect) {
      feats = res->obj->vkfeats;
      /* intersect format features for current modifier */
      for (unsigned i = 0; i < screen->modifier_props[format].drmFormatModifierCount; i++) {
         if (res->obj->modifier == screen->modifier_props[format].pDrmFormatModifierProperties[i].drmFormatModifier)
            feats &= screen->modifier_props[format].pDrmFormatModifierProperties[i].drmFormatModifierTilingFeatures;
      }
   }
   /* if the format features don't support framebuffer attachment, use VkImageViewUsageCreateInfo to remove it */
   if ((res->obj->vkusage & attachment) &&
       !(feats & (VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT | VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT))) {
      ivci->pNext = usage_info;
   } else {
      ivci->pNext = NULL;
   }
}

static struct zink_surface *
create_surface(struct pipe_context *pctx,
               struct pipe_resource *pres,
               const struct pipe_surface *templ,
               VkImageViewCreateInfo *ivci,
               bool actually)
{
   struct zink_screen *screen = zink_screen(pctx->screen);

   struct zink_surface *surface = CALLOC_STRUCT(zink_surface);
   if (!surface)
      return NULL;

   surface->base.texture = pres;
   pipe_reference_init(&surface->base.reference, 1);
   init_pipe_surface_info(pctx, &surface->base, templ, pres);

   if (!actually)
      return surface;
   assert(ivci->image);
   VkImageViewUsageCreateInfo usage_info = { VK_STRUCTURE_TYPE_IMAGE_VIEW_USAGE_CREATE_INFO };
   apply_view_usage_for_format(screen, surface, templ->format, ivci, &usage_info);
   VkResult result = VKSCR(CreateImageView)(screen->dev, ivci, NULL,
                                            &surface->image_view);
   if (result != VK_SUCCESS) {
      mesa_loge("ZINK: vkCreateImageView failed (%s)", vk_Result_to_str(result));
      FREE(surface);
      return NULL;
   }

   return surface;
}

static uint32_t
hash_ivci(const void *key)
{
   return _mesa_hash_data((char*)key + offsetof(VkImageViewCreateInfo, flags), sizeof(VkImageViewCreateInfo) - offsetof(VkImageViewCreateInfo, flags));
}

static struct hash_table *
get_surface_cache(struct zink_resource *res)
{
   struct kopper_displaytarget *cdt = res->obj->dt;
   assert(!res->obj->dt || res->obj->dt_idx != UINT32_MAX);
   return res->obj->dt ? &cdt->swapchain->images[res->obj->dt_idx].surface_cache : &res->obj->surface_cache;
}

/* get a cached surface for a shader descriptor */
struct zink_surface *
zink_get_surface(struct zink_context *ctx,
            struct pipe_resource *pres,
            const struct pipe_surface *templ,
            VkImageViewCreateInfo *ivci)
{
   struct zink_surface *surface = NULL;
   struct zink_resource *res = zink_resource(pres);

   /* not acquired */
   if (res->obj->dt && res->obj->dt_idx == UINT32_MAX)
      return NULL;
   if (!res->obj->dt && zink_format_needs_mutable(pres->format, templ->format))
      /* mutable not set by default */
      zink_resource_object_init_mutable(ctx, res);
   /* reset for mutable obj switch */
   ivci->image = res->obj->image;
   uint32_t hash = hash_ivci(ivci);

   simple_mtx_lock(&res->obj->surface_mtx);
   struct hash_table *ht = get_surface_cache(res);
   struct hash_entry *entry = _mesa_hash_table_search_pre_hashed(ht, hash, ivci);

   if (!entry) {
      surface = create_surface(&ctx->base, pres, templ, ivci, true);
      /* only transient surfaces have nr_samples set */
      surface->base.nr_samples = zink_screen(ctx->base.screen)->info.have_EXT_multisampled_render_to_single_sampled ? templ->nr_samples : 0;
      surface->ivci = *ivci;
      entry = _mesa_hash_table_insert_pre_hashed(ht, hash, mem_dup(ivci, sizeof(*ivci)), surface);
      if (!entry) {
         simple_mtx_unlock(&res->obj->surface_mtx);
         return NULL;
      }

      surface = entry->data;
   } else {
      surface = entry->data;
   }
   simple_mtx_unlock(&res->obj->surface_mtx);

   return surface;
}

/* this is the context hook, so only zink_ctx_surfaces will reach it */
static void
zink_surface_destroy(struct pipe_context *pctx,
                     struct pipe_surface *psurface)
{
   /* ensure this gets repopulated if another transient surface is created */
   struct zink_resource *res = zink_resource(psurface->texture);
   if (res->transient)
      res->transient->valid = false;
}

static VkImageViewCreateInfo
create_fb_ivci(struct zink_screen *screen, struct zink_resource *res, const struct pipe_surface *templ)
{
   bool is_array = templ->last_layer != templ->first_layer;
   enum pipe_texture_target target_2d[] = {PIPE_TEXTURE_2D, PIPE_TEXTURE_2D_ARRAY};

   return create_ivci(screen, res, templ, res->base.b.target == PIPE_TEXTURE_3D ? target_2d[is_array] : res->base.b.target);
}

struct pipe_surface *
zink_create_fb_surface(struct pipe_context *pctx,
                       struct pipe_resource *pres,
                       const struct pipe_surface *templ)
{
   struct zink_resource *res = zink_resource(pres);

   VkImageViewCreateInfo ivci = create_fb_ivci(zink_screen(pctx->screen), res, templ);
   return (struct pipe_surface*)zink_get_surface(zink_context(pctx), pres, templ, &ivci);
}

static struct pipe_surface *
zink_create_surface(struct pipe_context *pctx,
                    struct pipe_resource *pres,
                    const struct pipe_surface *templ)
{
   return zink_create_fb_surface(pctx, pres, templ);
}

struct zink_surface *
zink_create_transient_surface(struct zink_context *ctx, const struct pipe_surface *psurf, unsigned nr_samples)
{
   struct zink_resource *res = zink_resource(psurf->texture);
   struct zink_resource *transient = res->transient;
   assert(nr_samples > 1);
   if (!res->transient) {
      /* transient fb attachment: not cached */
      struct pipe_resource rtempl = *psurf->texture;
      rtempl.nr_samples = nr_samples;
      rtempl.bind |= ZINK_BIND_TRANSIENT;
      res->transient = zink_resource(ctx->base.screen->resource_create(ctx->base.screen, &rtempl));
      transient = res->transient;
      if (unlikely(!transient)) {
         mesa_loge("ZINK: failed to create transient resource!");
         return NULL;
      }
   }

   VkImageViewCreateInfo ivci = create_fb_ivci(zink_screen(ctx->base.screen), res, psurf);
   ivci.image = transient->obj->image;
   ivci.pNext = NULL;
   return zink_get_surface(ctx, &transient->base.b, psurf, &ivci);
}

void
zink_context_surface_init(struct pipe_context *context)
{
   context->create_surface = zink_create_surface;
   context->surface_destroy = zink_surface_destroy;
}

/* must be called before a swapchain image is used to ensure correct imageview is used */
struct zink_surface *
zink_surface_swapchain_update(struct zink_context *ctx, struct pipe_surface *psurf)
{
   struct zink_screen *screen = zink_screen(ctx->base.screen);
   struct zink_resource *res = zink_resource(psurf->texture);
   struct kopper_displaytarget *cdt = res->obj->dt;
   if (!cdt)
      return NULL; //dead swapchain

   VkImageViewCreateInfo ivci = create_ivci(screen, res, psurf, psurf->first_layer != psurf->last_layer ? PIPE_TEXTURE_2D_ARRAY : PIPE_TEXTURE_2D);
   return zink_get_surface(ctx, &res->base.b, psurf, &ivci);
}
