/*
 * Copyright 2025 LunarG, Inc.
 * Copyright 2025 Google LLC
 * SPDX-License-Identifier: MIT
 */

/* This file primarily concerns itself with mapping from the NIR (and Vulkan)
 * model of I/O to the Metal one. */

#include "msl_private.h"

#include "nir_builder.h"

/* Mapping from alu type to Metal scalar type */
static const char *
alu_type_to_string(nir_alu_type type)
{
   switch (type) {
   case nir_type_uint8:
      return "uchar";
   case nir_type_uint16:
      return "ushort";
   case nir_type_uint32:
      return "uint";
   case nir_type_uint64:
      return "ulong";
   case nir_type_int8:
      return "char";
   case nir_type_int16:
      return "short";
   case nir_type_int32:
      return "int";
   case nir_type_int64:
      return "long";
   case nir_type_float16:
      return "half";
   case nir_type_float32:
      return "float";
   case nir_type_bool8:
      return "bool";
   default:
      UNREACHABLE("Unsupported nir_alu_type");
   }
};

/* Type suffix for a vector of a given size. */
static const char *vector_suffixes[] = {
   [1] = "",
   [2] = "2",
   [3] = "3",
   [4] = "4",
};

/* The type names of the generated output structs */
static const char *VERTEX_OUTPUT_TYPE = "VertexOut";
static const char *FRAGMENT_OUTPUT_TYPE = "FragmentOut";

/* Mapping from NIR's varying slots to the generated struct member name */
static const char *VARYING_SLOT_NAME[NUM_TOTAL_VARYING_SLOTS] = {
   [VARYING_SLOT_POS] = "position",
   [VARYING_SLOT_PSIZ] = "point_size",
   [VARYING_SLOT_PRIMITIVE_ID] = "primitive_id",
   [VARYING_SLOT_LAYER] = "layer",
   [VARYING_SLOT_VAR0] = "vary_00",
   [VARYING_SLOT_VAR1] = "vary_01",
   [VARYING_SLOT_VAR2] = "vary_02",
   [VARYING_SLOT_VAR3] = "vary_03",
   [VARYING_SLOT_VAR4] = "vary_04",
   [VARYING_SLOT_VAR5] = "vary_05",
   [VARYING_SLOT_VAR6] = "vary_06",
   [VARYING_SLOT_VAR7] = "vary_07",
   [VARYING_SLOT_VAR8] = "vary_08",
   [VARYING_SLOT_VAR9] = "vary_09",
   [VARYING_SLOT_VAR10] = "vary_10",
   [VARYING_SLOT_VAR11] = "vary_11",
   [VARYING_SLOT_VAR12] = "vary_12",
   [VARYING_SLOT_VAR13] = "vary_13",
   [VARYING_SLOT_VAR14] = "vary_14",
   [VARYING_SLOT_VAR15] = "vary_15",
   [VARYING_SLOT_VAR16] = "vary_16",
   [VARYING_SLOT_VAR17] = "vary_17",
   [VARYING_SLOT_VAR18] = "vary_18",
   [VARYING_SLOT_VAR19] = "vary_19",
   [VARYING_SLOT_VAR20] = "vary_20",
   [VARYING_SLOT_VAR21] = "vary_21",
   [VARYING_SLOT_VAR22] = "vary_22",
   [VARYING_SLOT_VAR23] = "vary_23",
   [VARYING_SLOT_VAR24] = "vary_24",
   [VARYING_SLOT_VAR25] = "vary_25",
   [VARYING_SLOT_VAR26] = "vary_26",
   [VARYING_SLOT_VAR27] = "vary_27",
   [VARYING_SLOT_VAR28] = "vary_28",
   [VARYING_SLOT_VAR29] = "vary_29",
   [VARYING_SLOT_VAR30] = "vary_30",
   [VARYING_SLOT_VAR31] = "vary_31",
};

