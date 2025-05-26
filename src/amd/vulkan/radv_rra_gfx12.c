/*
 * Copyright © 2025 Valve Corporation
 *
 * SPDX-License-Identifier: MIT
 */

/* GFX12 specific code for RRA. */

#include "bvh/bvh.h"
#include "radv_rra.h"

#include "util/bitset.h"

struct rra_instance_sideband_data {
   uint32_t instance_index;
   uint32_t custom_instance_and_flags;
   uint32_t blas_metadata_size;
   uint32_t padding;
   mat3x4 otw_matrix;
};

static const char *node_type_names[16] = {
   [radv_bvh_node_triangle + 0] = "triangle0",
   [radv_bvh_node_triangle + 1] = "triangle1",
   [radv_bvh_node_triangle + 2] = "triangle2",
   [radv_bvh_node_triangle + 3] = "triangle3",
   [radv_bvh_node_box16] = "invalid4",
   [radv_bvh_node_box32] = "box32",
   [radv_bvh_node_instance] = "instance",
   [radv_bvh_node_aabb] = "invalid7",
   [8] = "invalid8",
   [9] = "invalid9",
   [10] = "invalid10",
   [11] = "invalid11",
   [12] = "invalid12",
   [13] = "invalid13",
   [14] = "invalid14",
   [15] = "invalid15",
};

bool
rra_validate_node_gfx12(struct hash_table_u64 *accel_struct_vas, uint8_t *data, void *node, uint32_t geometry_count,
                        uint32_t size, bool is_bottom_level, uint32_t depth)
{
   struct rra_validation_context ctx = {0};

   if (depth > 1024) {
      rra_validation_fail(&ctx, "depth > 1024");
      return true;
   }

   uint32_t cur_offset = (uint8_t *)node - data;
   snprintf(ctx.location, sizeof(ctx.location), "internal node (offset=%u)", cur_offset);

   struct radv_gfx12_box_node *box = node;
   uint32_t valid_child_count_minus_one = box->child_count_exponents >> 28;
   if (valid_child_count_minus_one == 0xf)
      return ctx.failed;

   uint32_t internal_id = box->internal_base_id;
   uint32_t primitive_id = box->primitive_base_id;
   for (uint32_t i = 0; i <= valid_child_count_minus_one; i++) {
      uint32_t child_type = (box->children[i].dword2 >> 24) & 0xf;
      uint32_t child_size = box->children[i].dword2 >> 28;

      uint32_t child_id;
      if (child_type == radv_bvh_node_box32) {
         child_id = internal_id;
         internal_id += (child_size * RADV_GFX12_BVH_NODE_SIZE) >> 3;
      } else {
         child_id = primitive_id;
         primitive_id += (child_size * RADV_GFX12_BVH_NODE_SIZE) >> 3;
      }

      uint32_t child_offset = (child_id & (~7u)) << 3;

      if (child_offset >= size) {
         rra_validation_fail(&ctx, "Invalid child offset (child index %u)", i);
         continue;
      }

      struct rra_validation_context child_ctx = {0};
      snprintf(child_ctx.location, sizeof(child_ctx.location), "%s node (offset=%u)", node_type_names[child_type],
               child_offset);

      void *child_node = data + child_offset;

      if (child_type == radv_bvh_node_box32) {
         ctx.failed |= rra_validate_node_gfx12(accel_struct_vas, data, child_node, geometry_count, size,
                                               is_bottom_level, depth + 1);
      } else if (child_type == radv_bvh_node_instance) {
         struct radv_gfx12_instance_node *child = (struct radv_gfx12_instance_node *)(child_node);
         const struct radv_gfx12_instance_node_user_data *user_data =
            (const void *)((const uint8_t *)child + sizeof(struct radv_gfx12_instance_node));

         uint64_t blas_va = radv_node_to_addr(child->pointer_flags_bvh_addr) - user_data->bvh_offset;
         if (!_mesa_hash_table_u64_search(accel_struct_vas, blas_va))
            rra_validation_fail(&child_ctx, "Invalid blas_addr(0x%llx)", (unsigned long long)blas_va);
      } else {
         uint32_t indices_midpoint = BITSET_EXTRACT(child_node, 42, 10);
         if (indices_midpoint < 54 + 28) {
            rra_validation_fail(&child_ctx, "Invalid indices_midpoint(%u)", indices_midpoint);
         } else {
            uint32_t geometry_id = BITSET_EXTRACT(child_node, indices_midpoint - 28, 28);
            if (geometry_id >= geometry_count) {
               rra_validation_fail(&child_ctx, "Invalid geometry_id(%u) >= geometry_count(%u)", geometry_id,
                                   geometry_count);
            }
         }
         if (!BITSET_TEST((BITSET_WORD *)child_node, 1024 - 29))
            rra_validation_fail(&child_ctx, "prim_range_stop is not set");
      }
   }

   return ctx.failed;
}

