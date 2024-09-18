/*
 * Copyright (c) 2023-2024 Tomeu Vizoso <tomeu@tomeuvizoso.net>
 * SPDX-License-Identifier: MIT
 */

#ifndef H_ETNA_ML
#define H_ETNA_ML

#include "pipe/p_state.h"
#include "util/u_dynarray.h"
#include "etnaviv_context.h"

#define MAX_CONFIG_BOS 4

/*
 * SWAP - swap value of @a and @b
 */
#define SWAP(a, b)                                                             \
   do {                                                                        \
      __typeof(a) __tmp = (a);                                                 \
      (a) = (b);                                                               \
      (b) = __tmp;                                                             \
   } while (0)

enum etna_job_type {
    ETNA_JOB_TYPE_NN,
    ETNA_JOB_TYPE_TP,
};

enum etna_ml_tp_type {
   ETNA_ML_TP_TRANSPOSE,
   ETNA_ML_TP_DETRANSPOSE,
   ETNA_ML_TP_RESHUFFLE,
};

struct etna_ml_subgraph {
   struct pipe_ml_subgraph base;

   struct util_dynarray operations;

   /* The three are indexed by tensor index */
   struct util_dynarray tensors; /* Contains struct pipe_resource* */
   struct util_dynarray offsets; /* These are integers */
   struct util_dynarray sizes; /* These are integers */
};

struct etna_vip_instruction {
   enum etna_job_type type;

   struct etna_bo *configs[MAX_CONFIG_BOS];
   struct etna_bo *coefficients;
   struct pipe_resource *input;
   unsigned input_offset;
   struct pipe_resource *output;
   unsigned output_offset;

   struct etna_bo *kernel;
};

#define MAX_INPUTS 10
struct etna_operation {
   struct list_head link;

   enum etna_job_type type;
   enum etna_ml_tp_type tp_type;

   bool addition;
   bool depthwise;
   bool pointwise;
   bool pooling_first_pixel;
   bool padding_same;
   bool relu;

   unsigned stride;

   unsigned input_tensors[MAX_INPUTS];
   unsigned input_count;
   unsigned input_tensor_size;
   unsigned input_width;
   unsigned input_height;
   unsigned input_channels;
   uint8_t input_zero_point;
   float input_scale;

   unsigned output_tensor;
   unsigned output_width;
   unsigned output_height;
   unsigned output_channels;
   uint8_t output_zero_point;
   float output_scale;

   struct pipe_resource *weight_tensor;
   unsigned weight_width;
   unsigned weight_height;
   uint8_t weight_zero_point;
   float weight_scale;

   uint8_t addition_offset;

   struct pipe_resource *bias_tensor;
};

#define ML_DBG(fmt, ...)                                  \
   do {                                                   \
      if (DBG_ENABLED(ETNA_DBG_ML_MSGS))                  \
         _debug_printf(fmt, ##__VA_ARGS__);             \
   } while (0)

unsigned etna_ml_allocate_tensor(struct etna_ml_subgraph *subgraph);
struct pipe_resource *etna_ml_get_tensor(struct etna_ml_subgraph *subgraph, unsigned idx);
unsigned etna_ml_get_offset(struct etna_ml_subgraph *subgraph, unsigned idx);

struct etna_bo *etna_ml_create_bo(struct pipe_context *pctx, size_t size);

struct pipe_resource *etna_ml_create_resource(struct pipe_context *pctx, size_t size);

struct etna_core_npu_info *etna_ml_get_core_info(struct etna_context *context);

struct pipe_ml_subgraph *
etna_ml_subgraph_create(struct pipe_context *context,
                        const struct pipe_ml_operation *operations,
                        unsigned count);

void
etna_ml_subgraph_invoke(struct pipe_context *pctx, struct pipe_ml_subgraph *psubgraph,
                        unsigned inputs_count, unsigned input_idxs[], void *inputs[]);

void
etna_ml_subgraph_read_outputs(struct pipe_context *context, struct pipe_ml_subgraph *subgraph,
                              unsigned outputs_count, unsigned output_idxs[], void *outputs[]);

void
etna_ml_subgraph_destroy(struct pipe_context *context, struct pipe_ml_subgraph *subgraph);

#endif
