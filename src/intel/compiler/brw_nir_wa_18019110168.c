/*
 * Copyright (c) 2022-2025 Intel Corporation
 * SPDX-License-Identifier: MIT
 */

#include "brw_nir.h"

/*
 * Wa_18019110168 for gfx 12.5.
 *
 * This file implements workaround for HW bug, which leads to fragment shader
 * reading incorrect per-primitive data if mesh shader, in addition to writing
 * per-primitive data, also writes to gl_ClipDistance.
 *
 * The suggested solution to that bug is to not use per-primitive data by:
 * - creating new vertices for provoking vertices shared by multiple primitives
 * - converting per-primitive attributes read by fragment shader to flat
 *   per-vertex attributes for the provoking vertex
 * - modifying fragment shader to read those per-vertex attributes
 *
 * There are at least 2 type of failures not handled very well:
 * - if the number of varying slots overflows, than only some attributes will
 *   be converted, leading to corruption of those unconverted attributes
 * - if the overall MUE size is so large it doesn't fit in URB, then URB
 *   allocation will fail in some way; unfortunately there's no good way to
 *   say how big MUE will be at this moment and back out
 */
static bool
copy_primitive_count_write(nir_builder *b,
                           nir_intrinsic_instr *intrin,
                           void *data)
{
   if (intrin->intrinsic != nir_intrinsic_set_vertex_and_primitive_count)
      return false;

   b->cursor = nir_after_instr(&intrin->instr);

   nir_variable *primitive_count = (nir_variable *)data;
   nir_store_var(b, primitive_count, intrin->src[1].ssa, 0x1);

   return true;
}

static nir_variable *
copy_primitive_count_writes(nir_shader *nir)
{
   nir_variable *primitive_count =
      nir_local_variable_create(nir_shader_get_entrypoint(nir),
                                glsl_uint_type(),
                                "Wa_18019110168_primitive_count");

   nir_shader_intrinsics_pass(nir,
                              copy_primitive_count_write,
                              nir_metadata_control_flow,
                              primitive_count);

   return primitive_count;
}

struct mapping {
   nir_variable    *temp_var;
   nir_deref_instr *per_prim_deref;
   nir_deref_instr *per_vert_deref;
};

static bool
rewrite_derefs_to_per_prim_vars(nir_builder *b,
                                nir_intrinsic_instr *intrin,
                                void *data)
{
   if (intrin->intrinsic != nir_intrinsic_load_deref &&
       intrin->intrinsic != nir_intrinsic_store_deref)
      return false;

   nir_deref_instr *old_deref =
      nir_instr_as_deref(intrin->src[0].ssa->parent_instr);
   nir_variable *var = nir_deref_instr_get_variable(old_deref);
   if (var == NULL)
      return false;

   struct mapping *mapping = data;
   if (mapping[var->data.location].temp_var == NULL)
      return false;

   b->cursor = nir_before_instr(&intrin->instr);

   nir_deref_instr *new_deref = nir_clone_deref_instr(
      b, mapping[var->data.location].temp_var, old_deref);

   nir_src_rewrite(&intrin->src[0], &new_deref->def);
   return true;
}

