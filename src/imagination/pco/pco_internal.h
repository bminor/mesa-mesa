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
#include "util/compiler.h"
#include "util/macros.h"
#include "util/list.h"
#include "util/u_dynarray.h"
#include "util/u_math.h"

#include <assert.h>
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
   PCO_DEBUG_PRINT_VERBOSE = BITFIELD64_BIT(7),
};

extern uint64_t pco_debug_print;

extern const char *pco_skip_passes;

#define PCO_DEBUG_PRINT(flag) \
   unlikely(pco_debug_print &(PCO_DEBUG_PRINT_##flag))

extern bool pco_color;

void pco_debug_init(void);

typedef struct _pco_cf_node pco_cf_node;
typedef struct _pco_func pco_func;
typedef struct _pco_block pco_block;
typedef struct _pco_instr pco_instr;

/** PCO reference index. */
typedef struct PACKED _pco_ref {
   /** Reference value. */
   union PACKED {
      unsigned val : 32;

      struct PACKED {
         unsigned num : 2; /** Index register number. */
         unsigned offset : 8; /** Offset. */
         unsigned _pad : 22;
      } idx_reg;
   };

   /** Source/destination modifiers. */
   bool oneminus : 1;
   bool clamp : 1;
   bool abs : 1;
   bool neg : 1;
   bool flr : 1;
   enum pco_elem elem : 4; /** .e0.e1.e2.e3 */

   enum pco_dtype dtype : 2; /** Reference data-type. */
   unsigned chans : 10; /** Number of channels (1-1024). */
   enum pco_bits bits : 3; /** Bit width. */
   enum pco_ref_type type : 3; /** Reference type. */
   enum pco_reg_class reg_class : 4; /** Register class. */

   unsigned _pad : 1;
} pco_ref;
static_assert(sizeof(pco_ref) == 8, "sizeof(pco_ref) != 8");

/** PCO phi source. */
typedef struct _pco_phi_src {
   struct list_head link; /** Link in pco_instr::phi_srcs. */

   pco_block *pred; /** Predecessor block. */
   pco_ref ref; /** Source reference. */
} pco_phi_src;

/** PCO instruction group. */
typedef struct _pco_igrp {
   struct list_head link; /** Link in pco_block::instrs. */
   pco_block *parent_block; /** Basic block containing the igrp. */
   pco_func *parent_func; /** Parent function. */

   pco_instr *instrs[_PCO_OP_PHASE_COUNT]; /** Instruction/group list. */

   /** Instruction group header. */
   struct {
      unsigned da;
      unsigned length;
      union {
         enum pco_oporg oporg;
         enum pco_opcnt opcnt;
      };
      bool olchk;
      bool w1p;
      bool w0p;
      enum pco_cc cc;
      enum pco_alutype alutype;
      union {
         struct {
            bool end;
            bool atom;
            unsigned rpt;
         };
         struct {
            unsigned miscctl;
            enum pco_ctrlop ctrlop;
         };
      };
   } hdr;

   struct {
      pco_ref s0;
      pco_ref s1;
      pco_ref s2;

      pco_ref s3;
      pco_ref s4;
      pco_ref s5;
   } srcs;

   struct {
      pco_ref is0;
      pco_ref is1;
      pco_ref is2;
      pco_ref is3;
      pco_ref is4;
      pco_ref is5;
   } iss;

   struct {
      pco_ref w0;
      pco_ref w1;
   } dests;

   struct {
      enum pco_igrp_hdr_variant igrp_hdr;
      union {
         enum pco_main_variant main;
         enum pco_backend_variant backend;
         enum pco_bitwise_variant bitwise;
         enum pco_ctrl_variant ctrl;
      } instr[_PCO_OP_PHASE_COUNT];
      enum pco_src_variant lower_src;
      enum pco_src_variant upper_src;
      enum pco_iss_variant iss;
      enum pco_dst_variant dest;
   } variant;

   struct {
      struct {
         unsigned hdr;
         unsigned lower_srcs;
         unsigned upper_srcs;
         unsigned iss;
         unsigned dests;
         unsigned instrs[_PCO_OP_PHASE_COUNT];
         unsigned word_padding;
         unsigned align_padding;
         unsigned total;
      } len;

      unsigned offset;
   } enc;

   unsigned index; /** Igrp index. */
   char *comment; /** Comment string. */

} pco_igrp;

/** PCO instruction. */
typedef struct _pco_instr {
   union {
      struct {
         struct list_head link; /** Link in pco_block::instrs. */
         pco_block *parent_block; /** Basic block containing the instruction. */
      };

      pco_igrp *parent_igrp; /** Igrp containing the instruction. */
   };

   pco_func *parent_func; /** Parent function. */

   enum pco_op op;

   unsigned num_dests;
   pco_ref *dest;
   unsigned num_srcs;
   pco_ref *src;

   union {
      struct list_head phi_srcs;
      pco_cf_node *target_cf_node;
   };

   /** Instruction flags/modifiers. */
   uint32_t mod[_PCO_OP_MAX_MODS];

   unsigned index; /** Instruction index. */
   char *comment; /** Comment string. */
} pco_instr;

/** PCO control-flow node type. */
enum pco_cf_node_type {
   PCO_CF_NODE_TYPE_BLOCK,
   PCO_CF_NODE_TYPE_IF,
   PCO_CF_NODE_TYPE_LOOP,
   PCO_CF_NODE_TYPE_FUNC,
};

/** PCO control-flow node. */
typedef struct _pco_cf_node {
   struct list_head link; /** Link in lists of pco_cf_nodes. */
   enum pco_cf_node_type type; /** CF node type. */
   struct _pco_cf_node *parent; /** Parent cf node. */
} pco_cf_node;

/** PCO basic block. */
typedef struct _pco_block {
   pco_cf_node cf_node; /** Control flow node. */
   pco_func *parent_func; /** Parent function. */
   struct list_head instrs; /** Instruction/group list. */
   unsigned index; /** Block index. */
} pco_block;

/** PCO if cf construct. */
typedef struct _pco_if {
   pco_cf_node cf_node; /** CF node. */
   pco_func *parent_func; /** Parent function. */
   pco_ref cond; /** If condition. */
   struct list_head then_body; /** List of pco_cf_nodes for if body. */
   struct list_head else_body; /** List of pco_cf_nodes for else body. */
   unsigned index; /** If index. */
} pco_if;

/** PCO loop cf construct. */
typedef struct _pco_loop {
   pco_cf_node cf_node; /** CF node. */
   pco_func *parent_func; /** Parent function. */
   struct list_head body; /** List of pco_cf_nodes for loop body. */
   unsigned index; /** Loop index. */
} pco_loop;

/** PCO function. */
typedef struct _pco_func {
   struct list_head link; /** Link in pco_shader::funcs. */
   pco_cf_node cf_node; /** Control flow node. */

   pco_shader *parent_shader; /** Shader containing the function. */

   enum pco_func_type type; /** Function type. */
   unsigned index; /** Function index. */
   const char *name; /** Function name. */

   struct list_head body; /** List of pco_cf_nodes for function body. */

   unsigned num_params;
   pco_ref *params;

   unsigned next_ssa; /** Next SSA node index. */
   unsigned next_instr; /** Next instruction index. */
   unsigned next_igrp; /** Next igrp index. */
   unsigned next_block; /** Next block index. */
   unsigned next_if; /** Next if index. */
   unsigned next_loop; /** Next loop index. */

   unsigned temps; /** Number of temps allocated. */
} pco_func;

/** PCO shader. */
typedef struct _pco_shader {
   pco_ctx *ctx; /** Compiler context. */
   nir_shader *nir; /** Source NIR shader. */

   gl_shader_stage stage; /** Shader stage. */
   const char *name; /** Shader name. */
   bool is_internal; /** Whether this is an internal shader. */
   bool is_grouped; /** Whether the shader uses igrps. */

   struct list_head funcs; /** List of functions. */
   unsigned next_func; /** Next function index. */

   struct {
      struct util_dynarray buf; /** Shader binary. */

      /** Binary patch info. */
      unsigned num_patches;
      struct {
         unsigned offset;
      } * patch;
   } binary;
} pco_shader;

/** Op info. */
struct pco_op_info {
   const char *str; /** Op name string. */
   unsigned num_dests; /** Number of dests. */
   unsigned num_srcs; /** Number of sources. */
   uint64_t mods; /** Supported mods. */
   uint8_t mod_map[_PCO_OP_MOD_COUNT]; /** Index into pco_instr::mod. */
   uint64_t dest_mods[_PCO_OP_MAX_DESTS]; /** Supported dest mods. */
   uint64_t src_mods[_PCO_OP_MAX_SRCS]; /** Supported source mods. */
   enum pco_op_type type; /** Op type. */
   bool has_target_cf_node; /** Set if op has a cf-node as a target. */
};
extern const struct pco_op_info pco_op_info[_PCO_OP_COUNT];

/** Op mod info. */
struct pco_op_mod_info {
   bool print_early : 1; /** Set if printed before the op. */
   bool is_bitset : 1; /** Set if type is an enum bitset. */
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

pco_shader *pco_shader_create(pco_ctx *ctx, nir_shader *nir, void *mem_ctx);
pco_func *pco_func_create(pco_shader *shader,
                          enum pco_func_type type,
                          unsigned num_params);
pco_block *pco_block_create(pco_func *func);
pco_if *pco_if_create(pco_func *func);
pco_loop *pco_loop_create(pco_func *func);
pco_instr *pco_instr_create(pco_func *func,
                            enum pco_op op,
                            unsigned num_dests,
                            unsigned num_srcs);

/* Cast helpers. */

/* CF nodes. */
#define PCO_DEFINE_CAST(name, in_type, out_type, field, type_field, type_value) \
   static inline out_type *name(const in_type *parent)                          \
   {                                                                            \
      assert(parent && parent->type_field == type_value);                       \
      return list_entry(parent, out_type, field);                               \
   }

PCO_DEFINE_CAST(pco_cf_node_as_block,
                pco_cf_node,
                pco_block,
                cf_node,
                type,
                PCO_CF_NODE_TYPE_BLOCK)
PCO_DEFINE_CAST(pco_cf_node_as_if,
                pco_cf_node,
                pco_if,
                cf_node,
                type,
                PCO_CF_NODE_TYPE_IF)
PCO_DEFINE_CAST(pco_cf_node_as_loop,
                pco_cf_node,
                pco_loop,
                cf_node,
                type,
                PCO_CF_NODE_TYPE_LOOP)
PCO_DEFINE_CAST(pco_cf_node_as_func,
                pco_cf_node,
                pco_func,
                cf_node,
                type,
                PCO_CF_NODE_TYPE_FUNC)

/* Iterators. */
#define pco_foreach_func_in_shader(func, shader) \
   list_for_each_entry (pco_func, func, &(shader)->funcs, link)

#define pco_foreach_cf_node_in_if_then(cf_node, _if) \
   list_for_each_entry (pco_cf_node, cf_node, &(_if)->then_body, link)

#define pco_foreach_cf_node_in_if_else(cf_node, _if) \
   list_for_each_entry (pco_cf_node, cf_node, &(_if)->else_body, link)

#define pco_foreach_cf_node_in_loop(cf_node, loop) \
   list_for_each_entry (pco_cf_node, cf_node, &(loop)->body, link)

#define pco_foreach_cf_node_in_func(cf_node, func) \
   list_for_each_entry (pco_cf_node, cf_node, &(func)->body, link)

#define pco_foreach_block_in_func(block, func)                   \
   for (pco_block *block = pco_first_block(func); block != NULL; \
        block = pco_next_block(block))

#define pco_foreach_instr_in_block(instr, block)           \
   assert(!block->parent_func->parent_shader->is_grouped); \
   list_for_each_entry (pco_instr, instr, &(block)->instrs, link)

#define pco_foreach_instr_in_block_safe(instr, block)      \
   assert(!block->parent_func->parent_shader->is_grouped); \
   list_for_each_entry_safe (pco_instr, instr, &(block)->instrs, link)

#define pco_foreach_igrp_in_block(igrp, block)            \
   assert(block->parent_func->parent_shader->is_grouped); \
   list_for_each_entry (pco_igrp, igrp, &(block)->instrs, link)

#define pco_foreach_phi_src_in_instr(phi_src, instr) \
   list_for_each_entry (pco_phi_src, phi_src, &(instr)->phi_srcs, link)

/**
 * \brief Returns the preamble function of a PCO shader.
 *
 * \param[in] shader The PCO shader.
 * \return The preamble function, or NULL if the shader has no preamble.
 */
static inline pco_func *pco_preamble(pco_shader *shader)
{
   if (list_is_empty(&shader->funcs))
      return NULL;

   pco_func *func = list_first_entry(&shader->funcs, pco_func, link);
   if (func->type == PCO_FUNC_TYPE_PREAMBLE)
      return func;

   return NULL;
}

/**
 * \brief Returns the entrypoint function of a PCO shader.
 *
 * \param[in] shader The PCO shader.
 * \return The entrypoint function, or NULL if the shader has no entrypoint.
 */
static inline pco_func *pco_entrypoint(pco_shader *shader)
{
   if (list_is_empty(&shader->funcs))
      return NULL;

   /* Entrypoint will either be the first or second function in the shader,
    * depending on whether or not there is a preamble.
    */
   pco_func *preamble = pco_preamble(shader);
   pco_func *func = !preamble ? list_first_entry(&shader->funcs, pco_func, link)
                              : list_entry(preamble->link.next, pco_func, link);

   if (func->type == PCO_FUNC_TYPE_ENTRYPOINT)
      return func;

   return NULL;
}

/* Motions. */
/**
 * \brief Returns the first block in a function.
 *
 * \param[in] func The function.
 * \return The first block, or NULL if the function body is empty.
 */
static inline pco_block *pco_first_block(pco_func *func)
{
   pco_cf_node *cf_node = list_first_entry(&func->body, pco_cf_node, link);
   if (list_is_empty(&func->body))
      return NULL;

   return pco_cf_node_as_block(cf_node);
}

/**
 * \brief Returns the last block in a function.
 *
 * \param[in] func The function.
 * \return The last block, or NULL if the function body is empty.
 */
static inline pco_block *pco_last_block(pco_func *func)
{
   pco_cf_node *cf_node = list_last_entry(&func->body, pco_cf_node, link);
   if (list_is_empty(&func->body))
      return NULL;

   return pco_cf_node_as_block(cf_node);
}

/**
 * \brief Returns the first instruction in a block.
 *
 * \param[in] block The block.
 * \return The first instruction, or NULL if the block is empty.
 */
static inline pco_instr *pco_first_instr(pco_block *block)
{
   assert(!block->parent_func->parent_shader->is_grouped);
   if (list_is_empty(&block->instrs))
      return NULL;

   return list_first_entry(&block->instrs, pco_instr, link);
}

/**
 * \brief Returns the last instruction in a block.
 *
 * \param[in] block The block.
 * \return The last instruction, or NULL if the block is empty.
 */
static inline pco_instr *pco_last_instr(pco_block *block)
{
   assert(!block->parent_func->parent_shader->is_grouped);
   if (list_is_empty(&block->instrs))
      return NULL;

   return list_last_entry(&block->instrs, pco_instr, link);
}

/**
 * \brief Returns the next instruction.
 *
 * \param[in] instr The current instruction.
 * \return The next instruction, or NULL if the end of the block has been
 *         reached.
 */
static inline pco_instr *pco_next_instr(pco_instr *instr)
{
   assert(!instr->parent_func->parent_shader->is_grouped);
   if (!instr || instr == pco_last_instr(instr->parent_block))
      return NULL;

   return list_entry(instr->link.next, pco_instr, link);
}

/**
 * \brief Returns the previous instruction.
 *
 * \param[in] instr The current instruction.
 * \return The previous instruction, or NULL if the start of the block has been
 *         reached.
 */
static inline pco_instr *pco_prev_instr(pco_instr *instr)
{
   assert(!instr->parent_func->parent_shader->is_grouped);
   if (!instr || instr == pco_first_instr(instr->parent_block))
      return NULL;

   return list_entry(instr->link.prev, pco_instr, link);
}

/**
 * \brief Returns the first instruction group in a block.
 *
 * \param[in] block The block.
 * \return The first instruction group, or NULL if the block is empty.
 */
static inline pco_igrp *pco_first_igrp(pco_block *block)
{
   assert(block->parent_func->parent_shader->is_grouped);
   if (list_is_empty(&block->instrs))
      return NULL;

   return list_first_entry(&block->instrs, pco_igrp, link);
}

/**
 * \brief Returns the last instruction group in a block.
 *
 * \param[in] block The block.
 * \return The last instruction group, or NULL if the block is empty.
 */
static inline pco_igrp *pco_last_igrp(pco_block *block)
{
   assert(block->parent_func->parent_shader->is_grouped);
   if (list_is_empty(&block->instrs))
      return NULL;

   return list_last_entry(&block->instrs, pco_igrp, link);
}

/**
 * \brief Returns the next instruction group.
 *
 * \param[in] igrp The current instruction group.
 * \return The next instruction group, or NULL if the end of the block has been
 *         reached.
 */
static inline pco_igrp *pco_next_igrp(pco_igrp *igrp)
{
   assert(igrp->parent_func->parent_shader->is_grouped);
   if (!igrp || igrp == pco_last_igrp(igrp->parent_block))
      return NULL;

   return list_entry(igrp->link.next, pco_igrp, link);
}

/**
 * \brief Returns the previous instruction group.
 *
 * \param[in] igrp The current instruction group.
 * \return The previous instruction group, or NULL if the start of the block has
 *         been reached.
 */
static inline pco_igrp *pco_prev_igrp(pco_igrp *igrp)
{
   assert(igrp->parent_func->parent_shader->is_grouped);
   if (!igrp || igrp == pco_first_igrp(igrp->parent_block))
      return NULL;

   return list_entry(igrp->link.prev, pco_igrp, link);
}

/* Debug printing helpers. */
static inline bool pco_should_print_nir(nir_shader *nir)
{
   if (!PCO_DEBUG_PRINT(NIR))
      return false;

   if (nir->info.internal && !PCO_DEBUG_PRINT(INTERNAL))
      return false;

   if (nir->info.stage == MESA_SHADER_VERTEX && !PCO_DEBUG_PRINT(VS))
      return false;
   else if (nir->info.stage == MESA_SHADER_FRAGMENT && !PCO_DEBUG_PRINT(FS))
      return false;
   else if (nir->info.stage == MESA_SHADER_COMPUTE && !PCO_DEBUG_PRINT(CS))
      return false;

   return true;
}

static inline bool pco_should_print_shader(pco_shader *shader)
{
   if (shader->is_internal && !PCO_DEBUG_PRINT(INTERNAL))
      return false;

   if (shader->stage == MESA_SHADER_VERTEX && !PCO_DEBUG_PRINT(VS))
      return false;
   else if (shader->stage == MESA_SHADER_FRAGMENT && !PCO_DEBUG_PRINT(FS))
      return false;
   else if (shader->stage == MESA_SHADER_COMPUTE && !PCO_DEBUG_PRINT(CS))
      return false;

   return true;
}

static inline bool pco_should_print_shader_pass(pco_shader *shader)
{
   if (!PCO_DEBUG_PRINT(PASSES))
      return false;

   if (shader->is_internal && !PCO_DEBUG_PRINT(INTERNAL))
      return false;

   if (shader->stage == MESA_SHADER_VERTEX && !PCO_DEBUG_PRINT(VS))
      return false;
   else if (shader->stage == MESA_SHADER_FRAGMENT && !PCO_DEBUG_PRINT(FS))
      return false;
   else if (shader->stage == MESA_SHADER_COMPUTE && !PCO_DEBUG_PRINT(CS))
      return false;

   return true;
}

static inline bool pco_should_print_binary(pco_shader *shader)
{
   if (!PCO_DEBUG_PRINT(BINARY))
      return false;

   if (shader->is_internal && !PCO_DEBUG_PRINT(INTERNAL))
      return false;

   if (shader->stage == MESA_SHADER_VERTEX && !PCO_DEBUG_PRINT(VS))
      return false;
   else if (shader->stage == MESA_SHADER_FRAGMENT && !PCO_DEBUG_PRINT(FS))
      return false;
   else if (shader->stage == MESA_SHADER_COMPUTE && !PCO_DEBUG_PRINT(CS))
      return false;

   return true;
}

/* PCO IR passes. */
bool pco_end(pco_shader *shader);

/* PCO ref checkers. */
/**
 * \brief Returns whether a reference is an index register.
 *
 * \param[in] ref PCO reference.
 * \return True if the reference is an index register.
 */
static inline bool pco_ref_is_idx_reg(pco_ref ref)
{
   return ref.type == PCO_REF_TYPE_IDX_REG;
}

/**
 * \brief Returns whether a reference is an immediate.
 *
 * \param[in] ref PCO reference.
 * \return True if the reference is an immediate.
 */
static inline bool pco_ref_is_imm(pco_ref ref)
{
   return ref.type == PCO_REF_TYPE_IMM;
}

/* PCO ref getters. */
/**
 * \brief Returns the pointee component of an indexed register reference.
 *
 * \param[in] ref Indexed register reference.
 * \return Pointee component of the indexed register reference.
 */
static inline pco_ref pco_ref_get_idx_pointee(pco_ref ref)
{
   assert(pco_ref_is_idx_reg(ref));

   pco_ref pointee = ref;
   pointee.val = ref.idx_reg.offset;
   pointee.type = PCO_REF_TYPE_REG;

   return pointee;
}

/**
 * \brief Returns the data type of a reference.
 *
 * \param[in] ref Reference.
 * \return Datatype.
 */
static inline enum pco_dtype pco_ref_get_dtype(pco_ref ref)
{
   return ref.dtype;
}

/**
 * \brief Returns the number of channels for a reference type.
 *
 * \param[in] ref Reference.
 * \return Number of channels.
 */
static inline unsigned pco_ref_get_chans(pco_ref ref)
{
   return ref.chans + 1;
}

/**
 * \brief Returns the number of bits for a reference type.
 *
 * \param[in] ref Reference.
 * \return Number of bits.
 */
static inline unsigned pco_ref_get_bits(pco_ref ref)
{
   switch (ref.bits) {
   case PCO_BITS_1:
      return 1;

   case PCO_BITS_8:
      return 8;

   case PCO_BITS_16:
      return 16;

   case PCO_BITS_ANY:
   case PCO_BITS_32:
      return 32;

   case PCO_BITS_64:
      return 64;

   default:
      break;
   }

   unreachable();
}

/**
 * \brief Returns the bit-sized value in an immediate reference.
 *
 * \param[in] ref Reference.
 * \return Immediate value.
 */
static inline uint64_t pco_ref_get_imm(pco_ref ref)
{
   assert(pco_ref_is_imm(ref));

   unsigned num_bits = pco_ref_get_bits(ref);

   switch (ref.dtype) {
   case PCO_DTYPE_FLOAT:
      assert(num_bits == 32);
      FALLTHROUGH;
   case PCO_DTYPE_ANY:
      FALLTHROUGH;
   case PCO_DTYPE_UNSIGNED:
      return ref.val & BITFIELD_MASK(num_bits);

   case PCO_DTYPE_SIGNED:
      return util_sign_extend(ref.val, num_bits);

   default:
      break;
   }

   unreachable();
}

/* PCO ref builders. */
#endif /* PCO_INTERNAL_H */
