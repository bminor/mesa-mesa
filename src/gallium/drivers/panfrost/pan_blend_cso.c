/*
 * Copyright (C) 2018 Alyssa Rosenzweig
 * Copyright (C) 2019-2025 Collabora, Ltd.
 * SPDX-License-Identifier: MIT
 */

#include "pan_blend_cso.h"

#include "compiler/nir/nir.h"
#include "compiler/nir/nir_builder.h"
#include "pan_shader.h"
#include "panfrost/util/pan_lower_framebuffer.h"

#ifndef PAN_ARCH

DERIVE_HASH_TABLE(pan_blend_shader_key);

void
pan_blend_shader_cache_init(struct pan_blend_shader_cache *cache,
                            unsigned gpu_id)
{
   cache->gpu_id = gpu_id;
   cache->shaders = pan_blend_shader_key_table_create(NULL);
   pthread_mutex_init(&cache->lock, NULL);
}

void
pan_blend_shader_cache_cleanup(struct pan_blend_shader_cache *cache)
{
   _mesa_hash_table_destroy(cache->shaders, NULL);
   pthread_mutex_destroy(&cache->lock);
}

#else /* PAN_ARCH */

static bool
pan_inline_blend_constants(nir_builder *b, nir_intrinsic_instr *intr,
                           void *data)
{
   if (intr->intrinsic != nir_intrinsic_load_blend_const_color_rgba)
      return false;

   float *floats = data;
   const nir_const_value constants[4] = {
      nir_const_value_for_float(floats[0], 32),
      nir_const_value_for_float(floats[1], 32),
      nir_const_value_for_float(floats[2], 32),
      nir_const_value_for_float(floats[3], 32)};

   b->cursor = nir_after_instr(&intr->instr);
   nir_def *constant = nir_build_imm(b, 4, 32, constants);
   nir_def_replace(&intr->def, constant);
   return true;
}

struct pan_blend_shader_variant *
GENX(pan_blend_get_shader_locked)(struct pan_blend_shader_cache *cache,
                                  const struct pan_blend_state *state,
                                  nir_alu_type src0_type,
                                  nir_alu_type src1_type, unsigned rt)
{
   struct pan_blend_shader_key key = {
      .format = state->rts[rt].format,
      .src0_type = src0_type,
      .src1_type = src1_type,
      .rt = rt,
      .has_constants = pan_blend_constant_mask(state->rts[rt].equation) != 0,
      .logicop_enable = state->logicop_enable,
      .logicop_func = state->logicop_func,
      .nr_samples = state->rts[rt].nr_samples,
      .equation = state->rts[rt].equation,
      .alpha_to_one = state->alpha_to_one,
   };
   /* Blend shaders should only be used for blending on Bifrost onwards */
   assert(PAN_ARCH <= 5 || state->logicop_enable || state->alpha_to_one ||
          !pan_blend_is_opaque(state->rts[rt].equation));
   assert(state->rts[rt].equation.color_mask != 0);

   struct hash_entry *he =
      _mesa_hash_table_search(cache->shaders, &key);
   struct pan_blend_shader *shader = he ? he->data : NULL;

   if (!shader) {
      shader = rzalloc(cache->shaders, struct pan_blend_shader);
      shader->key = key;
      list_inithead(&shader->variants);
      _mesa_hash_table_insert(cache->shaders, &shader->key, shader);
   }

   list_for_each_entry(struct pan_blend_shader_variant, iter, &shader->variants,
                       node) {
      if (!key.has_constants ||
          !memcmp(iter->constants, state->constants, sizeof(iter->constants))) {
         return iter;
      }
   }

   struct pan_blend_shader_variant *variant = NULL;

   if (shader->nvariants < PAN_BLEND_SHADER_MAX_VARIANTS) {
      variant = rzalloc(shader, struct pan_blend_shader_variant);
      util_dynarray_init(&variant->binary, variant);
      list_add(&variant->node, &shader->variants);
      shader->nvariants++;
   } else {
      variant = list_last_entry(&shader->variants,
                                struct pan_blend_shader_variant, node);
      list_del(&variant->node);
      list_add(&variant->node, &shader->variants);
      util_dynarray_clear(&variant->binary);
   }

   memcpy(variant->constants, state->constants, sizeof(variant->constants));

   nir_shader *nir =
      GENX(pan_blend_create_shader)(state, src0_type, src1_type, rt);

   nir_shader_intrinsics_pass(nir, pan_inline_blend_constants,
                              nir_metadata_control_flow,
                              (void *)state->constants);

   /* Compile the NIR shader */
   struct panfrost_compile_inputs inputs = {
      .gpu_id = cache->gpu_id,
      .is_blend = true,
      .blend.nr_samples = key.nr_samples,
   };

   enum pipe_format rt_formats[8] = {0};
   rt_formats[rt] = key.format;

#if PAN_ARCH >= 6
   inputs.blend.bifrost_blend_desc =
      GENX(pan_blend_get_internal_desc)(key.format, key.rt, 0, false);
#endif

   struct pan_shader_info info;
   pan_shader_preprocess(nir, inputs.gpu_id);

#if PAN_ARCH >= 6
   NIR_PASS(_, nir, GENX(pan_inline_rt_conversion), rt_formats);
#else
   NIR_PASS(_, nir, pan_lower_framebuffer, rt_formats,
            pan_raw_format_mask_midgard(rt_formats), MAX2(key.nr_samples, 1),
            cache->gpu_id < 0x700);
#endif

   GENX(pan_shader_compile)(nir, &inputs, &variant->binary, &info);

   variant->work_reg_count = info.work_reg_count;

#if PAN_ARCH <= 5
   variant->first_tag = info.midgard.first_tag;
#endif

   ralloc_free(nir);

   return variant;
}

#endif /* PAN_ARCH */