static bool
mesh_convert_attrs_prim_to_vert(struct nir_shader *nir,
                                struct brw_compile_mesh_params *params,
                                int *wa_mapping)
{
   const uint64_t outputs_written = nir->info.outputs_written;
   const uint64_t per_primitive_outputs =
      nir->info.per_primitive_outputs &
      ~BITFIELD64_BIT(VARYING_SLOT_PRIMITIVE_INDICES);
   const uint64_t other_outputs = outputs_written & ~per_primitive_outputs;

   uint64_t all_outputs = outputs_written;

   const uint64_t remapped_outputs = outputs_written &
      nir->info.per_primitive_outputs &
      ~(BITFIELD64_BIT(VARYING_SLOT_CULL_PRIMITIVE) |
        BITFIELD64_BIT(VARYING_SLOT_PRIMITIVE_INDICES) |
        BITFIELD64_BIT(VARYING_SLOT_PRIMITIVE_COUNT) |
        BITFIELD64_BIT(VARYING_SLOT_LAYER) |
        BITFIELD64_BIT(VARYING_SLOT_VIEWPORT) |
        BITFIELD64_BIT(VARYING_SLOT_PRIMITIVE_SHADING_RATE));

   /* indexed by slot of per-prim attribute */
   struct mapping mapping[VARYING_SLOT_MAX] = { {NULL, NULL, NULL}, };

   /* Figure out the mapping between per-primitive and new per-vertex outputs. */
   nir_function_impl *impl = nir_shader_get_entrypoint(nir);
   nir_foreach_shader_out_variable(var, nir) {
      int location = var->data.location;

      if (!(BITFIELD64_BIT(location) & remapped_outputs))
         continue;

      assert(location == VARYING_SLOT_PRIMITIVE_ID ||
             location >= VARYING_SLOT_VAR0);

      const struct glsl_type *type = var->type;
      if (nir_is_arrayed_io(var, MESA_SHADER_MESH)) {
         assert(glsl_type_is_array(type));
         type = glsl_get_array_element(type);
      }

      unsigned num_slots = glsl_count_attribute_slots(type, false);

      for (gl_varying_slot slot = VARYING_SLOT_VAR0; slot <= VARYING_SLOT_VAR31; slot++) {
         uint64_t mask = BITFIELD64_MASK(num_slots) << slot;
         if ((all_outputs & mask) == 0) {
            wa_mapping[location] = slot;
            all_outputs |= mask;
            break;
         }
      }

      if (wa_mapping[location] == 0) {
         fprintf(stderr, "Not enough space for hardware per-primitive data corruption work around.\n");
         return false;
      }

      mapping[location].temp_var =
         nir_local_variable_create(impl,
                                   glsl_array_type(type,
                                                   nir->info.mesh.max_primitives_out,
                                                   glsl_get_std140_size(type,  false)),
                                   var->name);
   }

   /* Rewrite all the per-primitive variable reads/writes to the temporary
    * variables.
    */
   NIR_PASS(_, nir, nir_shader_intrinsics_pass,
            rewrite_derefs_to_per_prim_vars,
            nir_metadata_control_flow, mapping);

   void *mem_ctx = ralloc_context(NULL);

   unsigned vertices_per_primitive =
      mesa_vertices_per_prim(nir->info.mesh.primitive_type);

   nir_variable *primitive_count_var = copy_primitive_count_writes(nir);

   nir_builder _b = nir_builder_at(nir_after_impl(impl)), *b = &_b;

   /* wait for all subgroups to finish */
   nir_barrier(b, SCOPE_WORKGROUP);

   /* Build a list of per-vertex variables we might need to copy */
   unsigned num_other_variables = 0;
   nir_foreach_shader_out_variable(var, nir) {
      if ((BITFIELD64_BIT(var->data.location) & other_outputs) == 0)
         continue;
      num_other_variables++;
   }

   nir_variable *primitive_indices_var = NULL;
   nir_deref_instr **per_vertex_derefs =
      ralloc_array(mem_ctx, nir_deref_instr *, num_other_variables);

   unsigned num_per_vertex_variables = 0;
   unsigned processed = 0;
   nir_foreach_shader_out_variable(var, nir) {
      if ((BITFIELD64_BIT(var->data.location) & other_outputs) == 0)
         continue;

      switch (var->data.location) {
      case VARYING_SLOT_PRIMITIVE_COUNT:
         break;
      case VARYING_SLOT_PRIMITIVE_INDICES:
         primitive_indices_var = var;
         break;
      default: {
         const struct glsl_type *type = var->type;
         assert(glsl_type_is_array(type));
         const struct glsl_type *array_element_type =
            glsl_get_array_element(type);

         /* Resize type of array output to make space for one extra vertex
          * attribute for each primitive, so we ensure that the provoking
          * vertex is not shared between primitives.
          */
         const struct glsl_type *new_type =
            glsl_array_type(array_element_type,
                            glsl_get_length(type) +
                            nir->info.mesh.max_primitives_out,
                            0);

         var->type = new_type;

         per_vertex_derefs[num_per_vertex_variables++] =
            nir_build_deref_var(b, var);
         break;
      }
      }

      ++processed;
   }
   assert(processed == num_other_variables);

   nir_def *zero = nir_imm_int(b, 0);

   nir_def *provoking_vertex =
      params->load_provoking_vertex(b, params->load_provoking_vertex_data);
   nir_def *local_invocation_index = nir_load_local_invocation_index(b);

   nir_def *cmp = nir_ieq(b, local_invocation_index, zero);
   nir_if *if_stmt = nir_push_if(b, cmp);
   {
      assert(primitive_count_var != NULL);
      assert(primitive_indices_var != NULL);

      /* Update types of derefs to match type of variables they (de)reference. */
      nir_foreach_function_impl(impl, nir) {
         nir_foreach_block(block, impl) {
            nir_foreach_instr(instr, block) {
               if (instr->type != nir_instr_type_deref)
                  continue;

               nir_deref_instr *deref = nir_instr_as_deref(instr);
               if (deref->deref_type != nir_deref_type_var)
                  continue;

               if (deref->var->type != deref->type)
                  deref->type = deref->var->type;
            }
         }
      }

      /* Create new per-vertex output variables mirroring per-primitive variables
       * and create derefs for both old and new variables.
       */
      nir_foreach_shader_out_variable(var, nir) {
         gl_varying_slot location = var->data.location;

         if ((BITFIELD64_BIT(location) & remapped_outputs) == 0)
            continue;

         const struct glsl_type *type = var->type;
         assert(glsl_type_is_array(type));
         const struct glsl_type *array_element_type = glsl_get_array_element(type);

         const struct glsl_type *new_type =
               glsl_array_type(array_element_type,
                               nir->info.mesh.max_vertices_out +
                               nir->info.mesh.max_primitives_out,
                               0);

         nir_variable *new_var =
            nir_variable_create(nir, nir_var_shader_out, new_type, var->name);
         assert(wa_mapping[location] >= VARYING_SLOT_VAR0);
         assert(wa_mapping[location] <= VARYING_SLOT_VAR31);
         new_var->data.location = wa_mapping[location];
         new_var->data.interpolation = INTERP_MODE_FLAT;

         mapping[location].per_vert_deref = nir_build_deref_var(b, new_var);
         mapping[location].per_prim_deref = nir_build_deref_var(b, mapping[location].temp_var);
      }

      nir_def *trueconst = nir_imm_true(b);

      /*
       * for each Primitive (0 : primitiveCount)
       *    if VertexUsed[PrimitiveIndices[Primitive][provoking vertex]]
       *       create 1 new vertex at offset "Vertex"
       *       copy per vert attributes of provoking vertex to the new one
       *       update PrimitiveIndices[Primitive][provoking vertex]
       *       Vertex++
       *    else
       *       VertexUsed[PrimitiveIndices[Primitive][provoking vertex]] := true
       *
       *    for each attribute : mapping
       *       copy per_prim_attr(Primitive) to per_vert_attr[Primitive][provoking vertex]
       */

      /* primitive count */
      nir_def *primitive_count = nir_load_var(b, primitive_count_var);

      /* primitive index */
      nir_variable *primitive_var =
         nir_local_variable_create(impl, glsl_uint_type(), "Primitive");
      nir_deref_instr *primitive_deref = nir_build_deref_var(b, primitive_var);
      nir_store_deref(b, primitive_deref, zero, 1);

      /* vertex index */
      nir_variable *vertex_var =
         nir_local_variable_create(impl, glsl_uint_type(), "Vertex");
      nir_deref_instr *vertex_deref = nir_build_deref_var(b, vertex_var);
      nir_store_deref(b, vertex_deref, nir_imm_int(b, nir->info.mesh.max_vertices_out), 1);

      /* used vertices bitvector */
      const struct glsl_type *used_vertex_type =
         glsl_array_type(glsl_bool_type(),
                         nir->info.mesh.max_vertices_out,
                         0);
      nir_variable *used_vertex_var =
         nir_local_variable_create(impl, used_vertex_type, "VertexUsed");
      nir_deref_instr *used_vertex_deref =
         nir_build_deref_var(b, used_vertex_var);
      /* Initialize it as "not used" */
      for (unsigned i = 0; i < nir->info.mesh.max_vertices_out; ++i) {
         nir_deref_instr *indexed_used_vertex_deref =
            nir_build_deref_array(b, used_vertex_deref, nir_imm_int(b, i));
         nir_store_deref(b, indexed_used_vertex_deref, nir_imm_false(b), 1);
      }

      nir_loop *loop = nir_push_loop(b);
      {
         nir_def *primitive_id = nir_load_deref(b, primitive_deref);
         nir_def *cmp = nir_ige(b, primitive_id, primitive_count);

         nir_push_if(b, cmp);
         {
            nir_jump(b, nir_jump_break);
         }
         nir_pop_if(b, NULL);

         nir_deref_instr *primitive_indices_deref =
            nir_build_deref_var(b, primitive_indices_var);
         nir_deref_instr *indexed_primitive_indices_deref;
         nir_def *src_vertex;
         nir_def *prim_indices;

         /* array of vectors, we have to extract index out of array deref */
         indexed_primitive_indices_deref =
            nir_build_deref_array(b, primitive_indices_deref, primitive_id);
         prim_indices = nir_load_deref(b, indexed_primitive_indices_deref);
         src_vertex = nir_vector_extract(b, prim_indices, provoking_vertex);

         nir_def *dst_vertex = nir_load_deref(b, vertex_deref);

         nir_deref_instr *indexed_used_vertex_deref =
            nir_build_deref_array(b, used_vertex_deref, src_vertex);
         nir_def *used_vertex = nir_load_deref(b, indexed_used_vertex_deref);

         nir_push_if(b, used_vertex);
         {
            /* If the vertex is used by another primitive, we need to make an
             * entire copy of the per-vertex variables.
             */
            for (unsigned a = 0; a < num_per_vertex_variables; ++a) {
               nir_deref_instr *attr_arr = per_vertex_derefs[a];
               nir_deref_instr *src = nir_build_deref_array(b, attr_arr, src_vertex);
               nir_deref_instr *dst = nir_build_deref_array(b, attr_arr, dst_vertex);

               assert(per_vertex_derefs[a]->instr.type == nir_instr_type_deref);

               nir_copy_deref(b, dst, src);
            }

            /* Rebuild the vertices indices for the primitive by pointing to
             * the new provoking vertex */
            nir_def *new_val =
               nir_vector_insert(b, prim_indices, dst_vertex, provoking_vertex);
            nir_store_deref(b, indexed_primitive_indices_deref, new_val,
                            BITFIELD_MASK(vertices_per_primitive));

            nir_store_deref(b, vertex_deref, nir_iadd_imm(b, dst_vertex, 1), 1);

            /* Finally write the per-primitive values into the per-vertex
             * block at remapped locations.
             */
            for (unsigned i = 0; i < ARRAY_SIZE(mapping); ++i) {
               if (!mapping[i].per_vert_deref)
                  continue;

               assert(mapping[i].per_prim_deref->instr.type == nir_instr_type_deref);

               nir_deref_instr *src =
                  nir_build_deref_array(b, mapping[i].per_prim_deref, primitive_id);
               nir_deref_instr *dst =
                  nir_build_deref_array(b, mapping[i].per_vert_deref, dst_vertex);

               nir_copy_deref(b, dst, src);
            }
         }
         nir_push_else(b, NULL);
         {
            /* If the vertex is not used yet by any primitive, we just have to
             * write the per-primitive values into the per-vertex block at
             * remapped locations.
             */
            nir_store_deref(b, indexed_used_vertex_deref, trueconst, 1);

            for (unsigned i = 0; i < ARRAY_SIZE(mapping); ++i) {
               if (!mapping[i].per_vert_deref)
                  continue;

               assert(mapping[i].per_prim_deref->instr.type == nir_instr_type_deref);

               nir_deref_instr *src =
                  nir_build_deref_array(b, mapping[i].per_prim_deref, primitive_id);
               nir_deref_instr *dst =
                  nir_build_deref_array(b, mapping[i].per_vert_deref, src_vertex);

               nir_copy_deref(b, dst, src);
            }
         }
         nir_pop_if(b, NULL);

         nir_store_deref(b, primitive_deref, nir_iadd_imm(b, primitive_id, 1), 1);
      }
      nir_pop_loop(b, loop);
   }
   nir_pop_if(b, if_stmt); /* local_invocation_index == 0 */

   nir->info.mesh.max_vertices_out += nir->info.mesh.max_primitives_out;

   ralloc_free(mem_ctx);

   return true;
}