static uint32_t
get_geometry_id(const void *node, uint32_t node_type)
{
   if (node_type == radv_bvh_node_instance)
      return 0;

   uint32_t indices_midpoint = BITSET_EXTRACT(node, 42, 10);
   return BITSET_EXTRACT(node, indices_midpoint - 28, 28);
}

void
rra_gather_bvh_info_gfx12(const uint8_t *bvh, uint32_t node_id, struct rra_bvh_info *dst)
{
   uint32_t node_type = node_id & 7;

   switch (node_type) {
   case radv_bvh_node_box32:
      dst->internal_nodes_size += sizeof(struct radv_gfx12_box_node);
      break;
   case radv_bvh_node_instance:
      dst->leaf_nodes_size += sizeof(struct radv_gfx12_instance_node);
      dst->instance_sideband_data_size += sizeof(struct rra_instance_sideband_data);
      break;
   case radv_bvh_node_triangle:
      dst->leaf_nodes_size += sizeof(struct radv_gfx12_primitive_node);
      break;
   default:
      unreachable("Invalid node type");
      break;
   }

   const void *node = bvh + ((node_id & (~7u)) << 3);
   if (node_type == radv_bvh_node_box32) {
      const struct radv_gfx12_box_node *src = node;

      uint32_t valid_child_count_minus_one = src->child_count_exponents >> 28;

      if (valid_child_count_minus_one != 0xf) {
         uint32_t internal_id = src->internal_base_id;
         uint32_t primitive_id = src->primitive_base_id;
         for (uint32_t i = 0; i <= valid_child_count_minus_one; i++) {
            uint32_t child_type = (src->children[i].dword2 >> 24) & 0xf;
            uint32_t child_size = src->children[i].dword2 >> 28;

            uint32_t child_id;
            if (child_type == radv_bvh_node_box32) {
               child_id = internal_id | child_type;
               internal_id += (child_size * RADV_GFX12_BVH_NODE_SIZE) >> 3;
            } else {
               child_id = primitive_id | child_type;
               primitive_id += (child_size * RADV_GFX12_BVH_NODE_SIZE) >> 3;
            }

            rra_gather_bvh_info_gfx12(bvh, child_id, dst);
         }
      }
   } else {
      dst->geometry_infos[get_geometry_id(node, node_type)].primitive_count++;
   }
}

