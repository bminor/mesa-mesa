/*
 * Copyright (c) 2023-2024 Tomeu Vizoso <tomeu@tomeuvizoso.net>
 * SPDX-License-Identifier: MIT
 */

#include <stdio.h>
#include <unistd.h>
#include <sys/time.h>

#include "util/u_inlines.h"

#include "etnaviv_context.h"
#include "etnaviv_debug.h"
#include "etnaviv_emit.h"
#include "etnaviv_ml_nn.h"
#include "etnaviv_ml_tp.h"
#include "etnaviv_ml.h"

struct pipe_resource *
etna_ml_get_tensor(struct etna_ml_subgraph *subgraph, unsigned idx)
{
   return *util_dynarray_element(&subgraph->tensors, struct pipe_resource *, idx);
}

unsigned
etna_ml_get_offset(struct etna_ml_subgraph *subgraph, unsigned idx)
{
   return *util_dynarray_element(&subgraph->offsets, unsigned, idx);
}

unsigned
etna_ml_allocate_tensor(struct etna_ml_subgraph *subgraph)
{
   struct pipe_resource **tensors = util_dynarray_grow(&subgraph->tensors, struct pipe_resource *, 1);
   tensors[0] = NULL;

   unsigned *offsets = util_dynarray_grow(&subgraph->offsets, unsigned, 1);
   offsets[0] = 0;

   unsigned *sizes = util_dynarray_grow(&subgraph->sizes, unsigned, 1);
   sizes[0] = 0;

   return util_dynarray_num_elements(&subgraph->tensors, struct pipe_resource *) - 1;
}

static void
etna_ml_create_tensor(struct etna_ml_subgraph *subgraph, unsigned idx, unsigned size)
{
   struct pipe_context *context = subgraph->base.context;
   struct pipe_resource **tensors = util_dynarray_begin(&subgraph->tensors);
   unsigned *sizes = util_dynarray_begin(&subgraph->sizes);

   assert(idx < util_dynarray_num_elements(&subgraph->tensors, struct pipe_resource *));

   struct pipe_resource *res = tensors[idx];

   if (res != NULL) {
      assert(size == pipe_buffer_size(res) || size == pipe_buffer_size(res) / 2);
      return;
   }

   res = etna_ml_create_resource(context, size);
   tensors[idx] = res;
   sizes[idx] = size;

   ML_DBG("created resource %p for tensor %d with size %d\n", res, idx, size);
}

static void
etna_ml_destroy_tensor(struct etna_ml_subgraph *subgraph, unsigned idx)
{
   struct pipe_resource **tensors = util_dynarray_begin(&subgraph->tensors);
   unsigned *offsets = util_dynarray_begin(&subgraph->offsets);
   unsigned *sizes = util_dynarray_begin(&subgraph->sizes);

   pipe_resource_reference(&tensors[idx], NULL);
   offsets[idx] = 0;
   sizes[idx] = 0;
}

struct etna_bo *
etna_ml_create_bo(struct pipe_context *pctx, size_t size)
{
   struct etna_context *ctx = etna_context(pctx);
   struct etna_bo *bo = etna_bo_new(ctx->screen->dev,
                                    size,
                                    DRM_ETNA_GEM_CACHE_WC);

   etna_bo_cpu_prep(bo, DRM_ETNA_PREP_WRITE);
   struct etna_nn_params *map = etna_bo_map(bo);
   memset(map, 0, size);
   etna_bo_cpu_fini(bo);

   return bo;
}

struct pipe_resource *
etna_ml_create_resource(struct pipe_context *pctx, size_t size)
{
   struct pipe_resource *res = pipe_buffer_create(pctx->screen, 0, PIPE_USAGE_DEFAULT, size);
   void *ptr = etna_bo_map(etna_resource(res)->bo);
   memset(ptr, 0, pipe_buffer_size(res));

   return res;
}

struct etna_core_npu_info *
etna_ml_get_core_info(struct etna_context *context)
{
   struct etna_screen *screen = context->screen;
   struct etna_core_info *info = etna_gpu_get_core_info(screen->npu);
   return &info->npu;
}

