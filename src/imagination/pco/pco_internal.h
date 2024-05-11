/*
 * Copyright © 2024 Imagination Technologies Ltd.
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef PCO_INTERNAL_H
#define PCO_INTERNAL_H

/**
 * \file pco_internal.h
 *
 * \brief PCO internal header.
 */

#include "compiler/spirv/nir_spirv.h"
#include "pco.h"
#include "pco_common.h"
#include "pco_ops.h"
#include "spirv/nir_spirv.h"
#include "util/macros.h"

#include <stdbool.h>
#include <stdint.h>

/** PCO compiler context. */
typedef struct _pco_ctx {
   /** Device information. */
   const struct pvr_device_info *dev_info;

   /** Device-specific NIR options. */
   nir_shader_compiler_options nir_options;

   /** Device-specific SPIR-V to NIR options. */
   struct spirv_to_nir_options spirv_options;
} pco_ctx;

void pco_setup_spirv_options(const struct pvr_device_info *dev_info,
                             struct spirv_to_nir_options *spirv_options);
void pco_setup_nir_options(const struct pvr_device_info *dev_info,
                           nir_shader_compiler_options *nir_options);

/* Debug. */
enum pco_debug {
   PCO_DEBUG_VAL_SKIP = BITFIELD64_BIT(0),
};

extern uint64_t pco_debug;

#define PCO_DEBUG(flag) unlikely(pco_debug &(PCO_DEBUG_##flag))

enum pco_debug_print {
   PCO_DEBUG_PRINT_VS = BITFIELD64_BIT(0),
   PCO_DEBUG_PRINT_FS = BITFIELD64_BIT(1),
   PCO_DEBUG_PRINT_CS = BITFIELD64_BIT(2),
   PCO_DEBUG_PRINT_ALL = PCO_DEBUG_PRINT_VS | PCO_DEBUG_PRINT_FS |
                         PCO_DEBUG_PRINT_CS,
   PCO_DEBUG_PRINT_INTERNAL = BITFIELD64_BIT(3),
   PCO_DEBUG_PRINT_PASSES = BITFIELD64_BIT(4),
   PCO_DEBUG_PRINT_NIR = BITFIELD64_BIT(5),
   PCO_DEBUG_PRINT_BINARY = BITFIELD64_BIT(6),
};

extern uint64_t pco_debug_print;

extern const char *pco_skip_passes;

#define PCO_DEBUG_PRINT(flag) \
   unlikely(pco_debug_print &(PCO_DEBUG_PRINT_##flag))

extern bool pco_color;

void pco_debug_init(void);
/** Op info. */
struct pco_op_info {
   const char *str; /** Op name string. */
   unsigned num_dests; /** Number of dests. */
   unsigned num_srcs; /** Number of sources. */
   uint64_t mods; /** Supported mods. */
   uint8_t mod_map[_PCO_OP_MOD_COUNT]; /** Index into pco_instr::mod. */
   uint64_t dest_mods[_PCO_OP_MAX_DESTS]; /** Supported dest mods. */
   uint64_t src_mods[_PCO_OP_MAX_SRCS]; /** Supported source mods. */
   bool is_pseudo; /** Set if op is a pseudo-instruction. */
   bool has_target_cf_node; /** Set if op has a cf-node as a target. */
};
extern const struct pco_op_info pco_op_info[_PCO_OP_COUNT];

/** Op mod info. */
struct pco_op_mod_info {
   bool print_early : 1; /** Set if printed before the op. */
   enum pco_mod_type type; /** Datatype. */
   union {
      const char *str; /** Mod name. */
      const char **strs; /** Mod names (enums). */
   };
   uint32_t nzdefault; /** Default value if non-zero. */
};
extern const struct pco_op_mod_info pco_op_mod_info[_PCO_OP_MOD_COUNT];

/** Reference mod info. */
struct pco_ref_mod_info {
   bool is_bitset : 1; /** Set if type is an enum bitset. */
   enum pco_mod_type type; /** Datatype. */
   union {
      const char *str; /** Mod name. */
      const char **strs; /** Mod names (enums). */
   };
};
extern const struct pco_ref_mod_info pco_ref_mod_info[_PCO_REF_MOD_COUNT];

#endif /* PCO_INTERNAL_H */