static void
rra_transcode_box8_node(struct rra_transcoding_context *ctx, const struct radv_gfx12_box_node *src, uint32_t parent_id,
                        uint32_t dst_offset)
{
   struct radv_gfx12_box_node *dst = (struct radv_gfx12_box_node *)(ctx->dst + dst_offset);

   memcpy(dst, src, sizeof(struct radv_gfx12_box_node));
   dst->internal_base_id = ctx->dst_internal_offset >> 3;
   dst->primitive_base_id = ctx->dst_leaf_offset >> 3;
   dst->unused = parent_id;

   uint32_t valid_child_count_minus_one = dst->child_count_exponents >> 28;
   if (valid_child_count_minus_one == 0xf)
      return;

   uint32_t internal_child_count = 0;
   uint32_t leaf_child_count = 0;
   for (uint32_t i = 0; i <= valid_child_count_minus_one; ++i) {
      uint32_t child_type = (src->children[i].dword2 >> 24) & 0xf;
      if (child_type == radv_bvh_node_box32)
         internal_child_count++;
      else
         leaf_child_count++;
   }

   uint32_t dst_internal_offset = ctx->dst_internal_offset;
   ctx->dst_internal_offset += internal_child_count * RADV_GFX12_BVH_NODE_SIZE;

   uint32_t dst_leaf_offset = ctx->dst_leaf_offset;
   ctx->dst_leaf_offset += leaf_child_count * RADV_GFX12_BVH_NODE_SIZE;

   uint32_t internal_id = src->internal_base_id;
   uint32_t primitive_id = src->primitive_base_id;
   for (uint32_t i = 0; i <= valid_child_count_minus_one; ++i) {
      uint32_t child_type = (src->children[i].dword2 >> 24) & 0xf;
      uint32_t child_size = src->children[i].dword2 >> 28;

      uint32_t child_id;
      uint32_t child_dst_offset;
      if (child_type == radv_bvh_node_box32) {
         child_id = internal_id | child_type;
         internal_id += (child_size * RADV_GFX12_BVH_NODE_SIZE) >> 3;
         child_dst_offset = dst_internal_offset;
         dst_internal_offset += RADV_GFX12_BVH_NODE_SIZE;
      } else {
         child_id = primitive_id | child_type;
         primitive_id += (child_size * RADV_GFX12_BVH_NODE_SIZE) >> 3;
         child_dst_offset = dst_leaf_offset;
         dst_leaf_offset += RADV_GFX12_BVH_NODE_SIZE;
      }

      rra_transcode_node_gfx12(ctx, radv_bvh_node_box32 | (dst_offset >> 3), child_id, child_dst_offset);

      dst->children[i].dword2 = (dst->children[i].dword2 & 0x0fffffff) | (1 << 28);
   }
}

void
rra_transcode_node_gfx12(struct rra_transcoding_context *ctx, uint32_t parent_id, uint32_t src_id, uint32_t dst_offset)
{
   uint32_t node_type = src_id & 7;
   uint32_t src_offset = (src_id & (~7u)) << 3;

   const void *src_child_node = ctx->src + src_offset;
   if (node_type == radv_bvh_node_box32) {
      rra_transcode_box8_node(ctx, src_child_node, parent_id, dst_offset);
   } else {
      memcpy(ctx->dst + dst_offset, src_child_node, RADV_GFX12_BVH_NODE_SIZE);

      if (node_type == radv_bvh_node_instance) {
         struct radv_gfx12_instance_node *dst = (void *)(ctx->dst + dst_offset);

         struct rra_instance_sideband_data *sideband_data = (void *)(ctx->dst + ctx->dst_instance_sideband_data_offset);
         ctx->dst_instance_sideband_data_offset += sizeof(struct rra_instance_sideband_data);

         const struct radv_gfx12_instance_node_user_data *user_data =
            (const void *)((const uint8_t *)src_child_node + sizeof(struct radv_gfx12_instance_node));

         uint64_t blas_addr = radv_node_to_addr(dst->pointer_flags_bvh_addr) - user_data->bvh_offset;

         dst->pointer_flags_bvh_addr = dst->pointer_flags_bvh_addr - (user_data->bvh_offset >> 3) +
                                       (sizeof(struct rra_accel_struct_metadata) >> 3);
         dst->unused = parent_id;

         sideband_data->instance_index = user_data->instance_index;
         sideband_data->custom_instance_and_flags = user_data->custom_instance;
         sideband_data->blas_metadata_size = offsetof(struct rra_accel_struct_metadata, unused);
         sideband_data->otw_matrix = user_data->otw_matrix;

         uint64_t *addr = ralloc(ctx->used_blas, uint64_t);
         if (addr) {
            *addr = blas_addr;
            _mesa_set_add(ctx->used_blas, addr);
         }
      }
   }
}
