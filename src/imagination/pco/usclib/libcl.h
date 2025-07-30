/*
 * Copyright © 2025 Imagination Technologies Ltd.
 * SPDX-License-Identifier: MIT
 */

#ifndef PCO_LIBCL_H
#define PCO_LIBCL_H

#include "compiler/libcl/libcl.h"
#include "compiler/shader_enums.h"
#include "pco/pco_common.h"
#include "pp_macros.h"

#define OVERLOADABLE __attribute__((overloadable))

void nir_mutex_pco(enum pco_mutex_id mutex_id, enum pco_mutex_op mutex_op);
uint32_t nir_load_instance_num_pco(void);

uint32_t nir_load_ssbo(uint2 buffer_index,
                       uint offset,
                       enum gl_access_qualifier access,
                       uint align_mul,
                       uint align_offset,
                       uint offset_shift);

void nir_store_ssbo(uint32_t value,
                    uint2 block_index,
                    uint offset,
                    uint write_mask,
                    enum gl_access_qualifier access,
                    uint align_mul,
                    uint align_offset,
                    uint offset_shift);

uint32_t
nir_load_shared(uint offset, uint base, uint align_mul, uint align_offset);

void nir_store_shared(uint32_t value,
                      uint offset,
                      uint base,
                      uint write_mask,
                      uint align_mul,
                      uint align_offset);

void OVERLOADABLE nir_uvsw_write_pco(uint offset, uint data);
void OVERLOADABLE nir_uvsw_write_pco(uint offset, uint2 data);
void OVERLOADABLE nir_uvsw_write_pco(uint offset, uint3 data);
void OVERLOADABLE nir_uvsw_write_pco(uint offset, uint4 data);

void OVERLOADABLE nir_uvsw_write_pco(uint offset, float data);
void OVERLOADABLE nir_uvsw_write_pco(uint offset, float2 data);
void OVERLOADABLE nir_uvsw_write_pco(uint offset, float3 data);
void OVERLOADABLE nir_uvsw_write_pco(uint offset, float4 data);

uint nir_load_vtxin_pco__1(uint offset);
uint2 nir_load_vtxin_pco__2(uint offset);
uint3 nir_load_vtxin_pco__3(uint offset);
uint4 nir_load_vtxin_pco__4(uint offset);

#define nir_load_vtxin_pco(n, ...) CAT2(nir_load_vtxin_pco__, n)(__VA_ARGS__)
#endif /* PCO_LIBCL_H */
