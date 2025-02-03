/*
 * Copyright © 2014 Connor Abbott
 * SPDX-License-Identifier: MIT
 */

/*
 * This file is split off from nir.h to allow #include'ing these defines from
 * OpenCL code.
 */

#ifndef NIR_DEFINES_H
#define NIR_DEFINES_H

#ifdef __cplusplus
extern "C" {
#endif

typedef struct shader_info shader_info;

typedef struct nir_shader nir_shader;
typedef struct nir_shader_compiler_options nir_shader_compiler_options;
typedef struct nir_builder nir_builder;
typedef struct nir_def nir_def;
typedef struct nir_variable nir_variable;

typedef struct nir_cf_node nir_cf_node;
typedef struct nir_block nir_block;
typedef struct nir_if nir_if;
typedef struct nir_loop nir_loop;
typedef struct nir_function nir_function;
typedef struct nir_function_impl nir_function_impl;

typedef struct nir_instr nir_instr;
typedef struct nir_alu_instr nir_alu_instr;
typedef struct nir_deref_instr nir_deref_instr;
typedef struct nir_call_instr nir_call_instr;
typedef struct nir_jump_instr nir_jump_instr;
typedef struct nir_tex_instr nir_tex_instr;
typedef struct nir_intrinsic_instr nir_intrinsic_instr;
typedef struct nir_load_const_instr nir_load_const_instr;
typedef struct nir_undef_instr nir_undef_instr;
typedef struct nir_phi_instr nir_phi_instr;
typedef struct nir_parallel_copy_instr nir_parallel_copy_instr;

typedef struct nir_xfb_info nir_xfb_info;
typedef struct nir_tcs_info nir_tcs_info;

/** NIR sized and unsized types
 *
 * The values in this enum are carefully chosen so that the sized type is
 * just the unsized type OR the number of bits.
 */
/* clang-format off */
typedef enum ENUM_PACKED {
   nir_type_invalid =   0, /* Not a valid type */
   nir_type_int =       2,
   nir_type_uint =      4,
   nir_type_bool =      6,
   nir_type_float =     128,
   nir_type_bool1 =     1  | nir_type_bool,
   nir_type_bool8 =     8  | nir_type_bool,
   nir_type_bool16 =    16 | nir_type_bool,
   nir_type_bool32 =    32 | nir_type_bool,
   nir_type_int1 =      1  | nir_type_int,
   nir_type_int8 =      8  | nir_type_int,
   nir_type_int16 =     16 | nir_type_int,
   nir_type_int32 =     32 | nir_type_int,
   nir_type_int64 =     64 | nir_type_int,
   nir_type_uint1 =     1  | nir_type_uint,
   nir_type_uint8 =     8  | nir_type_uint,
   nir_type_uint16 =    16 | nir_type_uint,
   nir_type_uint32 =    32 | nir_type_uint,
   nir_type_uint64 =    64 | nir_type_uint,
   nir_type_float16 =   16 | nir_type_float,
   nir_type_float32 =   32 | nir_type_float,
   nir_type_float64 =   64 | nir_type_float,
} nir_alu_type;
/* clang-format on */

#define NIR_ALU_TYPE_SIZE_MASK      0x79
#define NIR_ALU_TYPE_BASE_TYPE_MASK 0x86

static inline unsigned
nir_alu_type_get_type_size(nir_alu_type type)
{
   return type & NIR_ALU_TYPE_SIZE_MASK;
}

static inline nir_alu_type
nir_alu_type_get_base_type(nir_alu_type type)
{
   return (nir_alu_type)(type & NIR_ALU_TYPE_BASE_TYPE_MASK);
}

#ifdef __cplusplus
}
#endif

#endif