static bool
needs_reshuffle(struct etna_ml_subgraph *subgraph, const struct pipe_ml_operation *poperation)
{
   struct pipe_context *context = subgraph->base.context;
   struct etna_context *ctx = etna_context(context);
   unsigned nn_core_version = ctx->screen->specs.nn_core_version;
   bool has_stride = poperation->conv.stride_x > 1 || poperation->conv.stride_y > 1;
   bool pointwise = poperation->conv.pointwise;
   unsigned input_width = poperation->input_tensors[0]->dims[1];

   if (!has_stride)
      return false;

   if (nn_core_version < 8)
      return !(poperation->conv.depthwise && (input_width > 5 || input_width < 3)) && !pointwise;
   else {
      unsigned input_channels = poperation->input_tensors[0]->dims[3];

      if (poperation->conv.depthwise)
         return false;

      if (poperation->conv.pointwise && input_width >= 3 && input_channels > 1)
         return false;

      if (poperation->conv.pointwise && poperation->conv.padding_same)
         return false;

      return true;
   }
}

static const struct pipe_ml_operation *
etna_ml_find_producer(const struct pipe_ml_operation *poperations,
                      unsigned count,
                      unsigned tensor_idx)
{
   for (unsigned i = 0; i < count; i++) {
      const struct pipe_ml_operation *poperation = &poperations[i];

      for (unsigned j = 0; j < poperation->output_count; j++)
         if (poperation->output_tensors[j]->index == tensor_idx)
            return poperation;
   }

   return NULL;
}

static const struct pipe_ml_operation *
etna_ml_find_consumer(const struct pipe_ml_operation *poperations,
                      unsigned count,
                      unsigned tensor_idx)
{
   for (unsigned i = 0; i < count; i++) {
      const struct pipe_ml_operation *poperation = &poperations[i];

      for (unsigned j = 0; j < poperation->input_count; j++)
         if (poperation->input_tensors[j]->index == tensor_idx)
            return poperation;
   }

   return NULL;
}

static bool
needs_transpose(const struct pipe_ml_operation *poperations,
                unsigned count,
                const struct pipe_ml_operation *poperation)
{
   const struct pipe_ml_operation *producer;

   if (poperation->input_tensors[0]->dims[3] == 1)
      return false;

   producer = etna_ml_find_producer(poperations, count, poperation->input_tensors[0]->index);
   if (!producer)
      return true;

   return false;
}

static bool
needs_detranspose(const struct pipe_ml_operation *poperations,
                  unsigned count,
                  const struct pipe_ml_operation *poperation)
{
   const struct pipe_ml_operation *consumer;

   if (poperation->output_tensors[0]->dims[3] == 1)
      return false;

   /* TODO: Support multiple consumers */
   consumer = etna_ml_find_consumer(poperations, count, poperation->output_tensors[0]->index);
   if (!consumer)
      return true;

   return false;
}

static void
reference_tensor_with_offset(struct etna_ml_subgraph *subgraph,
                             unsigned src_tensor,
                             unsigned dst_tensor,
                             unsigned offset,
                             unsigned size)
{
   struct pipe_resource **tensors = util_dynarray_begin(&subgraph->tensors);
   unsigned *offsets = util_dynarray_begin(&subgraph->offsets);
   unsigned *sizes = util_dynarray_begin(&subgraph->sizes);
   pipe_resource_reference(&tensors[dst_tensor], tensors[src_tensor]);
   offsets[dst_tensor] = offset;
   sizes[dst_tensor] = size;
}

static void
dump_graph(struct list_head *etna_operations)
{
   ML_DBG("\n");
   ML_DBG("dumping intermediate graph: %d operations\n", list_length(etna_operations));

   ML_DBG("\n");
   ML_DBG("%3s %-4s %3s %3s  %s\n", "idx", "type", "in", "out", "operation type-specific");
   ML_DBG("================================================================================================\n");
   unsigned i = 0;
   list_for_each_entry(struct etna_operation, operation, etna_operations, link) {
      switch(operation->type) {
      case ETNA_JOB_TYPE_TP:
         ML_DBG("%3d %-4s %3d %3d",
                i, "TP", operation->input_tensors[0], operation->output_tensor);
         break;
      case ETNA_JOB_TYPE_NN:
         ML_DBG("%3d %-4s %3d %3d in2: %3d",
                i, "NN", operation->input_tensors[0], operation->output_tensor, operation->input_tensors[1]);
         break;
      }
      ML_DBG("\n");
      i++;
   }
   ML_DBG("\n");
}