/* Mapping from NIR varying slot to the MSL struct member attribute. */
static const char *VARYING_SLOT_SEMANTIC[NUM_TOTAL_VARYING_SLOTS] = {
   [VARYING_SLOT_POS] = "[[position]]",
   [VARYING_SLOT_PSIZ] = "[[point_size]]",
   [VARYING_SLOT_PRIMITIVE_ID] = "[[primitive_id]]",
   [VARYING_SLOT_LAYER] = "[[render_target_array_index]]",
   [VARYING_SLOT_VAR0] = "[[user(vary_00)]]",
   [VARYING_SLOT_VAR1] = "[[user(vary_01)]]",
   [VARYING_SLOT_VAR2] = "[[user(vary_02)]]",
   [VARYING_SLOT_VAR3] = "[[user(vary_03)]]",
   [VARYING_SLOT_VAR4] = "[[user(vary_04)]]",
   [VARYING_SLOT_VAR5] = "[[user(vary_05)]]",
   [VARYING_SLOT_VAR6] = "[[user(vary_06)]]",
   [VARYING_SLOT_VAR7] = "[[user(vary_07)]]",
   [VARYING_SLOT_VAR8] = "[[user(vary_08)]]",
   [VARYING_SLOT_VAR9] = "[[user(vary_09)]]",
   [VARYING_SLOT_VAR10] = "[[user(vary_10)]]",
   [VARYING_SLOT_VAR11] = "[[user(vary_11)]]",
   [VARYING_SLOT_VAR12] = "[[user(vary_12)]]",
   [VARYING_SLOT_VAR13] = "[[user(vary_13)]]",
   [VARYING_SLOT_VAR14] = "[[user(vary_14)]]",
   [VARYING_SLOT_VAR15] = "[[user(vary_15)]]",
   [VARYING_SLOT_VAR16] = "[[user(vary_16)]]",
   [VARYING_SLOT_VAR17] = "[[user(vary_17)]]",
   [VARYING_SLOT_VAR18] = "[[user(vary_18)]]",
   [VARYING_SLOT_VAR19] = "[[user(vary_19)]]",
   [VARYING_SLOT_VAR20] = "[[user(vary_20)]]",
   [VARYING_SLOT_VAR21] = "[[user(vary_21)]]",
   [VARYING_SLOT_VAR22] = "[[user(vary_22)]]",
   [VARYING_SLOT_VAR23] = "[[user(vary_23)]]",
   [VARYING_SLOT_VAR24] = "[[user(vary_24)]]",
   [VARYING_SLOT_VAR25] = "[[user(vary_25)]]",
   [VARYING_SLOT_VAR26] = "[[user(vary_26)]]",
   [VARYING_SLOT_VAR27] = "[[user(vary_27)]]",
   [VARYING_SLOT_VAR28] = "[[user(vary_28)]]",
   [VARYING_SLOT_VAR29] = "[[user(vary_29)]]",
   [VARYING_SLOT_VAR30] = "[[user(vary_30)]]",
   [VARYING_SLOT_VAR31] = "[[user(vary_31)]]",
};

/* Mapping from NIR fragment output slot to MSL struct member name */
static const char *FS_OUTPUT_NAME[] = {
   [FRAG_RESULT_DEPTH] = "depth_out",
   [FRAG_RESULT_STENCIL] = "stencil_out",
   [FRAG_RESULT_SAMPLE_MASK] = "sample_mask_out",
   [FRAG_RESULT_DATA0] = "color_0",
   [FRAG_RESULT_DATA1] = "color_1",
   [FRAG_RESULT_DATA2] = "color_2",
   [FRAG_RESULT_DATA3] = "color_3",
   [FRAG_RESULT_DATA4] = "color_4",
   [FRAG_RESULT_DATA5] = "color_5",
   [FRAG_RESULT_DATA6] = "color_6",
   [FRAG_RESULT_DATA7] = "color_7",
};

