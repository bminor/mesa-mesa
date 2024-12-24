/*
 * Copyright © 2025 Imagination Technologies Ltd.
 * SPDX-License-Identifier: MIT
 */

#include "compiler/libcl/libcl.h"
#include "compiler/shader_enums.h"

enum pco_mutex_id {
   PCO_MUTEX_ID_ATOMIC_EMU,
   PCO_MUTEX_ID_BARRIER,

   _PCO_MUTEX_ID_COUNT,
};
static_assert(_PCO_MUTEX_ID_COUNT <= 16, "Too many mutex IDs.");

enum pco_mutex_op {
   PCO_MUTEX_OP_RELEASE,
   PCO_MUTEX_OP_RELEASE_SLEEP,
   PCO_MUTEX_OP_RELEASE_WAKEUP,
   PCO_MUTEX_OP_LOCK,
};

#define ROGUE_MAX_INSTANCES_PER_TASK 32

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