static void
lower_operations(struct etna_ml_subgraph *subgraph,
                 const struct pipe_ml_operation *poperations,
                 unsigned count,
                 struct list_head *etna_operations)
{
   for (unsigned i = 0; i < count; i++) {
      const struct pipe_ml_operation *poperation = &poperations[i];

      switch(poperation->type) {
         case PIPE_ML_OPERATION_TYPE_CONVOLUTION: {
            unsigned input_tensor = poperation->input_tensors[0]->index;

            if (needs_transpose(poperations, count, poperation)) {
               ML_DBG("Adding transpose for convolution operation.\n");
               struct etna_operation *operation = calloc(1, sizeof(*operation));
               etna_ml_lower_transpose(subgraph, poperation, operation, &input_tensor);
               list_addtail(&operation->link, etna_operations);
            }

            if (needs_reshuffle(subgraph, poperation)) {
               ML_DBG("Adding reshuffle for convolution operation.\n");
               struct etna_operation *operation = calloc(1, sizeof(*operation));
               unsigned temp = 0;
               etna_ml_lower_reshuffle(subgraph, poperation, operation, &temp);
               operation->input_tensors[0] = input_tensor;
               input_tensor = temp;
               list_addtail(&operation->link, etna_operations);
            }

            ML_DBG("Adding convolution.\n");
            struct etna_operation *operation = calloc(1, sizeof(*operation));
            etna_ml_lower_convolution(subgraph, poperation, operation);
            operation->input_tensors[0] = input_tensor;
            list_addtail(&operation->link, etna_operations);

            if (needs_detranspose(poperations, count, poperation)) {
               ML_DBG("Adding detranspose for convolution operation.\n");
               struct etna_operation *detranspose = calloc(1, sizeof(*operation));
               etna_ml_lower_detranspose(subgraph, operation, detranspose);
               operation->output_tensor = detranspose->input_tensors[0];
               list_addtail(&detranspose->link, etna_operations);
            }
            break;
         }
         case PIPE_ML_OPERATION_TYPE_ADD: {
            struct etna_operation *operation = calloc(1, sizeof(*operation));
            etna_ml_lower_add(subgraph, poperation, operation);
            list_addtail(&operation->link, etna_operations);

            if (needs_detranspose(poperations, count, poperation)) {
               struct etna_operation *detranspose = calloc(1, sizeof(*operation));
               etna_ml_lower_detranspose(subgraph, operation, detranspose);
               operation->output_tensor = detranspose->input_tensors[0];
               list_addtail(&detranspose->link, etna_operations);
            }
            break;
         }
         default:
            unreachable("Unsupported ML operation type");
      }
   }

   /* Create combined input tensors first */
   list_for_each_entry(struct etna_operation, operation, etna_operations, link) {
      if (operation->input_count == 1)
         continue;

      etna_ml_create_tensor(subgraph, operation->input_tensors[0], operation->input_tensor_size);

      for (int i = 1; i < operation->input_count; i++)
         reference_tensor_with_offset(subgraph,
                                      operation->input_tensors[0],
                                      operation->input_tensors[i],
                                      i * operation->input_tensor_size / operation->input_count,
                                      operation->input_tensor_size / operation->input_count);

   }

   /* Create all other input tensors */
   list_for_each_entry(struct etna_operation, operation, etna_operations, link) {
      if (operation->input_count != 1)
         continue;

      etna_ml_create_tensor(subgraph, operation->input_tensors[0], operation->input_tensor_size);
   }