/* Mapping from NIR fragment output slot to MSL struct member attribute */
static const char *FS_OUTPUT_SEMANTIC[] = {
   [FRAG_RESULT_DEPTH] = "", // special case, depends on depth layout
   [FRAG_RESULT_STENCIL] = "stencil", [FRAG_RESULT_SAMPLE_MASK] = "sample_mask",
   [FRAG_RESULT_DATA0] = "color(0)",  [FRAG_RESULT_DATA1] = "color(1)",
   [FRAG_RESULT_DATA2] = "color(2)",  [FRAG_RESULT_DATA3] = "color(3)",
   [FRAG_RESULT_DATA4] = "color(4)",  [FRAG_RESULT_DATA5] = "color(5)",
   [FRAG_RESULT_DATA6] = "color(6)",  [FRAG_RESULT_DATA7] = "color(7)",
};

const char *depth_layout_arg[8] = {
   [FRAG_DEPTH_LAYOUT_ANY] = "any",
   [FRAG_DEPTH_LAYOUT_GREATER] = "greater",
   [FRAG_DEPTH_LAYOUT_LESS] = "less",
   [FRAG_DEPTH_LAYOUT_UNCHANGED] = "any",
};

/* Generate the struct definition for the vertex shader return value */
static void
vs_output_block(nir_shader *shader, struct nir_to_msl_ctx *ctx)
{
   P(ctx, "struct %s {\n", VERTEX_OUTPUT_TYPE);
   ctx->indentlevel++;
   u_foreach_bit64(location, shader->info.outputs_written) {
      struct io_slot_info info = ctx->outputs_info[location];
      const char *type = alu_type_to_string(info.type);
      const char *vector_suffix = vector_suffixes[info.num_components];
      P_IND(ctx, "%s%s %s %s;\n", type, vector_suffix,
            VARYING_SLOT_NAME[location], VARYING_SLOT_SEMANTIC[location]);
   }

   ctx->indentlevel--;
   P(ctx, "};\n");
}

/* Generate the struct definition for the fragment shader input argument */
static void
fs_input_block(nir_shader *shader, struct nir_to_msl_ctx *ctx)
{
   P(ctx, "struct FragmentIn {\n");
   ctx->indentlevel++;
   u_foreach_bit64(location, shader->info.inputs_read) {
      struct io_slot_info info = ctx->inputs_info[location];
      const char *type = alu_type_to_string(info.type);
      const char *vector_suffix = vector_suffixes[info.num_components];
      const char *interp = "";
      switch (info.interpolation) {
      case INTERP_MODE_NOPERSPECTIVE:
         if (info.centroid)
            interp = "[[centroid_no_perspective]]";
         else if (info.sample)
            interp = "[[sample_no_perspective]]";
         else
            interp = "[[center_no_perspective]]";
         break;
      case INTERP_MODE_FLAT:
         interp = "[[flat]]";
         break;
      default:
         if (info.centroid)
            interp = "[[centroid_perspective]]";
         else if (info.sample)
            interp = "[[sample_perspective]]";
         break;
      }
      P_IND(ctx, "%s%s %s %s %s;\n", type, vector_suffix,
            VARYING_SLOT_NAME[location], VARYING_SLOT_SEMANTIC[location],
            interp);
   }

   /* Enable reading from framebuffer */
   u_foreach_bit64(location, shader->info.outputs_read) {
      struct io_slot_info info = ctx->outputs_info[location];
      const char *type = alu_type_to_string(info.type);
      const char *vector_suffix = vector_suffixes[info.num_components];
      P_IND(ctx, "%s%s ", type, vector_suffix);
      P(ctx, "%s [[%s, raster_order_group(0)]];\n", FS_OUTPUT_NAME[location],
        FS_OUTPUT_SEMANTIC[location]);
   }

   ctx->indentlevel--;
   P(ctx, "};\n");
}