void
brw_nir_mesh_convert_attrs_prim_to_vert(struct nir_shader *nir,
                                        struct brw_compile_mesh_params *params,
                                        int *wa_mapping)
{
   NIR_PASS(_, nir, mesh_convert_attrs_prim_to_vert, params, wa_mapping);

   /* Remove per-primitive references */
   NIR_PASS(_, nir, nir_opt_dce);
   NIR_PASS(_, nir, nir_remove_dead_variables, nir_var_shader_out, NULL);

   nir_shader_gather_info(nir, nir_shader_get_entrypoint(nir));

   /* deal with copy_derefs */
   NIR_PASS(_, nir, nir_split_var_copies);
   NIR_PASS(_, nir, nir_lower_var_copies);
}

static bool
frag_update_derefs_instr(struct nir_builder *b, nir_instr *instr, void *data)
{
   if (instr->type != nir_instr_type_deref)
      return false;

   nir_deref_instr *deref = nir_instr_as_deref(instr);
   if (deref->deref_type != nir_deref_type_var)
      return false;

   nir_variable *var = deref->var;
   if (!(var->data.mode & nir_var_shader_in))
      return false;

   int location = var->data.location;
   nir_deref_instr **new_derefs = (nir_deref_instr **)data;
   if (new_derefs[location] == NULL)
      return false;

   nir_def_replace(&deref->def, &new_derefs[location]->def);

   return true;
}