   /* Create any output tensors that aren't inputs to other operations, these
    * are the outputs of the graph.
    */
   ML_DBG("Ensuring all output tensors have their memory backing.\n");
   list_for_each_entry(struct etna_operation, operation, etna_operations, link) {
      struct pipe_resource *res = etna_ml_get_tensor(subgraph, operation->output_tensor);
      if (res != NULL)
         continue;

      unsigned size = operation->output_width * operation->output_height * operation->output_channels;
      etna_ml_create_tensor(subgraph, operation->output_tensor, size);
   }

   if (DBG_ENABLED(ETNA_DBG_ML_MSGS))
      dump_graph(etna_operations);
}

static unsigned
count_tensors(const struct pipe_ml_operation *poperations,
              unsigned count)
{
   unsigned tensor_count = 0;

   for (unsigned i = 0; i < count; i++) {
      const struct pipe_ml_operation *poperation = &poperations[i];

      for (unsigned j = 0; j < poperation->input_count; j++)
         tensor_count = MAX2(tensor_count, poperation->input_tensors[j]->index);

      for (unsigned j = 0; j < poperation->output_count; j++)
         tensor_count = MAX2(tensor_count, poperation->output_tensors[j]->index);

      switch (poperation->type) {
      case PIPE_ML_OPERATION_TYPE_CONVOLUTION:
         tensor_count = MAX2(tensor_count, poperation->conv.weight_tensor->index);
         tensor_count = MAX2(tensor_count, poperation->conv.bias_tensor->index);
         break;
      case PIPE_ML_OPERATION_TYPE_ADD:
         break;
      default:
         unreachable("Unsupported ML operation type");
      }
   }

   return tensor_count + 1;
}

struct pipe_ml_subgraph *
etna_ml_subgraph_create(struct pipe_context *pcontext,
                        const struct pipe_ml_operation *poperations,
                        unsigned count)
{
   struct etna_context *ctx = etna_context(pcontext);
   unsigned nn_core_count = etna_ml_get_core_info(ctx)->nn_core_count;
   struct etna_ml_subgraph *subgraph;
   struct list_head operations;
   unsigned tensor_count;

   if (nn_core_count < 1) {
      fprintf(stderr, "We need at least 1 NN core to do anything useful.\n");
      abort();
   }

   subgraph = calloc(1, sizeof(*subgraph));
   tensor_count = count_tensors(poperations, count);

   list_inithead(&operations);

   subgraph->base.context = pcontext;
   util_dynarray_init(&subgraph->operations, NULL);

   util_dynarray_init(&subgraph->tensors, NULL);
   if (!util_dynarray_resize(&subgraph->tensors, struct pipe_resource *, tensor_count))
      return NULL;
   memset(util_dynarray_begin(&subgraph->tensors), 0, subgraph->tensors.size);

   util_dynarray_init(&subgraph->offsets, NULL);
   if (!util_dynarray_resize(&subgraph->offsets, unsigned, tensor_count))
      return NULL;
   memset(util_dynarray_begin(&subgraph->offsets), 0, subgraph->offsets.size);

   util_dynarray_init(&subgraph->sizes, NULL);
   if (!util_dynarray_resize(&subgraph->sizes, unsigned, tensor_count))
      return NULL;
   memset(util_dynarray_begin(&subgraph->sizes), 0, subgraph->sizes.size);

   lower_operations(subgraph, poperations, count, &operations);

   list_for_each_entry(struct etna_operation, operation, &operations, link) {
      struct etna_vip_instruction instruction = {0};

      switch(operation->type) {
         case ETNA_JOB_TYPE_NN:
            etna_ml_compile_operation_nn(subgraph, operation, &instruction);
            break;
         case ETNA_JOB_TYPE_TP:
            etna_ml_compile_operation_tp(subgraph, operation, &instruction);
            break;
      }

      util_dynarray_append(&subgraph->operations, struct etna_vip_instruction, instruction);
   }

   list_for_each_entry_safe(struct etna_operation, operation, &operations, link) {
      pipe_resource_reference(&operation->weight_tensor, NULL);
      pipe_resource_reference(&operation->bias_tensor, NULL);
      free(operation);
   }

   return &subgraph->base;
}

