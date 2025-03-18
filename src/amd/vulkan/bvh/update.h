/*
 * Copyright © 2022 Konstantin Seurer
 * Copyright © 2025 Valve Corporation
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef RADV_BVH_UPDATE_H
#define RADV_BVH_UPDATE_H

#include "encode.h"

bool
radv_build_triangle(inout vk_aabb bounds, VOID_REF dst_ptr, vk_bvh_geometry_data geom_data, uint32_t global_id,
                    bool gfx12)
{
   bool is_valid = true;
   triangle_indices indices = load_indices(geom_data.indices, geom_data.index_format, global_id);

   triangle_vertices vertices = load_vertices(geom_data.data, indices, geom_data.vertex_format, geom_data.stride);

   /* An inactive triangle is one for which the first (X) component of any vertex is NaN. If any
    * other vertex component is NaN, and the first is not, the behavior is undefined. If the vertex
    * format does not have a NaN representation, then all triangles are considered active.
    */
   if (isnan(vertices.vertex[0].x) || isnan(vertices.vertex[1].x) || isnan(vertices.vertex[2].x))
#if ALWAYS_ACTIVE
      is_valid = false;
#else
      return false;
#endif

   if (geom_data.transform != NULL) {
      mat4 transform = mat4(1.0);

      for (uint32_t col = 0; col < 4; col++)
         for (uint32_t row = 0; row < 3; row++)
            transform[col][row] = DEREF(INDEX(float, geom_data.transform, col + row * 4));

      for (uint32_t i = 0; i < 3; i++)
         vertices.vertex[i] = transform * vertices.vertex[i];
   }

   vk_ir_triangle_node node;

   bounds.min = vec3(INFINITY);
   bounds.max = vec3(-INFINITY);

   for (uint32_t coord = 0; coord < 3; coord++) {
      for (uint32_t comp = 0; comp < 3; comp++) {
         node.coords[coord][comp] = vertices.vertex[coord][comp];
         bounds.min[comp] = min(bounds.min[comp], vertices.vertex[coord][comp]);
         bounds.max[comp] = max(bounds.max[comp], vertices.vertex[coord][comp]);
      }
   }

   node.triangle_id = global_id;
   node.geometry_id_and_flags = geom_data.geometry_id;

   if (gfx12)
      radv_encode_triangle_gfx12(dst_ptr, node);
   else
      radv_encode_triangle_gfx10_3(dst_ptr, node);

   return is_valid;
}

bool
radv_build_aabb(inout vk_aabb bounds, VOID_REF src_ptr, VOID_REF dst_ptr, uint32_t geometry_id, uint32_t global_id,
                bool gfx12)
{
   bool is_valid = true;

   for (uint32_t vec = 0; vec < 2; vec++)
      for (uint32_t comp = 0; comp < 3; comp++) {
         float coord = DEREF(INDEX(float, src_ptr, comp + vec * 3));

         if (vec == 0)
            bounds.min[comp] = coord;
         else
            bounds.max[comp] = coord;
      }

   /* An inactive AABB is one for which the minimum X coordinate is NaN. If any other component is
    * NaN, and the first is not, the behavior is undefined.
    */
   if (isnan(bounds.min.x))
#if ALWAYS_ACTIVE
      is_valid = false;
#else
      return false;
#endif

   vk_ir_aabb_node node;
   node.base.aabb = bounds;
   node.primitive_id = global_id;
   node.geometry_id_and_flags = geometry_id;

   if (gfx12)
      radv_encode_aabb_gfx12(dst_ptr, node);
   else
      radv_encode_aabb_gfx10_3(dst_ptr, node);

   return is_valid;
}

#endif
