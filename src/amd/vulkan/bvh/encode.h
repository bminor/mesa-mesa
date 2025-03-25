/*
 * Copyright © 2022 Friedrich Vock
 * Copyright © 2025 Valve Corporation
 *
 * SPDX-License-Identifier: MIT
 */

/* Helpers for encoding BVH nodes on different HW generations. */

#ifndef RADV_BVH_ENCODE_H
#define RADV_BVH_ENCODE_H

#include "build_helpers.h"

void
radv_encode_triangle_gfx10_3(VOID_REF dst_addr, vk_ir_triangle_node src)
{
   REF(radv_bvh_triangle_node) dst = REF(radv_bvh_triangle_node)(dst_addr);

   DEREF(dst).coords = src.coords;
   DEREF(dst).triangle_id = src.triangle_id;
   DEREF(dst).geometry_id_and_flags = src.geometry_id_and_flags;
   DEREF(dst).id = 9;
}

void
radv_encode_aabb_gfx10_3(VOID_REF dst_addr, vk_ir_aabb_node src)
{
   REF(radv_bvh_aabb_node) dst = REF(radv_bvh_aabb_node)(dst_addr);

   DEREF(dst).primitive_id = src.primitive_id;
   DEREF(dst).geometry_id_and_flags = src.geometry_id_and_flags;
}

void
radv_encode_instance_gfx10_3(VOID_REF dst_addr, vk_ir_instance_node src)
{
   REF(radv_bvh_instance_node) dst = REF(radv_bvh_instance_node)(dst_addr);

   radv_accel_struct_header blas_header = DEREF(REF(radv_accel_struct_header)(src.base_ptr));

   DEREF(dst).bvh_ptr = addr_to_node(src.base_ptr + blas_header.bvh_offset);
   DEREF(dst).bvh_offset = blas_header.bvh_offset;

   mat4 transform = mat4(src.otw_matrix);
   mat4 inv_transform = transpose(inverse(transpose(transform)));
   DEREF(dst).wto_matrix = mat3x4(inv_transform);
   DEREF(dst).otw_matrix = mat3x4(transform);

   DEREF(dst).custom_instance_and_mask = src.custom_instance_and_mask;
   DEREF(dst).sbt_offset_and_flags = radv_encode_sbt_offset_and_flags(src.sbt_offset_and_flags);
   DEREF(dst).instance_id = src.instance_id;
}

#endif
