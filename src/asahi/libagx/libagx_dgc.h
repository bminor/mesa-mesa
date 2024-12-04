/*
 * Copyright 2024 Valve Corporation
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include "compiler/libcl/libcl.h"
#include "agx_pack.h"

#define agx_push(ptr, T, cfg)                                                  \
   for (unsigned _loop = 0; _loop < 1;                                         \
        ++_loop, ptr = (GLOBAL void *)(((uintptr_t)ptr) + AGX_##T##_LENGTH))   \
      agx_pack(ptr, T, cfg)

#define agx_push_packed(ptr, src, T)                                           \
   static_assert(sizeof(src) == AGX_##T##_LENGTH);                             \
   memcpy(ptr, &src, sizeof(src));                                             \
   ptr = (GLOBAL void *)(((uintptr_t)ptr) + sizeof(src));

struct agx_workgroup {
   uint32_t x, y, z;
};

static inline struct agx_workgroup
agx_workgroup(uint32_t x, uint32_t y, uint32_t z)
{
   return (struct agx_workgroup){.x = x, .y = y, .z = z};
}

static inline unsigned
agx_workgroup_threads(struct agx_workgroup wg)
{
   return wg.x * wg.y * wg.z;
}

struct agx_grid {
   enum agx_cdm_mode mode;
   union {
      uint32_t count[3];
      uint64_t ptr;
   };
};

static struct agx_grid
agx_3d(uint32_t x, uint32_t y, uint32_t z)
{
   return (struct agx_grid){.mode = AGX_CDM_MODE_DIRECT, .count = {x, y, z}};
}

static struct agx_grid
agx_1d(uint32_t x)
{
   return agx_3d(x, 1, 1);
}

static struct agx_grid
agx_grid_indirect(uint64_t ptr)
{
   return (struct agx_grid){.mode = AGX_CDM_MODE_INDIRECT_GLOBAL, .ptr = ptr};
}

static struct agx_grid
agx_grid_indirect_local(uint64_t ptr)
{
   return (struct agx_grid){.mode = AGX_CDM_MODE_INDIRECT_LOCAL, .ptr = ptr};
}

static inline bool
agx_is_indirect(struct agx_grid grid)
{
   return grid.mode != AGX_CDM_MODE_DIRECT;
}

enum agx_chip {
   AGX_CHIP_G13G,
   AGX_CHIP_G13X,
   AGX_CHIP_G14G,
   AGX_CHIP_G14X,
};

static inline GLOBAL uint32_t *
agx_cdm_launch(GLOBAL uint32_t *out, enum agx_chip chip, struct agx_grid grid,
               struct agx_workgroup wg,
               struct agx_cdm_launch_word_0_packed launch, uint32_t usc)
{
#ifndef __OPENCL_VERSION__
   struct agx_cdm_launch_word_0_packed mode;
   agx_pack(&mode, CDM_LAUNCH_WORD_0, cfg) {
      cfg.mode = grid.mode;
   }

   agx_merge(launch, mode, CDM_LAUNCH_WORD_0);
#endif

   agx_push_packed(out, launch, CDM_LAUNCH_WORD_0);

   agx_push(out, CDM_LAUNCH_WORD_1, cfg) {
      cfg.pipeline = usc;
   }

   if (chip == AGX_CHIP_G14X) {
      agx_push(out, CDM_UNK_G14X, cfg)
         ;
   }

   if (agx_is_indirect(grid)) {
      agx_push(out, CDM_INDIRECT, cfg) {
         cfg.address_hi = grid.ptr >> 32;
         cfg.address_lo = grid.ptr;
      }
   } else {
      agx_push(out, CDM_GLOBAL_SIZE, cfg) {
         cfg.x = grid.count[0];
         cfg.y = grid.count[1];
         cfg.z = grid.count[2];
      }
   }

   if (grid.mode != AGX_CDM_MODE_INDIRECT_LOCAL) {
      agx_push(out, CDM_LOCAL_SIZE, cfg) {
         cfg.x = wg.x;
         cfg.y = wg.y;
         cfg.z = wg.z;
      }
   }

   return out;
}

static inline GLOBAL uint32_t *
agx_cdm_barrier(GLOBAL uint32_t *out, enum agx_chip chip)
{
   agx_push(out, CDM_BARRIER, cfg) {
      cfg.unk_5 = true;
      cfg.unk_6 = true;
      cfg.unk_8 = true;
      // cfg.unk_11 = true;
      // cfg.unk_20 = true;
      // cfg.unk_24 = true; if clustered?
      if (chip == AGX_CHIP_G13X) {
         cfg.unk_4 = true;
         // cfg.unk_26 = true;
      }

      /* With multiple launches in the same CDM stream, we can get cache
       * coherency (? or sync?) issues. We hit this with blits, which need - in
       * between dispatches - need the PBE cache to be flushed and the texture
       * cache to be invalidated. Until we know what bits mean what exactly,
       * let's just set these after every launch to be safe. We can revisit in
       * the future when we figure out what the bits mean.
       */
      cfg.unk_0 = true;
      cfg.unk_1 = true;
      cfg.unk_2 = true;
      cfg.usc_cache_inval = true;
      cfg.unk_4 = true;
      cfg.unk_5 = true;
      cfg.unk_6 = true;
      cfg.unk_7 = true;
      cfg.unk_8 = true;
      cfg.unk_9 = true;
      cfg.unk_10 = true;
      cfg.unk_11 = true;
      cfg.unk_12 = true;
      cfg.unk_13 = true;
      cfg.unk_14 = true;
      cfg.unk_15 = true;
      cfg.unk_16 = true;
      cfg.unk_17 = true;
      cfg.unk_18 = true;
      cfg.unk_19 = true;
   }

   return out;
}

