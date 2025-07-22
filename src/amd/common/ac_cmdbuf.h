/*
 * Copyright 2012 Advanced Micro Devices, Inc.
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef AC_CMDBUF_H
#define AC_CMDBUF_H

#include <inttypes.h>

#include "ac_pm4.h"

#ifdef __cplusplus
extern "C" {
#endif

struct ac_cmdbuf {
   uint32_t cdw;         /* Number of used dwords. */
   uint32_t max_dw;      /* Maximum number of dwords. */
   uint32_t reserved_dw; /* Number of dwords reserved. */
   uint32_t *buf;        /* The base pointer of the chunk. */
};

#define ac_cmdbuf_begin(cs) struct ac_cmdbuf *__cs = (cs);                        \
                            uint32_t __cs_num = __cs->cdw;                        \
                            UNUSED uint32_t __cs_num_initial = __cs_num;          \
                            UNUSED uint32_t __cs_reserved_dw = __cs->reserved_dw; \
                            uint32_t *__cs_buf = __cs->buf

#define ac_cmdbuf_begin_again(cs) do { \
   assert(__cs == NULL);               \
   __cs = (cs);                        \
   __cs_num = __cs->cdw;               \
   __cs_num_initial = __cs_num;        \
   __cs_buf = __cs->buf;               \
} while (0)

#define ac_cmdbuf_emit(value)                                   \
   do {                                                         \
      assert(!__cs_reserved_dw || __cs_num < __cs_reserved_dw); \
      __cs_buf[__cs_num++] = (value);                           \
   } while (0)

#define ac_cmdbuf_packets_added()  (__cs_num != __cs_num_initial)

#define ac_cmdbuf_emit_array(values, num) do {                       \
   unsigned __n = (num);                                             \
   assert(!__cs_reserved_dw || __cs_num + __n <= __cs_reserved_dw);  \
   memcpy(__cs_buf + __cs_num, (values), __n * 4);                   \
   __cs_num += __n;                                                  \
} while (0)

#define ac_cmdbuf_end() do {           \
   __cs->cdw = __cs_num;               \
   assert(__cs->cdw <= __cs->max_dw);  \
   __cs = NULL;                        \
} while (0)

/* Packet building helpers. Don't use directly. */
#define __ac_cmdbuf_set_reg_seq(reg, num, idx, prefix_name, packet, reset_filter_cam)     \
   do {                                                                                   \
      assert((reg) >= prefix_name##_REG_OFFSET && (reg) < prefix_name##_REG_END);         \
      ac_cmdbuf_emit(PKT3(packet, num, 0) | PKT3_RESET_FILTER_CAM_S(reset_filter_cam));   \
      ac_cmdbuf_emit((((reg) - prefix_name##_REG_OFFSET) >> 2) | ((idx) << 28));          \
   } while (0)

#define __ac_cmdbuf_set_reg(reg, idx, value, prefix_name, packet)                         \
   do {                                                                                   \
      __ac_cmdbuf_set_reg_seq(reg, 1, idx, prefix_name, packet, 0);                       \
      ac_cmdbuf_emit(value);                                                              \
   } while (0)

/* Packet building helpers for CONFIG registers. */
#define ac_cmdbuf_set_config_reg_seq(reg, num) __ac_cmdbuf_set_reg_seq(reg, num, 0, SI_CONFIG, PKT3_SET_CONFIG_REG, 0)

#define ac_cmdbuf_set_config_reg(reg, value) __ac_cmdbuf_set_reg(reg, 0, value, SI_CONFIG, PKT3_SET_CONFIG_REG)

struct ac_preamble_state {
   uint64_t border_color_va;

   struct {
      bool cache_cb_gl2;
      bool cache_db_gl2;
   } gfx10;

   struct {
      uint32_t compute_dispatch_interleave;
   } gfx11;
};

void
ac_init_compute_preamble_state(const struct ac_preamble_state *state,
                               struct ac_pm4_state *pm4);

void
ac_init_graphics_preamble_state(const struct ac_preamble_state *state,
                                struct ac_pm4_state *pm4);

#ifdef __cplusplus
}
#endif

#endif