static void
dump_buffer(const uint32_t *ptr, unsigned size, char *name, int operation_nr, int suboperation_nr)
{
   char buffer[255];

   snprintf(buffer, sizeof(buffer), "mesa-%s-%03u-%03u.bin", name, operation_nr, suboperation_nr);

   ML_DBG("Dumping buffer from 0x%lx to %s\n", ptr, buffer);

   FILE *f = fopen(buffer, "wb");
   assert(f);
   fwrite(ptr, 1, size, f);
   if(ferror(f)) {
      ML_DBG("Error in writing to file: %s\n", strerror(errno));
   }
   fflush(f);
   fclose(f);
}

static void
dump_bo(struct etna_bo *bo, char *name, int operation_nr, int suboperation_nr)
{
   const uint32_t *map = etna_bo_map(bo);
   dump_buffer(map, etna_bo_size(bo), name, operation_nr, suboperation_nr);
}

static void
init_npu(struct pipe_context *pctx)
{
   struct etna_context *ctx = etna_context(pctx);
   struct etna_cmd_stream *stream = ctx->stream;

   /* These zeroes match the blob's cmdstream. They are here to make diff'ing easier.*/
   etna_cmd_stream_emit(stream, 0x0);
   etna_cmd_stream_emit(stream, 0x0);
   etna_cmd_stream_emit(stream, 0x0);
   etna_cmd_stream_emit(stream, 0x0);
   etna_cmd_stream_emit(stream, 0x0);
   etna_cmd_stream_emit(stream, 0x0);
   etna_cmd_stream_emit(stream, 0x0);
   etna_cmd_stream_emit(stream, 0x0);

   etna_set_state(stream, VIVS_PA_SYSTEM_MODE, VIVS_PA_SYSTEM_MODE_PROVOKING_VERTEX_LAST |
                                               VIVS_PA_SYSTEM_MODE_HALF_PIXEL_CENTER);
   etna_set_state(stream, VIVS_GL_API_MODE, VIVS_GL_API_MODE_OPENCL);

   etna_cmd_stream_emit(stream, 0x0);
   etna_cmd_stream_emit(stream, 0x0);

   pctx->flush(pctx, NULL, 0);
}

static void
close_batch(struct pipe_context *pctx)
{
   struct etna_context *ctx = etna_context(pctx);
   struct etna_cmd_stream *stream = ctx->stream;

   unsigned cache = VIVS_GL_FLUSH_CACHE_DEPTH | VIVS_GL_FLUSH_CACHE_COLOR | VIVS_GL_FLUSH_CACHE_UNK10;
   if (!DBG_ENABLED(ETNA_DBG_NPU_PARALLEL))
      cache |= VIVS_GL_FLUSH_CACHE_UNK11 | VIVS_GL_FLUSH_CACHE_SHADER_L1;

   etna_set_state(stream, VIVS_GL_FLUSH_CACHE, cache);
   etna_set_state(stream, VIVS_GL_FLUSH_CACHE, cache);

   etna_cmd_stream_emit(stream, 0x0);
   etna_cmd_stream_emit(stream, 0x0);

   ctx->dirty = 0;
}