/* Generate the struct definition for the fragment shader return value */
static void
fs_output_block(nir_shader *shader, struct nir_to_msl_ctx *ctx)
{
   P_IND(ctx, "struct %s {\n", FRAGMENT_OUTPUT_TYPE);
   ctx->indentlevel++;
   u_foreach_bit64(location, shader->info.outputs_written) {
      struct io_slot_info info = ctx->outputs_info[location];
      const char *type = alu_type_to_string(info.type);
      const char *vector_suffix = vector_suffixes[info.num_components];
      P_IND(ctx, "%s%s ", type, vector_suffix);
      if (location == FRAG_RESULT_DEPTH) {
         enum gl_frag_depth_layout depth_layout = shader->info.fs.depth_layout;
         assert(depth_layout_arg[depth_layout]);
         P(ctx, "%s [[depth(%s)]];\n", FS_OUTPUT_NAME[location],
           depth_layout_arg[depth_layout]);
      } else {
         P(ctx, "%s [[%s]];\n", FS_OUTPUT_NAME[location],
           FS_OUTPUT_SEMANTIC[location]);
      }
   }
   ctx->indentlevel--;
   P_IND(ctx, "};\n")
}

struct gather_ctx {
   struct io_slot_info *input;
   struct io_slot_info *output;
};

static bool
msl_nir_gather_io_info(nir_builder *b, nir_intrinsic_instr *intrin, void *data)
{
   struct gather_ctx *ctx = (struct gather_ctx *)data;
   switch (intrin->intrinsic) {
   case nir_intrinsic_load_interpolated_input: {
      unsigned component = nir_intrinsic_component(intrin);
      struct nir_io_semantics io = nir_intrinsic_io_semantics(intrin);
      assert(io.num_slots == 1u && "We don't support arrays");

      unsigned location = nir_src_as_uint(intrin->src[1u]) + io.location;
      ctx->input[location].type = nir_intrinsic_dest_type(intrin);
      ctx->input[location].num_components =
         MAX2(ctx->input[location].num_components,
              intrin->num_components + component);
      assert(ctx->input[location].num_components <= 4u &&
             "Cannot have more than a vec4");

      nir_intrinsic_instr *interp_intrin =
         nir_src_as_intrinsic(intrin->src[0u]);
      ctx->input[location].interpolation =
         nir_intrinsic_interp_mode(interp_intrin);
      ctx->input[location].centroid =
         interp_intrin->intrinsic == nir_intrinsic_load_barycentric_centroid;
      ctx->input[location].sample =
         interp_intrin->intrinsic == nir_intrinsic_load_barycentric_sample;
      break;
   }
   case nir_intrinsic_load_input: {
      unsigned component = nir_intrinsic_component(intrin);
      struct nir_io_semantics io = nir_intrinsic_io_semantics(intrin);
      assert(io.num_slots == 1u && "We don't support arrays");

      unsigned location = nir_src_as_uint(intrin->src[0u]) + io.location;
      ctx->input[location].type = nir_intrinsic_dest_type(intrin);
      ctx->input[location].interpolation = INTERP_MODE_FLAT;
      ctx->input[location].num_components =
         MAX2(ctx->input[location].num_components,
              intrin->num_components + component);
      assert(ctx->input[location].num_components <= 4u &&
             "Cannot have more than a vec4");
      break;
   }
   case nir_intrinsic_load_output: {
      unsigned component = nir_intrinsic_component(intrin);
      struct nir_io_semantics io = nir_intrinsic_io_semantics(intrin);
      assert(io.num_slots == 1u && "We don't support arrays");

      unsigned location = nir_src_as_uint(intrin->src[0u]) + io.location;
      ctx->output[location].type = nir_intrinsic_dest_type(intrin);
      ctx->output[location].num_components =
         MAX2(ctx->output[location].num_components,
              intrin->num_components + component);
      assert(ctx->output[location].num_components <= 4u &&
             "Cannot have more than a vec4");
      break;
   }
   case nir_intrinsic_store_output: {
      unsigned component = nir_intrinsic_component(intrin);
      unsigned write_mask = nir_intrinsic_write_mask(intrin);
      struct nir_io_semantics io = nir_intrinsic_io_semantics(intrin);
      assert(io.num_slots == 1u && "We don't support arrays");

      /* Due to nir_lower_blend that doesn't generate intrinsics with the same
       * num_components as destination, we need to compute current store's
       * num_components using offset and mask. */
      unsigned num_components = component + 1u;
      unsigned mask_left_most_index = 0u;
      for (unsigned i = 0u; i < intrin->num_components; ++i) {
         if ((write_mask >> i) & 1u)
            mask_left_most_index = i;
      }
      num_components += mask_left_most_index;
      unsigned location = nir_src_as_uint(intrin->src[1u]) + io.location;
      ctx->output[location].type = nir_intrinsic_src_type(intrin);
      ctx->output[location].num_components =
         MAX3(ctx->output[location].num_components, num_components,
              intrin->num_components);
      assert(ctx->output[location].num_components <= 4u &&
             "Cannot have more than a vec4");
      break;
   }
   default:
      break;
   }

   return false;
}