static inline GLOBAL uint32_t *
agx_cdm_return(GLOBAL uint32_t *out)
{
   agx_push(out, CDM_STREAM_RETURN, cfg)
      ;

   return out;
}

static inline GLOBAL uint32_t *
agx_cdm_terminate(GLOBAL uint32_t *out)
{
   agx_push(out, CDM_STREAM_TERMINATE, _)
      ;

   return out;
}

static inline GLOBAL uint32_t *
agx_vdm_terminate(GLOBAL uint32_t *out)
{
   agx_push(out, VDM_STREAM_TERMINATE, _)
      ;

   return out;
}

static inline GLOBAL uint32_t *
agx_cdm_jump(GLOBAL uint32_t *out, uint64_t target)
{
   agx_push(out, CDM_STREAM_LINK, cfg) {
      cfg.target_lo = target & BITFIELD_MASK(32);
      cfg.target_hi = target >> 32;
   }

   return out;
}

static inline GLOBAL uint32_t *
agx_vdm_jump(GLOBAL uint32_t *out, uint64_t target)
{
   agx_push(out, VDM_STREAM_LINK, cfg) {
      cfg.target_lo = target & BITFIELD_MASK(32);
      cfg.target_hi = target >> 32;
   }

   return out;
}

static inline GLOBAL uint32_t *
agx_cs_jump(GLOBAL uint32_t *out, uint64_t target, bool vdm)
{
   return vdm ? agx_vdm_jump(out, target) : agx_cdm_jump(out, target);
}

static inline GLOBAL uint32_t *
agx_cdm_call(GLOBAL uint32_t *out, uint64_t target)
{
   agx_push(out, CDM_STREAM_LINK, cfg) {
      cfg.target_lo = target & BITFIELD_MASK(32);
      cfg.target_hi = target >> 32;
      cfg.with_return = true;
   }

   return out;
}

static inline GLOBAL uint32_t *
agx_vdm_call(GLOBAL uint32_t *out, uint64_t target)
{
   agx_push(out, VDM_STREAM_LINK, cfg) {
      cfg.target_lo = target & BITFIELD_MASK(32);
      cfg.target_hi = target >> 32;
      cfg.with_return = true;
   }

   return out;
}