void
etna_ml_subgraph_invoke(struct pipe_context *pctx, struct pipe_ml_subgraph *psubgraph,
                        unsigned inputs_count, unsigned input_idxs[], void *inputs[],
                        bool is_signed[])
{
   struct etna_context *ctx = etna_context(pctx);
   unsigned tp_core_count = etna_ml_get_core_info(ctx)->tp_core_count;
   struct etna_ml_subgraph *subgraph = (struct etna_ml_subgraph *)(psubgraph);
   unsigned *offsets = util_dynarray_begin(&subgraph->offsets);
   unsigned *sizes = util_dynarray_begin(&subgraph->sizes);
   struct etna_cmd_stream *stream = ctx->stream;
   static bool is_initialized = false;

   if (!is_initialized) {
      init_npu(pctx);
      is_initialized = true;
   }

   if (!DBG_ENABLED(ETNA_DBG_NPU_NO_BATCHING)) {
      /* These zeroes match the blob's cmdstream. They are here to make diff'ing easier.*/
      etna_cmd_stream_emit(stream, 0x0);
      etna_cmd_stream_emit(stream, 0x0);
      etna_cmd_stream_emit(stream, 0x0);
      etna_cmd_stream_emit(stream, 0x0);
      etna_cmd_stream_emit(stream, 0x0);
      etna_cmd_stream_emit(stream, 0x0);
      etna_cmd_stream_emit(stream, 0x0);
      etna_cmd_stream_emit(stream, 0x0);
   }

   for (int i = 0; i < inputs_count; i++) {
      struct pipe_resource *res = etna_ml_get_tensor(subgraph, input_idxs[i]);
      if (is_signed[i]) {
         struct pipe_transfer *dst_transfer;
         const uint8_t *src = inputs[i];
         uint8_t *dst_map;
         dst_map = pipe_buffer_map_range(pctx, res, 0, sizes[input_idxs[i]], PIPE_MAP_WRITE, &dst_transfer);
         assert(dst_map);
         for (unsigned k = 0; k < sizes[input_idxs[i]]; k++) {
            dst_map[k] = src[k] + 128;
         }
         pipe_buffer_unmap(pctx, dst_transfer);
      } else {
         pipe_buffer_write(pctx, res, offsets[input_idxs[i]], sizes[input_idxs[i]], inputs[i]);
      }
   }

   unsigned i = 0;
   util_dynarray_foreach(&subgraph->operations, struct etna_vip_instruction, operation) {

      if (DBG_ENABLED(ETNA_DBG_DUMP_SHADERS)) {
         switch (operation->type) {
            case ETNA_JOB_TYPE_TP:
               for (unsigned j = 0; j < tp_core_count && operation->configs[j]; j++) {
                  dump_bo(operation->configs[j], "tp", i, j);
               }
               break;
            case ETNA_JOB_TYPE_NN:
               dump_bo(operation->configs[0], "nn", i, 0);
               dump_bo(operation->coefficients, "compressed", i, 0);
               break;
            default:
               unreachable("Unsupported ML operation type");
         }
      }

      if (DBG_ENABLED(ETNA_DBG_NPU_NO_BATCHING)) {
         /* These zeroes match the blob's cmdstream. They are here to make diff'ing easier.*/
         etna_cmd_stream_emit(stream, 0x0);
         etna_cmd_stream_emit(stream, 0x0);
         etna_cmd_stream_emit(stream, 0x0);
         etna_cmd_stream_emit(stream, 0x0);
         etna_cmd_stream_emit(stream, 0x0);
         etna_cmd_stream_emit(stream, 0x0);
         etna_cmd_stream_emit(stream, 0x0);
         etna_cmd_stream_emit(stream, 0x0);
      }

      for (unsigned j = 0; j < tp_core_count && operation->configs[j]; j++)
         etna_cmd_stream_ref_bo(stream, operation->configs[j], ETNA_RELOC_READ);
      if (operation->coefficients)
         etna_cmd_stream_ref_bo(stream, operation->coefficients, ETNA_RELOC_READ);
      etna_cmd_stream_ref_bo(stream, etna_resource(operation->input)->bo, ETNA_RELOC_READ);
      etna_cmd_stream_ref_bo(stream, etna_resource(operation->output)->bo, ETNA_RELOC_WRITE);

      switch (operation->type) {
         case ETNA_JOB_TYPE_TP:
            etna_ml_emit_operation_tp(subgraph, operation, i);
            break;
         case ETNA_JOB_TYPE_NN:
            etna_ml_emit_operation_nn(subgraph, operation, i);
            break;
         default:
            unreachable("Unsupported ML operation type");
      }

      if (DBG_ENABLED(ETNA_DBG_NPU_NO_BATCHING)) {
         ML_DBG("Running operation %d - %d\n", i, operation->type);
         close_batch(pctx);

         if (DBG_ENABLED(ETNA_DBG_DUMP_SHADERS))
            dump_buffer(ctx->stream->buffer, ctx->stream->offset * 4, "cmd", i, 0);

         pctx->flush(pctx, NULL, 0);

         if (DBG_ENABLED(ETNA_DBG_DUMP_SHADERS)) {
            struct pipe_transfer *transfer = NULL;

            pipe_buffer_map(pctx, operation->input, PIPE_MAP_READ, &transfer);
            dump_bo(etna_resource(operation->input)->bo, "input", i, 0);
            pipe_buffer_unmap(pctx, transfer);

            pipe_buffer_map(pctx, operation->output, PIPE_MAP_READ, &transfer);
            dump_bo(etna_resource(operation->output)->bo, "output", i, 0);
            pipe_buffer_unmap(pctx, transfer);
         }

         stream = ctx->stream;
      }

      i++;
   }

   if (!DBG_ENABLED(ETNA_DBG_NPU_NO_BATCHING))
      close_batch(pctx);

   if (DBG_ENABLED(ETNA_DBG_FLUSH_ALL))
      pctx->flush(pctx, NULL, 0);
}