void
msl_gather_io_info(struct nir_to_msl_ctx *ctx,
                   struct io_slot_info *info_array_input,
                   struct io_slot_info *info_array_output)
{
   struct gather_ctx gather_ctx = {
      .input = info_array_input,
      .output = info_array_output,
   };
   nir_shader_intrinsics_pass(ctx->shader, msl_nir_gather_io_info,
                              nir_metadata_all, &gather_ctx);
}

/* Generate all the struct definitions needed for shader I/O */
void
msl_emit_io_blocks(struct nir_to_msl_ctx *ctx, nir_shader *shader)
{
   switch (ctx->shader->info.stage) {
   case MESA_SHADER_VERTEX:
      vs_output_block(shader, ctx);
      break;
   case MESA_SHADER_FRAGMENT:
      fs_input_block(shader, ctx);
      fs_output_block(shader, ctx);
      break;
   case MESA_SHADER_COMPUTE:
      break;
   default:
      assert(0);
   }
   // TODO_KOSMICKRISP This should not exist. We need to create input structs in
   // nir that will later be translated
   P(ctx, "struct Buffer {\n");
   ctx->indentlevel++;
   P_IND(ctx, "uint64_t contents[1];\n"); // TODO_KOSMICKRISP This should not be
                                          // a cpu pointer
   ctx->indentlevel--;
   P(ctx, "};\n")

   P(ctx, "struct SamplerTable {\n");
   ctx->indentlevel++;
   P_IND(ctx, "sampler handles[1024];\n");
   ctx->indentlevel--;
   P(ctx, "};\n")
}

void
msl_emit_output_var(struct nir_to_msl_ctx *ctx, nir_shader *shader)
{
   switch (shader->info.stage) {
   case MESA_SHADER_VERTEX:
      P_IND(ctx, "%s out = {};\n", VERTEX_OUTPUT_TYPE);
      break;
   case MESA_SHADER_FRAGMENT:
      P_IND(ctx, "%s out = {};\n", FRAGMENT_OUTPUT_TYPE);

      /* Load inputs to output */
      u_foreach_bit64(location, shader->info.outputs_read) {
         P_IND(ctx, "out.%s = in.%s;\n", FS_OUTPUT_NAME[location],
               FS_OUTPUT_NAME[location]);
      }
      break;
   default:
      break;
   }
}

const char *
msl_output_name(struct nir_to_msl_ctx *ctx, unsigned location)
{
   switch (ctx->shader->info.stage) {
   case MESA_SHADER_VERTEX:
      return VARYING_SLOT_NAME[location];
   case MESA_SHADER_FRAGMENT:
      return FS_OUTPUT_NAME[location];
   default:
      assert(0);
      return "";
   }
}

const char *
msl_input_name(struct nir_to_msl_ctx *ctx, unsigned location)
{
   switch (ctx->shader->info.stage) {
   case MESA_SHADER_FRAGMENT:
      return VARYING_SLOT_NAME[location];
   default:
      assert(0);
      return "";
   }
}
