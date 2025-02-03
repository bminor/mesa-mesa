/*
 * Copyright © 2024 Imagination Technologies Ltd.
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef PCO_DATA_H
#define PCO_DATA_H

/**
 * \file pco_data.h
 *
 * \brief PCO shader-specific data/compiler-driver interface.
 */

#include "common/pvr_limits.h"
#include "compiler/shader_enums.h"
#include "nir/nir_lower_blend.h"
#include "util/format/u_format.h"

#include <stdbool.h>

#define PCO_POINT_SAMPLER 0xffff
#define PCO_IA_SAMPLER 0xfffe

/* Compiler-specific forward-declarations. */
typedef struct _pco_shader pco_shader;

/** Generic range struct. */
typedef struct _pco_range {
   unsigned start;
   unsigned count;
   unsigned stride;
} pco_range;

/** PCO vertex shader-specific data. */
typedef struct _pco_vs_data {
   /** Attributes/input mappings. */
   pco_range attribs[VERT_ATTRIB_MAX];

   enum pipe_format attrib_formats[VERT_ATTRIB_MAX];

   /** Varyings/output mappings. */
   pco_range varyings[VARYING_SLOT_MAX];

   unsigned f32_smooth; /** Number of F32 linear varyings. */
   unsigned f32_flat; /** Number of F32 flat varyings. */
   unsigned f32_npc; /** Number of F32 NPC varyings. */

   unsigned f16_smooth; /** Number of F16 linear varyings. */
   unsigned f16_flat; /** Number of F16 flat varyings. */
   unsigned f16_npc; /** Number of F16 NPC varyings. */

   unsigned vtxouts; /** How many vertex outputs are written to. */
} pco_vs_data;

/** PCO fragment shader-specific data. */
typedef struct _pco_fs_data {
   /** Varyings/input mappings. */
   pco_range varyings[VARYING_SLOT_MAX];

   /** Results/output mappings. */
   pco_range outputs[FRAG_RESULT_MAX];

   /** If outputs are to be placed in pixout regs. */
   bool output_reg[FRAG_RESULT_MAX];

   /** Fragment output formats. */
   enum pipe_format output_formats[FRAG_RESULT_MAX];

   /** On-chip input attachment mappings. */
   pco_range ias_onchip[4];

   /** On-chip input attachment formats. */
   enum pipe_format ia_formats[4];

   /* Blend options. */
   nir_lower_blend_options blend_opts;
   pco_range blend_consts;
   uint8_t blend_consts_needed;

   struct {
      bool w; /** Whether the shader uses pos.w. */
      bool z; /** Whether the shader uses pos.z */
      bool pntc; /** Whether the shader uses point coord. */
      bool phase_change; /** Whether the shader does a phase change. */
      bool fbfetch; /** Whether the shader fetches from the framebuffer. */
      bool depth_feedback;
      bool discard;
      bool early_frag;
   } uses;
} pco_fs_data;

/** PCO compute shader-specific data. */
typedef struct _pco_cs_data {
   unsigned workgroup_size[3]; /** Workgroup size. */

   pco_range shmem;
   bool zero_shmem;
} pco_cs_data;

/** PCO image descriptor metadata. */
enum pco_image_meta {
   PCO_IMAGE_META_LAYER_SIZE,
   PCO_IMAGE_META_RSVD0,
   PCO_IMAGE_META_RSVD1,
   PCO_IMAGE_META_RSVD2,

   PCO_IMAGE_META_COUNT,
};

/** PCO sampler descriptor metadata. */
enum pco_sampler_meta {
   PCO_SAMPLER_META_COMPARE_OP,
   PCO_SAMPLER_META_RSVD0,
   PCO_SAMPLER_META_RSVD1,
   PCO_SAMPLER_META_RSVD2,

   PCO_SAMPLER_META_COUNT,
};

/** PCO descriptor binding data. */
typedef struct _pco_binding_data {
   pco_range range; /** Descriptor location range. */
   bool used; /** Whether the descriptor binding is used by the shader. */

   /** Whether the descriptor binding is a combined image sampler. */
   bool is_img_smp;
} pco_binding_data;

/** PCO descriptor set data. */
typedef struct _pco_descriptor_set_data {
   pco_range range; /** Descriptor location range. */

   unsigned binding_count; /** Number of bindings. */
   pco_binding_data *bindings; /** Descriptor set bindings. */

   bool used; /** Whether the descriptor set is used by the shader. */
} pco_descriptor_set_data;

/** PCO push constant data. */
typedef struct _pco_push_const_data {
   pco_range range; /** Push constant range. */

   unsigned used;
} pco_push_const_data;

/** PCO common data. */
typedef struct _pco_common_data {
   /** System value mappings. */
   pco_range sys_vals[SYSTEM_VALUE_MAX];

   /** Descriptor set data. */
   pco_descriptor_set_data desc_sets[PVR_MAX_DESCRIPTOR_SETS];

   /** Push constant data. */
   pco_push_const_data push_consts;

   pco_range point_sampler;
   pco_range ia_sampler;

   unsigned temps; /** Number of allocated temp registers. */
   unsigned vtxins; /** Number of allocated vertex input registers. */
   unsigned interns; /** Number of allocated internal registers. */

   unsigned coeffs; /** Number of allocated coefficient registers. */
   unsigned shareds; /** Number of allocated shared registers. */

   unsigned entry_offset; /** Offset of the shader entrypoint. */

   struct {
      bool atomics; /** Whether the shader uses atomics. */
      bool barriers; /** Whether the shader uses barriers. */
      bool side_effects; /** Whether the shader has side effects. */
      bool empty; /** Whether the shader is empty. */
      bool point_sampler; /** Whether the shader uses a point sampler. */
      bool ia_sampler; /** Does the shader use an input attachment sampler? */
   } uses;
} pco_common_data;

/** PCO shader data. */
typedef struct _pco_data {
   union {
      pco_vs_data vs;
      pco_fs_data fs;
      pco_cs_data cs;
   };

   pco_common_data common;
} pco_data;

/** PCO precompiled shader data. */
typedef struct PACKED _pco_precomp_data {
   uint8_t temps;
   uint8_t vtxins;

   uint16_t coeffs : 12;
   uint16_t shareds : 12;

   uint8_t pad[3];
} pco_precomp_data;
static_assert(sizeof(pco_precomp_data) == 8, "sizeof(pco_precomp_data) != 8");

pco_precomp_data pco_get_precomp_data(pco_shader *shader);
#endif /* PCO_DATA_H */