void
etna_ml_subgraph_read_outputs(struct pipe_context *context, struct pipe_ml_subgraph *psubgraph,
                              unsigned outputs_count, unsigned output_idxs[], void *outputs[],
                              bool is_signed[])
{
   struct etna_ml_subgraph *subgraph = (struct etna_ml_subgraph *)(psubgraph);
   unsigned operation_count = util_dynarray_num_elements(&subgraph->operations, struct etna_vip_instruction);
   struct etna_vip_instruction *last_operation;

   last_operation = util_dynarray_element(&subgraph->operations,
                                          struct etna_vip_instruction,
                                          operation_count - 1);

   if (DBG_ENABLED(ETNA_DBG_ML_MSGS)) {
      long start, end;
      struct timespec time;

      clock_gettime(CLOCK_MONOTONIC, &time);
      start = (long)time.tv_sec * 1000 + (long)time.tv_nsec / 1000000;

      context->flush(context, NULL, 0);

      struct pipe_transfer *transfer = NULL;
      pipe_buffer_map(context, last_operation->output, PIPE_MAP_READ, &transfer);
      pipe_buffer_unmap(context, transfer);

      clock_gettime(CLOCK_MONOTONIC, &time);
      end = (long)time.tv_sec * 1000 + (long)time.tv_nsec / 1000000;
      ML_DBG("Running the NN job took %ld ms.\n", (end - start));
   } else
      context->flush(context, NULL, 0);

   for (int i = 0; i < outputs_count; i++) {
      struct pipe_resource *res = etna_ml_get_tensor(subgraph, output_idxs[i]);
      if (is_signed[i]) {
         struct pipe_transfer *src_transfer;
         uint8_t *src_map;
         src_map = (uint8_t *) pipe_buffer_map_range(context,
                                                     res,
                                                     0, pipe_buffer_size(res),
                                                     PIPE_MAP_READ,
                                                     &src_transfer);
         assert(src_map);
         for (unsigned k = 0; k < pipe_buffer_size(res); k++) {
            ((uint8_t *)(outputs[i]))[k] = src_map[k] - 128;
         }
         pipe_buffer_unmap(context, src_transfer);
      } else {
         pipe_buffer_read(context, res, 0, pipe_buffer_size(res), outputs[i]);
      }
   }
}

void
etna_ml_subgraph_destroy(struct pipe_context *context, struct pipe_ml_subgraph *psubgraph)
{
   struct etna_ml_subgraph *subgraph = (struct etna_ml_subgraph *)(psubgraph);

   util_dynarray_foreach(&subgraph->operations, struct etna_vip_instruction, operation) {
      for (unsigned j = 0; j < MAX_CONFIG_BOS && operation->configs[j]; j++)
         etna_bo_del(operation->configs[j]);
      etna_bo_del(operation->coefficients);
      pipe_resource_reference(&operation->input, NULL);
      pipe_resource_reference(&operation->output, NULL);
   }
   util_dynarray_fini(&subgraph->operations);

   util_dynarray_foreach(&subgraph->tensors, struct pipe_resource *, tensor) {
      pipe_resource_reference(tensor, NULL);
   }
   util_dynarray_fini(&subgraph->tensors);
   util_dynarray_fini(&subgraph->offsets);
   util_dynarray_fini(&subgraph->sizes);

   free(subgraph);
}