#define AGX_MAX_LINKED_USC_SIZE                                                \
   (AGX_USC_PRESHADER_LENGTH + AGX_USC_FRAGMENT_PROPERTIES_LENGTH +            \
    AGX_USC_REGISTERS_LENGTH + AGX_USC_SHADER_LENGTH + AGX_USC_SHARED_LENGTH + \
    AGX_USC_SAMPLER_LENGTH + (AGX_USC_UNIFORM_LENGTH * 9))

/*
 * This data structure contains everything needed to dispatch a compute shader
 * (and hopefully eventually graphics?).
 *
 * It is purely flat, no CPU pointers. That makes it suitable for sharing
 * between CPU and GPU. The intention is that it is packed on the CPU side and
 * then consumed on either host or device for dispatching work.
 */
struct agx_shader {
   struct agx_cdm_launch_word_0_packed launch;
   struct agx_workgroup workgroup;

   struct {
      uint32_t size;
      uint8_t data[AGX_MAX_LINKED_USC_SIZE];
   } usc;
};

/* Opaque structure representing a USC program being constructed */
struct agx_usc_builder {
   GLOBAL uint8_t *head;

#ifndef NDEBUG
   uint8_t *begin;
   size_t size;
#endif
} PACKED;

#ifdef __OPENCL_VERSION__
static_assert(sizeof(struct agx_usc_builder) == 8);
#endif

static struct agx_usc_builder
agx_usc_builder(GLOBAL void *out, ASSERTED size_t size)
{
   return (struct agx_usc_builder){
      .head = out,

#ifndef NDEBUG
      .begin = out,
      .size = size,
#endif
   };
}

static bool
agx_usc_builder_validate(struct agx_usc_builder *b, size_t size)
{
#ifndef NDEBUG
   assert(((b->head - b->begin) + size) <= b->size);
#endif

   return true;
}

#define agx_usc_pack(b, struct_name, template)                                 \
   for (bool it =                                                              \
           agx_usc_builder_validate((b), AGX_USC_##struct_name##_LENGTH);      \
        it; it = false, (b)->head += AGX_USC_##struct_name##_LENGTH)           \
      agx_pack((b)->head, USC_##struct_name, template)

#define agx_usc_push_blob(b, blob, length)                                     \
   for (bool it = agx_usc_builder_validate((b), length); it;                   \
        it = false, (b)->head += length)                                       \
      memcpy((b)->head, blob, length);

#define agx_usc_push_packed(b, struct_name, packed)                            \
   agx_usc_push_blob(b, packed.opaque, AGX_USC_##struct_name##_LENGTH);

static void
agx_usc_uniform(struct agx_usc_builder *b, unsigned start_halfs,
                unsigned size_halfs, uint64_t buffer)
{
   assert((start_halfs + size_halfs) <= (1 << 9) && "uniform file overflow");
   assert(size_halfs <= 64 && "caller's responsibility to split");
   assert(size_halfs > 0 && "no empty uniforms");

   if (start_halfs & BITFIELD_BIT(8)) {
      agx_usc_pack(b, UNIFORM_HIGH, cfg) {
         cfg.start_halfs = start_halfs & BITFIELD_MASK(8);
         cfg.size_halfs = size_halfs;
         cfg.buffer = buffer;
      }
   } else {
      agx_usc_pack(b, UNIFORM, cfg) {
         cfg.start_halfs = start_halfs;
         cfg.size_halfs = size_halfs;
         cfg.buffer = buffer;
      }
   }
}

static inline void
agx_usc_words_precomp(GLOBAL uint32_t *out, CONST struct agx_shader *s,
                      uint64_t data, unsigned data_size)
{
   /* Map the data directly as uniforms starting at u0 */
   struct agx_usc_builder b = agx_usc_builder(out, sizeof(s->usc.data));
   agx_usc_uniform(&b, 0, DIV_ROUND_UP(data_size, 2), data);
   agx_usc_push_blob(&b, s->usc.data, s->usc.size);
}