static bool
frag_update_derefs(nir_shader *shader, nir_deref_instr **mapping)
{
   return nir_shader_instructions_pass(shader, frag_update_derefs_instr,
                                       nir_metadata_none, (void *)mapping);
}

bool
brw_nir_frag_convert_attrs_prim_to_vert(struct nir_shader *nir,
                                        const int *wa_mapping)
{
   /* indexed by slot of per-prim attribute */
   nir_deref_instr *new_derefs[VARYING_SLOT_MAX] = {NULL, };

   nir_function_impl *impl = nir_shader_get_entrypoint(nir);
   nir_builder _b = nir_builder_at(nir_before_impl(impl)), *b = &_b;

   uint64_t remapped_inputs = 0;
   nir_foreach_shader_in_variable_safe(var, nir) {
      gl_varying_slot location = var->data.location;
      if (location == VARYING_SLOT_PRIMITIVE_COUNT ||
          location == VARYING_SLOT_PRIMITIVE_INDICES ||
          location == VARYING_SLOT_PRIMITIVE_SHADING_RATE ||
          location == VARYING_SLOT_LAYER ||
          location == VARYING_SLOT_VIEWPORT ||
          location == VARYING_SLOT_CULL_PRIMITIVE)
         continue;

      gl_varying_slot new_location = wa_mapping[location];
      if (new_location == -1)
         continue;

      assert(wa_mapping[new_location] == -1);

      nir_variable *new_var =
         nir_variable_create(nir, nir_var_shader_in, var->type, var->name);
      new_var->data.location = new_location;
      new_var->data.location_frac = var->data.location_frac;
      new_var->data.interpolation = INTERP_MODE_FLAT;

      new_derefs[location] = nir_build_deref_var(b, new_var);
   }

   nir->info.inputs_read |= remapped_inputs;
   nir->info.per_primitive_inputs &= ~remapped_inputs;

   NIR_PASS(_, nir, frag_update_derefs, new_derefs);

   nir_shader_gather_info(nir, impl);

   return true;
}
