/*
 * Copyright © 2016 Red Hat.
 * Copyright © 2016 Bas Nieuwenhuizen
 *
 * based in part on anv driver which is:
 * Copyright © 2015 Intel Corporation
 *
 * SPDX-License-Identifier: MIT
 */

#include "radv_cmd_buffer.h"
#include "meta/radv_meta.h"
#include "ac_shader_util.h"
#include "radv_cp_dma.h"
#include "radv_cs.h"
#include "radv_debug.h"
#include "radv_descriptor_update_template.h"
#include "radv_dgc.h"
#include "radv_event.h"
#include "radv_pipeline_layout.h"
#include "radv_pipeline_rt.h"
#include "radv_radeon_winsys.h"
#include "radv_rmv.h"
#include "radv_rra.h"
#include "radv_sdma.h"
#include "radv_shader.h"
#include "radv_shader_object.h"
#include "radv_sqtt.h"
#include "sid.h"
#include "vk_command_pool.h"
#include "vk_enum_defines.h"
#include "vk_format.h"
#include "vk_framebuffer.h"
#include "vk_render_pass.h"
#include "vk_synchronization.h"
#include "vk_util.h"

#include "ac_debug.h"
#include "ac_descriptors.h"
#include "ac_nir.h"
#include "ac_shader_args.h"

#include "aco_interface.h"

#include "compiler/shader_info.h"
#include "util/compiler.h"
#include "util/fast_idiv_by_const.h"

enum {
   RADV_PREFETCH_VBO_DESCRIPTORS = (1 << 0),
   RADV_PREFETCH_VS = (1 << 1),
   RADV_PREFETCH_TCS = (1 << 2),
   RADV_PREFETCH_TES = (1 << 3),
   RADV_PREFETCH_GS = (1 << 4),
   RADV_PREFETCH_PS = (1 << 5),
   RADV_PREFETCH_MS = (1 << 6),
   RADV_PREFETCH_CS = (1 << 7),
   RADV_PREFETCH_RT = (1 << 8),
   RADV_PREFETCH_GFX_SHADERS = (RADV_PREFETCH_VS | RADV_PREFETCH_TCS | RADV_PREFETCH_TES | RADV_PREFETCH_GS |
                                RADV_PREFETCH_PS | RADV_PREFETCH_MS),
   RADV_PREFETCH_GRAPHICS = (RADV_PREFETCH_VBO_DESCRIPTORS | RADV_PREFETCH_GFX_SHADERS),
};

static void radv_handle_image_transition(struct radv_cmd_buffer *cmd_buffer, struct radv_image *image,
                                         VkImageLayout src_layout, VkImageLayout dst_layout, uint32_t src_family_index,
                                         uint32_t dst_family_index, const VkImageSubresourceRange *range,
                                         struct radv_sample_locations_state *sample_locs);

ALWAYS_INLINE static void
radv_cmd_set_line_width(struct radv_cmd_buffer *cmd_buffer, float line_width)
{
   struct radv_cmd_state *state = &cmd_buffer->state;

   state->dynamic.vk.rs.line.width = line_width;

   state->dirty_dynamic |= RADV_DYNAMIC_LINE_WIDTH;
   state->dirty |= RADV_CMD_DIRTY_GUARDBAND;
}

ALWAYS_INLINE static void
radv_cmd_set_tessellation_domain_origin(struct radv_cmd_buffer *cmd_buffer, VkTessellationDomainOrigin domain_origin)
{
   struct radv_cmd_state *state = &cmd_buffer->state;

   state->dynamic.vk.ts.domain_origin = domain_origin;

   state->dirty_dynamic |= RADV_DYNAMIC_TESS_DOMAIN_ORIGIN;
}

ALWAYS_INLINE static void
radv_cmd_set_patch_control_points(struct radv_cmd_buffer *cmd_buffer, uint32_t patch_control_points)
{
   struct radv_cmd_state *state = &cmd_buffer->state;

   state->dynamic.vk.ts.patch_control_points = patch_control_points;

   state->dirty_dynamic |= RADV_DYNAMIC_PATCH_CONTROL_POINTS;
}

ALWAYS_INLINE static void
radv_cmd_set_depth_clamp_range(struct radv_cmd_buffer *cmd_buffer, VkDepthClampModeEXT depth_clamp_mode,
                               const VkDepthClampRangeEXT *depth_clamp_range)
{
   struct radv_cmd_state *state = &cmd_buffer->state;

   state->dynamic.vk.vp.depth_clamp_mode = depth_clamp_mode;
   if (depth_clamp_mode == VK_DEPTH_CLAMP_MODE_USER_DEFINED_RANGE_EXT) {
      state->dynamic.vk.vp.depth_clamp_range = *depth_clamp_range;
   }

   state->dirty_dynamic |= RADV_DYNAMIC_DEPTH_CLAMP_RANGE;
}

ALWAYS_INLINE static void
radv_cmd_set_depth_clip_negative_one_to_one(struct radv_cmd_buffer *cmd_buffer, bool negative_one_to_one)
{
   struct radv_cmd_state *state = &cmd_buffer->state;

   state->dynamic.vk.vp.depth_clip_negative_one_to_one = negative_one_to_one;

   state->dirty_dynamic |= RADV_DYNAMIC_DEPTH_CLIP_NEGATIVE_ONE_TO_ONE;
}

ALWAYS_INLINE static void
radv_cmd_set_primitive_restart_enable(struct radv_cmd_buffer *cmd_buffer, bool primitive_restart_enable)
{
   struct radv_cmd_state *state = &cmd_buffer->state;

   state->dynamic.vk.ia.primitive_restart_enable = primitive_restart_enable;

   state->dirty_dynamic |= RADV_DYNAMIC_PRIMITIVE_RESTART_ENABLE;
}

struct radv_cmd_set_depth_bias_info {
   float constant_factor;
   float clamp;
   float slope_factor;
   VkDepthBiasRepresentationEXT representation;
};

ALWAYS_INLINE static void
radv_cmd_set_depth_bias(struct radv_cmd_buffer *cmd_buffer, const struct radv_cmd_set_depth_bias_info *info)
{
   struct radv_cmd_state *state = &cmd_buffer->state;

   state->dynamic.vk.rs.depth_bias.constant_factor = info->constant_factor;
   state->dynamic.vk.rs.depth_bias.clamp = info->clamp;
   state->dynamic.vk.rs.depth_bias.slope_factor = info->slope_factor;
   state->dynamic.vk.rs.depth_bias.representation = info->representation;

   state->dirty_dynamic |= RADV_DYNAMIC_DEPTH_BIAS;
}

ALWAYS_INLINE static void
radv_cmd_set_line_stipple(struct radv_cmd_buffer *cmd_buffer, uint32_t line_stipple_factor,
                          uint32_t line_stipple_pattern)
{
   struct radv_cmd_state *state = &cmd_buffer->state;

   state->dynamic.vk.rs.line.stipple.factor = line_stipple_factor;
   state->dynamic.vk.rs.line.stipple.pattern = line_stipple_pattern;

   state->dirty_dynamic |= RADV_DYNAMIC_LINE_STIPPLE;
}

ALWAYS_INLINE static void
radv_cmd_set_cull_mode(struct radv_cmd_buffer *cmd_buffer, VkCullModeFlags cull_mode)
{
   struct radv_cmd_state *state = &cmd_buffer->state;

   state->dynamic.vk.rs.cull_mode = cull_mode;

   state->dirty_dynamic |= RADV_DYNAMIC_CULL_MODE;
}

ALWAYS_INLINE static void
radv_cmd_set_front_face(struct radv_cmd_buffer *cmd_buffer, VkFrontFace front_face)
{
   struct radv_cmd_state *state = &cmd_buffer->state;

   state->dynamic.vk.rs.front_face = front_face;

   state->dirty_dynamic |= RADV_DYNAMIC_FRONT_FACE;
}

ALWAYS_INLINE static void
radv_cmd_set_depth_bias_enable(struct radv_cmd_buffer *cmd_buffer, bool depth_bias_enable)
{
   struct radv_cmd_state *state = &cmd_buffer->state;

   state->dynamic.vk.rs.depth_bias.enable = depth_bias_enable;

   state->dirty_dynamic |= RADV_DYNAMIC_DEPTH_BIAS_ENABLE;
}

ALWAYS_INLINE static void
radv_cmd_set_rasterizer_discard_enable(struct radv_cmd_buffer *cmd_buffer, bool rasterizer_discard_enable)
{
   struct radv_cmd_state *state = &cmd_buffer->state;

   state->dynamic.vk.rs.rasterizer_discard_enable = rasterizer_discard_enable;

   state->dirty_dynamic |= RADV_DYNAMIC_RASTERIZER_DISCARD_ENABLE;
}

ALWAYS_INLINE static void
radv_cmd_set_polygon_mode(struct radv_cmd_buffer *cmd_buffer, VkPolygonMode polygon_mode)
{
   struct radv_cmd_state *state = &cmd_buffer->state;

   if (radv_polygon_mode_is_points_or_lines(state->dynamic.vk.rs.polygon_mode) !=
       radv_polygon_mode_is_points_or_lines(polygon_mode))
      state->dirty |= RADV_CMD_DIRTY_GUARDBAND;

   state->dynamic.vk.rs.polygon_mode = polygon_mode;

   state->dirty_dynamic |= RADV_DYNAMIC_POLYGON_MODE;
}

ALWAYS_INLINE static void
radv_cmd_set_line_stipple_enable(struct radv_cmd_buffer *cmd_buffer, bool line_stipple_enable)
{
   struct radv_cmd_state *state = &cmd_buffer->state;

   state->dynamic.vk.rs.line.stipple.enable = line_stipple_enable;

   state->dirty_dynamic |= RADV_DYNAMIC_LINE_STIPPLE_ENABLE;
}

ALWAYS_INLINE static void
radv_cmd_set_depth_clip_enable(struct radv_cmd_buffer *cmd_buffer, enum vk_mesa_depth_clip_enable depth_clip_enable)
{
   struct radv_cmd_state *state = &cmd_buffer->state;

   state->dynamic.vk.rs.depth_clip_enable = depth_clip_enable;

   state->dirty_dynamic |= RADV_DYNAMIC_DEPTH_CLIP_ENABLE;
}

ALWAYS_INLINE static void
radv_cmd_set_conservative_rasterization_mode(struct radv_cmd_buffer *cmd_buffer,
                                             VkConservativeRasterizationModeEXT conservative_mode)
{
   struct radv_cmd_state *state = &cmd_buffer->state;

   state->dynamic.vk.rs.conservative_mode = conservative_mode;

   state->dirty_dynamic |= RADV_DYNAMIC_CONSERVATIVE_RAST_MODE;
}

ALWAYS_INLINE static void
radv_cmd_set_provoking_vertex_mode(struct radv_cmd_buffer *cmd_buffer, VkProvokingVertexModeEXT provoking_vertex_mode)
{
   struct radv_cmd_state *state = &cmd_buffer->state;

   state->dynamic.vk.rs.provoking_vertex = provoking_vertex_mode;

   state->dirty_dynamic |= RADV_DYNAMIC_PROVOKING_VERTEX_MODE;
}

ALWAYS_INLINE static void
radv_cmd_set_depth_clamp_enable(struct radv_cmd_buffer *cmd_buffer, bool depth_clamp_enable)
{
   struct radv_cmd_state *state = &cmd_buffer->state;

   state->dynamic.vk.rs.depth_clamp_enable = depth_clamp_enable;

   state->dirty_dynamic |= RADV_DYNAMIC_DEPTH_CLAMP_ENABLE;
}

ALWAYS_INLINE static void
radv_cmd_set_line_rasterization_mode(struct radv_cmd_buffer *cmd_buffer, VkLineRasterizationMode line_rast_mode)
{
   struct radv_cmd_state *state = &cmd_buffer->state;

   state->dynamic.vk.rs.line.mode = line_rast_mode;

   state->dirty_dynamic |= RADV_DYNAMIC_LINE_RASTERIZATION_MODE;
}

ALWAYS_INLINE static void
radv_cmd_set_alpha_to_coverage_enable(struct radv_cmd_buffer *cmd_buffer, bool alpha_to_coverage_enable)
{
   struct radv_cmd_state *state = &cmd_buffer->state;

   state->dynamic.vk.ms.alpha_to_coverage_enable = alpha_to_coverage_enable;

   state->dirty_dynamic |= RADV_DYNAMIC_ALPHA_TO_COVERAGE_ENABLE;
}

ALWAYS_INLINE static void
radv_cmd_set_alpha_to_one_enable(struct radv_cmd_buffer *cmd_buffer, bool alpha_to_one_enable)
{
   struct radv_cmd_state *state = &cmd_buffer->state;

   state->dynamic.vk.ms.alpha_to_one_enable = alpha_to_one_enable;

   state->dirty_dynamic |= RADV_DYNAMIC_ALPHA_TO_ONE_ENABLE;
}

ALWAYS_INLINE static void
radv_cmd_set_sample_mask(struct radv_cmd_buffer *cmd_buffer, uint32_t sample_mask)
{
   struct radv_cmd_state *state = &cmd_buffer->state;

   state->dynamic.vk.ms.sample_mask = sample_mask;

   state->dirty_dynamic |= RADV_DYNAMIC_SAMPLE_MASK;
}

ALWAYS_INLINE static void
radv_cmd_set_rasterization_samples(struct radv_cmd_buffer *cmd_buffer, VkSampleCountFlagBits rasterization_samples)
{
   struct radv_cmd_state *state = &cmd_buffer->state;

   state->dynamic.vk.ms.rasterization_samples = rasterization_samples;

   state->dirty_dynamic |= RADV_DYNAMIC_RASTERIZATION_SAMPLES;
}

ALWAYS_INLINE static void
radv_cmd_set_sample_locations_enable(struct radv_cmd_buffer *cmd_buffer, bool sample_locations_enable)
{
   struct radv_cmd_state *state = &cmd_buffer->state;

   state->dynamic.vk.ms.sample_locations_enable = sample_locations_enable;

   state->dirty_dynamic |= RADV_DYNAMIC_SAMPLE_LOCATIONS_ENABLE;
}

ALWAYS_INLINE static void
radv_cmd_set_depth_bounds(struct radv_cmd_buffer *cmd_buffer, float min_depth_bounds, float max_depth_bounds)
{
   struct radv_cmd_state *state = &cmd_buffer->state;

   state->dynamic.vk.ds.depth.bounds_test.min = min_depth_bounds;
   state->dynamic.vk.ds.depth.bounds_test.max = max_depth_bounds;

   state->dirty_dynamic |= RADV_DYNAMIC_DEPTH_BOUNDS;
}

ALWAYS_INLINE static void
radv_cmd_set_stencil_compare_mask(struct radv_cmd_buffer *cmd_buffer, VkStencilFaceFlags face_mask,
                                  uint32_t compare_mask)
{
   struct radv_cmd_state *state = &cmd_buffer->state;

   if (face_mask & VK_STENCIL_FACE_FRONT_BIT)
      state->dynamic.vk.ds.stencil.front.compare_mask = compare_mask;
   if (face_mask & VK_STENCIL_FACE_BACK_BIT)
      state->dynamic.vk.ds.stencil.back.compare_mask = compare_mask;

   state->dirty_dynamic |= RADV_DYNAMIC_STENCIL_COMPARE_MASK;
}

ALWAYS_INLINE static void
radv_cmd_set_stencil_write_mask(struct radv_cmd_buffer *cmd_buffer, VkStencilFaceFlags face_mask, uint32_t write_mask)
{
   struct radv_cmd_state *state = &cmd_buffer->state;

   if (face_mask & VK_STENCIL_FACE_FRONT_BIT)
      state->dynamic.vk.ds.stencil.front.write_mask = write_mask;
   if (face_mask & VK_STENCIL_FACE_BACK_BIT)
      state->dynamic.vk.ds.stencil.back.write_mask = write_mask;

   state->dirty_dynamic |= RADV_DYNAMIC_STENCIL_WRITE_MASK;
}

ALWAYS_INLINE static void
radv_cmd_set_stencil_reference(struct radv_cmd_buffer *cmd_buffer, VkStencilFaceFlags face_mask, uint32_t reference)
{
   struct radv_cmd_state *state = &cmd_buffer->state;

   if (face_mask & VK_STENCIL_FACE_FRONT_BIT)
      state->dynamic.vk.ds.stencil.front.reference = reference;
   if (face_mask & VK_STENCIL_FACE_BACK_BIT)
      state->dynamic.vk.ds.stencil.back.reference = reference;

   state->dirty_dynamic |= RADV_DYNAMIC_STENCIL_REFERENCE;
}

ALWAYS_INLINE static void
radv_cmd_set_logic_op(struct radv_cmd_buffer *cmd_buffer, uint32_t logic_op)
{
   struct radv_cmd_state *state = &cmd_buffer->state;

   state->dynamic.vk.cb.logic_op = logic_op;

   state->dirty_dynamic |= RADV_DYNAMIC_LOGIC_OP;
}

ALWAYS_INLINE static void
radv_cmd_set_color_write_enable(struct radv_cmd_buffer *cmd_buffer, uint32_t color_write_enable)
{
   struct radv_cmd_state *state = &cmd_buffer->state;

   state->dynamic.color_write_enable = color_write_enable;

   state->dirty_dynamic |= RADV_DYNAMIC_COLOR_WRITE_ENABLE;
}

ALWAYS_INLINE static void
radv_cmd_set_color_write_mask(struct radv_cmd_buffer *cmd_buffer, uint32_t color_write_mask)
{
   const struct radv_device *device = radv_cmd_buffer_device(cmd_buffer);
   const struct radv_physical_device *pdev = radv_device_physical(device);
   struct radv_cmd_state *state = &cmd_buffer->state;

   state->dynamic.color_write_mask = color_write_mask;

   state->dirty_dynamic |= RADV_DYNAMIC_COLOR_WRITE_MASK;

   if (pdev->info.rbplus_allowed)
      state->dirty |= RADV_CMD_DIRTY_RBPLUS;
}

ALWAYS_INLINE static void
radv_cmd_set_color_blend_enable(struct radv_cmd_buffer *cmd_buffer, uint8_t color_blend_enable)
{
   struct radv_cmd_state *state = &cmd_buffer->state;

   state->dynamic.color_blend_enable = color_blend_enable;

   state->dirty_dynamic |= RADV_DYNAMIC_COLOR_BLEND_ENABLE;
}

ALWAYS_INLINE static void
radv_cmd_set_logic_op_enable(struct radv_cmd_buffer *cmd_buffer, bool logic_op_enable)
{
   struct radv_cmd_state *state = &cmd_buffer->state;

   state->dynamic.vk.cb.logic_op_enable = logic_op_enable;

   state->dirty_dynamic |= RADV_DYNAMIC_LOGIC_OP_ENABLE;
}

ALWAYS_INLINE static void
radv_cmd_set_fragment_shading_rate(struct radv_cmd_buffer *cmd_buffer, const VkExtent2D *fragment_size,
                                   const VkFragmentShadingRateCombinerOpKHR combiner_ops[2])
{
   struct radv_cmd_state *state = &cmd_buffer->state;

   state->dynamic.vk.fsr.fragment_size = *fragment_size;
   for (unsigned i = 0; i < 2; i++)
      state->dynamic.vk.fsr.combiner_ops[i] = combiner_ops[i];

   state->dirty_dynamic |= RADV_DYNAMIC_FRAGMENT_SHADING_RATE;
}

ALWAYS_INLINE static void
radv_cmd_set_attachment_feedback_loop_enable(struct radv_cmd_buffer *cmd_buffer, VkImageAspectFlags aspect_mask)
{
   struct radv_cmd_state *state = &cmd_buffer->state;

   state->dynamic.feedback_loop_aspects = aspect_mask;

   state->dirty_dynamic |= RADV_DYNAMIC_ATTACHMENT_FEEDBACK_LOOP_ENABLE;
}

ALWAYS_INLINE static void
radv_cmd_set_primitive_topology(struct radv_cmd_buffer *cmd_buffer, uint32_t primitive_topology)
{
   struct radv_cmd_state *state = &cmd_buffer->state;

   if (radv_primitive_topology_is_line_list(state->dynamic.vk.ia.primitive_topology) !=
       radv_primitive_topology_is_line_list(primitive_topology))
      state->dirty |= RADV_CMD_DIRTY_RASTER_STATE;

   state->dynamic.vk.ia.primitive_topology = primitive_topology;

   state->dirty_dynamic |= RADV_DYNAMIC_PRIMITIVE_TOPOLOGY;
}

ALWAYS_INLINE static void
radv_cmd_set_blend_constants(struct radv_cmd_buffer *cmd_buffer, const float blend_constants[4])
{
   struct radv_cmd_state *state = &cmd_buffer->state;

   memcpy(state->dynamic.vk.cb.blend_constants, blend_constants, sizeof(float) * 4);

   state->dirty_dynamic |= RADV_DYNAMIC_BLEND_CONSTANTS;
}

ALWAYS_INLINE static void
radv_cmd_set_discard_rectangle(struct radv_cmd_buffer *cmd_buffer, uint32_t first, uint32_t count,
                               const VkRect2D *discard_rectangles)
{
   struct radv_cmd_state *state = &cmd_buffer->state;

   typed_memcpy(&state->dynamic.vk.dr.rectangles[first], discard_rectangles, count);

   state->dirty_dynamic |= RADV_DYNAMIC_DISCARD_RECTANGLE;
}

ALWAYS_INLINE static void
radv_cmd_set_discard_rectangle_mode(struct radv_cmd_buffer *cmd_buffer,
                                    VkDiscardRectangleModeEXT discard_rectangle_mode)
{
   struct radv_cmd_state *state = &cmd_buffer->state;

   state->dynamic.vk.dr.mode = discard_rectangle_mode;

   state->dirty_dynamic |= RADV_DYNAMIC_DISCARD_RECTANGLE_MODE;
}

ALWAYS_INLINE static void
radv_cmd_set_discard_rectangle_enable(struct radv_cmd_buffer *cmd_buffer, bool discard_rectangle_enable)
{
   struct radv_cmd_state *state = &cmd_buffer->state;

   state->dynamic.vk.dr.enable = discard_rectangle_enable;

   state->dirty_dynamic |= RADV_DYNAMIC_DISCARD_RECTANGLE_ENABLE;
}

ALWAYS_INLINE static void
radv_cmd_set_depth_test_enable(struct radv_cmd_buffer *cmd_buffer, bool depth_test_enable)
{
   struct radv_cmd_state *state = &cmd_buffer->state;

   state->dynamic.vk.ds.depth.test_enable = depth_test_enable;

   state->dirty_dynamic |= RADV_DYNAMIC_DEPTH_TEST_ENABLE;
}

ALWAYS_INLINE static void
radv_cmd_set_depth_write_enable(struct radv_cmd_buffer *cmd_buffer, bool depth_write_enable)
{
   struct radv_cmd_state *state = &cmd_buffer->state;

   state->dynamic.vk.ds.depth.write_enable = depth_write_enable;

   state->dirty_dynamic |= RADV_DYNAMIC_DEPTH_WRITE_ENABLE;
}

ALWAYS_INLINE static void
radv_cmd_set_depth_compare_op(struct radv_cmd_buffer *cmd_buffer, VkCompareOp depth_compare_op)
{
   struct radv_cmd_state *state = &cmd_buffer->state;

   state->dynamic.vk.ds.depth.compare_op = depth_compare_op;

   state->dirty_dynamic |= RADV_DYNAMIC_DEPTH_COMPARE_OP;
}

ALWAYS_INLINE static void
radv_cmd_set_depth_bounds_test_enable(struct radv_cmd_buffer *cmd_buffer, bool depth_bounds_test_enable)
{
   struct radv_cmd_state *state = &cmd_buffer->state;

   state->dynamic.vk.ds.depth.bounds_test.enable = depth_bounds_test_enable;

   state->dirty_dynamic |= RADV_DYNAMIC_DEPTH_BOUNDS_TEST_ENABLE;
}

ALWAYS_INLINE static void
radv_cmd_set_stencil_test_enable(struct radv_cmd_buffer *cmd_buffer, bool stencil_test_enable)
{
   struct radv_cmd_state *state = &cmd_buffer->state;

   state->dynamic.vk.ds.stencil.test_enable = stencil_test_enable;

   state->dirty_dynamic |= RADV_DYNAMIC_STENCIL_TEST_ENABLE;
}

ALWAYS_INLINE static void
radv_cmd_set_stencil_op(struct radv_cmd_buffer *cmd_buffer, VkStencilFaceFlags face_mask, VkStencilOp fail_op,
                        VkStencilOp pass_op, VkStencilOp depth_fail_op, VkCompareOp compare_op)
{
   struct radv_cmd_state *state = &cmd_buffer->state;

   if (face_mask & VK_STENCIL_FACE_FRONT_BIT) {
      state->dynamic.vk.ds.stencil.front.op.fail = fail_op;
      state->dynamic.vk.ds.stencil.front.op.pass = pass_op;
      state->dynamic.vk.ds.stencil.front.op.depth_fail = depth_fail_op;
      state->dynamic.vk.ds.stencil.front.op.compare = compare_op;
   }

   if (face_mask & VK_STENCIL_FACE_BACK_BIT) {
      state->dynamic.vk.ds.stencil.back.op.fail = fail_op;
      state->dynamic.vk.ds.stencil.back.op.pass = pass_op;
      state->dynamic.vk.ds.stencil.back.op.depth_fail = depth_fail_op;
      state->dynamic.vk.ds.stencil.back.op.compare = compare_op;
   }

   state->dirty_dynamic |= RADV_DYNAMIC_STENCIL_OP;
}

ALWAYS_INLINE static void
radv_cmd_set_viewport_with_count(struct radv_cmd_buffer *cmd_buffer, uint32_t viewport_count)
{
   struct radv_cmd_state *state = &cmd_buffer->state;

   state->dynamic.vk.vp.viewport_count = viewport_count;

   state->dirty_dynamic |= RADV_DYNAMIC_VIEWPORT_WITH_COUNT;
   state->dirty |= RADV_CMD_DIRTY_GUARDBAND;
}

ALWAYS_INLINE static void
radv_cmd_set_viewport(struct radv_cmd_buffer *cmd_buffer, uint32_t first, uint32_t count, const VkViewport *viewports,
                      const struct radv_viewport_xform_state *vp_xform)
{
   struct radv_cmd_state *state = &cmd_buffer->state;

   memcpy(state->dynamic.vk.vp.viewports + first, viewports, count * sizeof(*viewports));
   memcpy(state->dynamic.vp_xform + first, vp_xform, count * sizeof(*vp_xform));

   state->dirty_dynamic |= RADV_DYNAMIC_VIEWPORT;
   state->dirty |= RADV_CMD_DIRTY_GUARDBAND;
}

ALWAYS_INLINE static void
radv_cmd_set_scissor_with_count(struct radv_cmd_buffer *cmd_buffer, uint32_t scissor_count)
{
   struct radv_cmd_state *state = &cmd_buffer->state;

   state->dynamic.vk.vp.scissor_count = scissor_count;

   state->dirty_dynamic |= RADV_DYNAMIC_SCISSOR_WITH_COUNT;
}

ALWAYS_INLINE static void
radv_cmd_set_scissor(struct radv_cmd_buffer *cmd_buffer, uint32_t first, uint32_t count, const VkRect2D *scissors)
{
   struct radv_cmd_state *state = &cmd_buffer->state;

   memcpy(state->dynamic.vk.vp.scissors + first, scissors, count * sizeof(*scissors));

   state->dirty_dynamic |= RADV_DYNAMIC_SCISSOR;
}

ALWAYS_INLINE static void
radv_cmd_set_rendering_attachment_locations(struct radv_cmd_buffer *cmd_buffer, uint32_t count,
                                            const uint8_t color_map[MAX_RTS])
{
   struct radv_cmd_state *state = &cmd_buffer->state;

   typed_memcpy(state->dynamic.vk.cal.color_map, color_map, count);

   state->dirty_dynamic |= RADV_DYNAMIC_COLOR_ATTACHMENT_MAP;
   state->dirty |= RADV_CMD_DIRTY_FBFETCH_OUTPUT;
}

ALWAYS_INLINE static void
radv_cmd_set_rendering_input_attachment_indices(struct radv_cmd_buffer *cmd_buffer, uint32_t count,
                                                const uint8_t color_map[MAX_RTS], uint8_t depth_att,
                                                uint8_t stencil_att)
{
   struct radv_cmd_state *state = &cmd_buffer->state;

   typed_memcpy(state->dynamic.vk.ial.color_map, color_map, count);
   state->dynamic.vk.ial.depth_att = depth_att;
   state->dynamic.vk.ial.stencil_att = stencil_att;

   state->dirty_dynamic |= RADV_DYNAMIC_INPUT_ATTACHMENT_MAP;
   state->dirty |= RADV_CMD_DIRTY_FBFETCH_OUTPUT;
}

ALWAYS_INLINE static void
radv_cmd_set_sample_locations(struct radv_cmd_buffer *cmd_buffer, VkSampleCountFlagBits per_pixel, VkExtent2D grid_size,
                              uint32_t count, const VkSampleLocationEXT *sample_locations)
{
   struct radv_cmd_state *state = &cmd_buffer->state;

   state->dynamic.sample_location.per_pixel = per_pixel;
   state->dynamic.sample_location.grid_size = grid_size;
   state->dynamic.sample_location.count = count;
   typed_memcpy(&state->dynamic.sample_location.locations[0], sample_locations, count);

   state->dirty_dynamic |= RADV_DYNAMIC_SAMPLE_LOCATIONS;
}

ALWAYS_INLINE static void
radv_cmd_set_color_blend_equation(struct radv_cmd_buffer *cmd_buffer, uint32_t first, uint32_t count,
                                  const struct radv_blend_equation_state *blend_eq)
{
   struct radv_cmd_state *state = &cmd_buffer->state;

   typed_memcpy(state->dynamic.blend_eq.att + first, blend_eq->att, count);
   if (first == 0)
      state->dynamic.blend_eq.mrt0_is_dual_src = blend_eq->mrt0_is_dual_src;

   state->dirty_dynamic |= RADV_DYNAMIC_COLOR_BLEND_EQUATION;
}

ALWAYS_INLINE static void
radv_cmd_set_vertex_binding_strides(struct radv_cmd_buffer *cmd_buffer, uint32_t first, uint32_t count,
                                    const uint16_t *strides)
{
   struct radv_cmd_state *state = &cmd_buffer->state;

   typed_memcpy(state->dynamic.vk.vi_binding_strides + first, strides, count);

   state->dirty_dynamic |= RADV_DYNAMIC_VERTEX_INPUT_BINDING_STRIDE;
}

ALWAYS_INLINE static void
radv_cmd_set_vertex_input(struct radv_cmd_buffer *cmd_buffer, const struct radv_vertex_input_state *vi_state)
{
   struct radv_cmd_state *state = &cmd_buffer->state;

   memcpy(&state->dynamic.vertex_input, vi_state, sizeof(*vi_state));

   state->dirty_dynamic |= RADV_DYNAMIC_VERTEX_INPUT;
   state->dirty |= RADV_CMD_DIRTY_VS_PROLOG_STATE | RADV_CMD_DIRTY_VERTEX_BUFFER;
}

static void
radv_bind_dynamic_state(struct radv_cmd_buffer *cmd_buffer, const struct radv_dynamic_state *src)
{
   struct radv_dynamic_state *dest = &cmd_buffer->state.dynamic;
   uint64_t copy_mask = src->mask;

   /* Special case for setting the number of rectangles from the pipeline. */
   dest->vk.dr.rectangle_count = src->vk.dr.rectangle_count;

   if (copy_mask & RADV_DYNAMIC_VIEWPORT) {
      if (memcmp(&dest->vk.vp.viewports, &src->vk.vp.viewports, src->vk.vp.viewport_count * sizeof(VkViewport))) {
         radv_cmd_set_viewport(cmd_buffer, 0, src->vk.vp.viewport_count, src->vk.vp.viewports, src->vp_xform);
      }
   }

   if (copy_mask & RADV_DYNAMIC_VIEWPORT_WITH_COUNT) {
      if (dest->vk.vp.viewport_count != src->vk.vp.viewport_count) {
         radv_cmd_set_viewport_with_count(cmd_buffer, src->vk.vp.viewport_count);
      }
   }

   if (copy_mask & RADV_DYNAMIC_SCISSOR) {
      if (memcmp(&dest->vk.vp.scissors, &src->vk.vp.scissors, src->vk.vp.scissor_count * sizeof(VkRect2D))) {
         radv_cmd_set_scissor(cmd_buffer, 0, src->vk.vp.scissor_count, src->vk.vp.scissors);
      }
   }

   if (copy_mask & RADV_DYNAMIC_SCISSOR_WITH_COUNT) {
      if (dest->vk.vp.scissor_count != src->vk.vp.scissor_count) {
         radv_cmd_set_scissor_with_count(cmd_buffer, src->vk.vp.scissor_count);
      }
   }

   if (copy_mask & RADV_DYNAMIC_BLEND_CONSTANTS) {
      if (memcmp(&dest->vk.cb.blend_constants, &src->vk.cb.blend_constants, sizeof(src->vk.cb.blend_constants))) {
         radv_cmd_set_blend_constants(cmd_buffer, src->vk.cb.blend_constants);
      }
   }

   if (copy_mask & RADV_DYNAMIC_DISCARD_RECTANGLE) {
      if (memcmp(&dest->vk.dr.rectangles, &src->vk.dr.rectangles, src->vk.dr.rectangle_count * sizeof(VkRect2D))) {
         radv_cmd_set_discard_rectangle(cmd_buffer, 0, src->vk.dr.rectangle_count, src->vk.dr.rectangles);
      }
   }

   if (copy_mask & RADV_DYNAMIC_SAMPLE_LOCATIONS) {
      if (dest->sample_location.per_pixel != src->sample_location.per_pixel ||
          dest->sample_location.grid_size.width != src->sample_location.grid_size.width ||
          dest->sample_location.grid_size.height != src->sample_location.grid_size.height ||
          memcmp(&dest->sample_location.locations, &src->sample_location.locations,
                 src->sample_location.count * sizeof(VkSampleLocationEXT))) {
         radv_cmd_set_sample_locations(cmd_buffer, src->sample_location.per_pixel, src->sample_location.grid_size,
                                       src->sample_location.count, src->sample_location.locations);
      }
   }

   if (copy_mask & RADV_DYNAMIC_COLOR_BLEND_ENABLE) {
      if (dest->color_blend_enable != src->color_blend_enable) {
         radv_cmd_set_color_blend_enable(cmd_buffer, src->color_blend_enable);
      }
   }

   if (copy_mask & RADV_DYNAMIC_COLOR_BLEND_EQUATION) {
      if (memcmp(&dest->blend_eq, &src->blend_eq, sizeof(src->blend_eq))) {
         radv_cmd_set_color_blend_equation(cmd_buffer, 0, MAX_RTS, &src->blend_eq);
      }
   }

   if (memcmp(&dest->vk.cal.color_map, &src->vk.cal.color_map, sizeof(src->vk.cal.color_map))) {
      radv_cmd_set_rendering_attachment_locations(cmd_buffer, MAX_RTS, src->vk.cal.color_map);
   }

   if (memcmp(&dest->vk.ial, &src->vk.ial, sizeof(src->vk.ial))) {
      radv_cmd_set_rendering_input_attachment_indices(cmd_buffer, MAX_RTS, src->vk.ial.color_map, src->vk.ial.depth_att,
                                                      src->vk.ial.stencil_att);
   }

   if (copy_mask & RADV_DYNAMIC_PRIMITIVE_TOPOLOGY) {
      if (dest->vk.ia.primitive_topology != src->vk.ia.primitive_topology) {
         radv_cmd_set_primitive_topology(cmd_buffer, src->vk.ia.primitive_topology);
      }
   }

   if (copy_mask & RADV_DYNAMIC_LINE_WIDTH) {
      if (dest->vk.rs.line.width != src->vk.rs.line.width) {
         radv_cmd_set_line_width(cmd_buffer, src->vk.rs.line.width);
      }
   }

   if (copy_mask & RADV_DYNAMIC_TESS_DOMAIN_ORIGIN) {
      if (dest->vk.ts.domain_origin != src->vk.ts.domain_origin) {
         radv_cmd_set_tessellation_domain_origin(cmd_buffer, src->vk.ts.domain_origin);
      }
   }

   if (copy_mask & RADV_DYNAMIC_PATCH_CONTROL_POINTS) {
      if (dest->vk.ts.patch_control_points != src->vk.ts.patch_control_points) {
         radv_cmd_set_patch_control_points(cmd_buffer, src->vk.ts.patch_control_points);
      }
   }

   if (copy_mask & RADV_DYNAMIC_DEPTH_CLAMP_RANGE) {
      if (dest->vk.vp.depth_clamp_mode != src->vk.vp.depth_clamp_mode ||
          dest->vk.vp.depth_clamp_range.minDepthClamp != src->vk.vp.depth_clamp_range.minDepthClamp ||
          dest->vk.vp.depth_clamp_range.maxDepthClamp != src->vk.vp.depth_clamp_range.maxDepthClamp) {
         radv_cmd_set_depth_clamp_range(cmd_buffer, src->vk.vp.depth_clamp_mode, &src->vk.vp.depth_clamp_range);
      }
   }

   if (copy_mask & RADV_DYNAMIC_DEPTH_CLIP_NEGATIVE_ONE_TO_ONE) {
      if (dest->vk.vp.depth_clip_negative_one_to_one != src->vk.vp.depth_clip_negative_one_to_one) {
         radv_cmd_set_depth_clip_negative_one_to_one(cmd_buffer, src->vk.vp.depth_clip_negative_one_to_one);
      }
   }

   if (copy_mask & RADV_DYNAMIC_PRIMITIVE_RESTART_ENABLE) {
      if (dest->vk.ia.primitive_restart_enable != src->vk.ia.primitive_restart_enable) {
         radv_cmd_set_primitive_restart_enable(cmd_buffer, src->vk.ia.primitive_restart_enable);
      }
   }

   if (copy_mask & RADV_DYNAMIC_DEPTH_BIAS) {
      if (dest->vk.rs.depth_bias.constant_factor != src->vk.rs.depth_bias.constant_factor ||
          dest->vk.rs.depth_bias.clamp != src->vk.rs.depth_bias.clamp ||
          dest->vk.rs.depth_bias.slope_factor != src->vk.rs.depth_bias.slope_factor ||
          dest->vk.rs.depth_bias.representation != src->vk.rs.depth_bias.representation) {
         const struct radv_cmd_set_depth_bias_info info = {
            .constant_factor = src->vk.rs.depth_bias.constant_factor,
            .clamp = src->vk.rs.depth_bias.clamp,
            .slope_factor = src->vk.rs.depth_bias.slope_factor,
            .representation = src->vk.rs.depth_bias.representation,
         };
         radv_cmd_set_depth_bias(cmd_buffer, &info);
      }
   }

   if (copy_mask & RADV_DYNAMIC_LINE_STIPPLE) {
      if (dest->vk.rs.line.stipple.factor != src->vk.rs.line.stipple.factor ||
          dest->vk.rs.line.stipple.pattern != src->vk.rs.line.stipple.pattern) {
         radv_cmd_set_line_stipple(cmd_buffer, src->vk.rs.line.stipple.factor, src->vk.rs.line.stipple.pattern);
      }
   }

   if (copy_mask & RADV_DYNAMIC_CULL_MODE) {
      if (dest->vk.rs.cull_mode != src->vk.rs.cull_mode) {
         radv_cmd_set_cull_mode(cmd_buffer, src->vk.rs.cull_mode);
      }
   }

   if (copy_mask & RADV_DYNAMIC_FRONT_FACE) {
      if (dest->vk.rs.front_face != src->vk.rs.front_face) {
         radv_cmd_set_front_face(cmd_buffer, src->vk.rs.front_face);
      }
   }

   if (copy_mask & RADV_DYNAMIC_DEPTH_BIAS_ENABLE) {
      if (dest->vk.rs.depth_bias.enable != src->vk.rs.depth_bias.enable) {
         radv_cmd_set_depth_bias_enable(cmd_buffer, src->vk.rs.depth_bias.enable);
      }
   }

   if (copy_mask & RADV_DYNAMIC_RASTERIZER_DISCARD_ENABLE) {
      if (dest->vk.rs.rasterizer_discard_enable != src->vk.rs.rasterizer_discard_enable) {
         radv_cmd_set_rasterizer_discard_enable(cmd_buffer, src->vk.rs.rasterizer_discard_enable);
      }
   }

   if (copy_mask & RADV_DYNAMIC_POLYGON_MODE) {
      if (dest->vk.rs.polygon_mode != src->vk.rs.polygon_mode) {
         radv_cmd_set_polygon_mode(cmd_buffer, src->vk.rs.polygon_mode);
      }
   }

   if (copy_mask & RADV_DYNAMIC_LINE_STIPPLE_ENABLE) {
      if (dest->vk.rs.line.stipple.enable != src->vk.rs.line.stipple.enable) {
         radv_cmd_set_line_stipple_enable(cmd_buffer, src->vk.rs.line.stipple.enable);
      }
   }

   if (copy_mask & RADV_DYNAMIC_DEPTH_CLIP_ENABLE) {
      if (dest->vk.rs.depth_clip_enable != src->vk.rs.depth_clip_enable) {
         radv_cmd_set_depth_clip_enable(cmd_buffer, src->vk.rs.depth_clip_enable);
      }
   }

   if (copy_mask & RADV_DYNAMIC_CONSERVATIVE_RAST_MODE) {
      if (dest->vk.rs.conservative_mode != src->vk.rs.conservative_mode) {
         radv_cmd_set_conservative_rasterization_mode(cmd_buffer, src->vk.rs.conservative_mode);
      }
   }

   if (copy_mask & RADV_DYNAMIC_PROVOKING_VERTEX_MODE) {
      if (dest->vk.rs.provoking_vertex != src->vk.rs.provoking_vertex) {
         radv_cmd_set_provoking_vertex_mode(cmd_buffer, src->vk.rs.provoking_vertex);
      }
   }

   if (copy_mask & RADV_DYNAMIC_DEPTH_CLAMP_ENABLE) {
      if (dest->vk.rs.depth_clamp_enable != src->vk.rs.depth_clamp_enable) {
         radv_cmd_set_depth_clamp_enable(cmd_buffer, src->vk.rs.depth_clamp_enable);
      }
   }

   if (copy_mask & RADV_DYNAMIC_LINE_RASTERIZATION_MODE) {
      if (dest->vk.rs.line.mode != src->vk.rs.line.mode) {
         radv_cmd_set_line_rasterization_mode(cmd_buffer, src->vk.rs.line.mode);
      }
   }

   if (copy_mask & RADV_DYNAMIC_ALPHA_TO_COVERAGE_ENABLE) {
      if (dest->vk.ms.alpha_to_coverage_enable != src->vk.ms.alpha_to_coverage_enable) {
         radv_cmd_set_alpha_to_coverage_enable(cmd_buffer, src->vk.ms.alpha_to_coverage_enable);
      }
   }

   if (copy_mask & RADV_DYNAMIC_ALPHA_TO_ONE_ENABLE) {
      if (dest->vk.ms.alpha_to_one_enable != src->vk.ms.alpha_to_one_enable) {
         radv_cmd_set_alpha_to_one_enable(cmd_buffer, src->vk.ms.alpha_to_one_enable);
      }
   }

   if (copy_mask & RADV_DYNAMIC_SAMPLE_MASK) {
      if (dest->vk.ms.sample_mask != src->vk.ms.sample_mask) {
         radv_cmd_set_sample_mask(cmd_buffer, src->vk.ms.sample_mask);
      }
   }

   if (copy_mask & RADV_DYNAMIC_RASTERIZATION_SAMPLES) {
      if (dest->vk.ms.rasterization_samples != src->vk.ms.rasterization_samples) {
         radv_cmd_set_rasterization_samples(cmd_buffer, src->vk.ms.rasterization_samples);
      }
   }

   if (copy_mask & RADV_DYNAMIC_SAMPLE_LOCATIONS_ENABLE) {
      if (dest->vk.ms.sample_locations_enable != src->vk.ms.sample_locations_enable) {
         radv_cmd_set_sample_locations_enable(cmd_buffer, src->vk.ms.sample_locations_enable);
      }
   }

   if (copy_mask & RADV_DYNAMIC_DEPTH_BOUNDS) {
      if (dest->vk.ds.depth.bounds_test.min != src->vk.ds.depth.bounds_test.min ||
          dest->vk.ds.depth.bounds_test.max != src->vk.ds.depth.bounds_test.max) {
         radv_cmd_set_depth_bounds(cmd_buffer, src->vk.ds.depth.bounds_test.min, src->vk.ds.depth.bounds_test.max);
      }
   }

   if (copy_mask & RADV_DYNAMIC_STENCIL_COMPARE_MASK) {
      if (dest->vk.ds.stencil.front.compare_mask != src->vk.ds.stencil.front.compare_mask) {
         radv_cmd_set_stencil_compare_mask(cmd_buffer, VK_STENCIL_FACE_FRONT_BIT,
                                           src->vk.ds.stencil.front.compare_mask);
      }

      if (dest->vk.ds.stencil.back.compare_mask != src->vk.ds.stencil.back.compare_mask) {
         radv_cmd_set_stencil_compare_mask(cmd_buffer, VK_STENCIL_FACE_BACK_BIT, src->vk.ds.stencil.back.compare_mask);
      }
   }

   if (copy_mask & RADV_DYNAMIC_STENCIL_WRITE_MASK) {
      if (dest->vk.ds.stencil.front.write_mask != src->vk.ds.stencil.front.write_mask) {
         radv_cmd_set_stencil_write_mask(cmd_buffer, VK_STENCIL_FACE_FRONT_BIT, src->vk.ds.stencil.front.write_mask);
      }

      if (dest->vk.ds.stencil.back.write_mask != src->vk.ds.stencil.back.write_mask) {
         radv_cmd_set_stencil_write_mask(cmd_buffer, VK_STENCIL_FACE_BACK_BIT, src->vk.ds.stencil.back.write_mask);
      }
   }

   if (copy_mask & RADV_DYNAMIC_STENCIL_REFERENCE) {
      if (dest->vk.ds.stencil.front.reference != src->vk.ds.stencil.front.reference) {
         radv_cmd_set_stencil_reference(cmd_buffer, VK_STENCIL_FACE_FRONT_BIT, src->vk.ds.stencil.front.reference);
      }

      if (dest->vk.ds.stencil.back.reference != src->vk.ds.stencil.back.reference) {
         radv_cmd_set_stencil_reference(cmd_buffer, VK_STENCIL_FACE_BACK_BIT, src->vk.ds.stencil.back.reference);
      }
   }

   if (copy_mask & RADV_DYNAMIC_DEPTH_TEST_ENABLE) {
      if (dest->vk.ds.depth.test_enable != src->vk.ds.depth.test_enable) {
         radv_cmd_set_depth_test_enable(cmd_buffer, src->vk.ds.depth.test_enable);
      }
   }

   if (copy_mask & RADV_DYNAMIC_DEPTH_WRITE_ENABLE) {
      if (dest->vk.ds.depth.write_enable != src->vk.ds.depth.write_enable) {
         radv_cmd_set_depth_write_enable(cmd_buffer, src->vk.ds.depth.write_enable);
      }
   }

   if (copy_mask & RADV_DYNAMIC_DEPTH_COMPARE_OP) {
      if (dest->vk.ds.depth.compare_op != src->vk.ds.depth.compare_op) {
         radv_cmd_set_depth_compare_op(cmd_buffer, src->vk.ds.depth.compare_op);
      }
   }

   if (copy_mask & RADV_DYNAMIC_DEPTH_BOUNDS_TEST_ENABLE) {
      if (dest->vk.ds.depth.bounds_test.enable != src->vk.ds.depth.bounds_test.enable) {
         radv_cmd_set_depth_bounds_test_enable(cmd_buffer, src->vk.ds.depth.bounds_test.enable);
      }
   }

   if (copy_mask & RADV_DYNAMIC_STENCIL_TEST_ENABLE) {
      if (dest->vk.ds.stencil.test_enable != src->vk.ds.stencil.test_enable) {
         radv_cmd_set_stencil_test_enable(cmd_buffer, src->vk.ds.stencil.test_enable);
      }
   }

   if (copy_mask & RADV_DYNAMIC_STENCIL_OP) {
      if (dest->vk.ds.stencil.front.op.fail != src->vk.ds.stencil.front.op.fail ||
          dest->vk.ds.stencil.front.op.pass != src->vk.ds.stencil.front.op.pass ||
          dest->vk.ds.stencil.front.op.depth_fail != src->vk.ds.stencil.front.op.depth_fail ||
          dest->vk.ds.stencil.front.op.compare != src->vk.ds.stencil.front.op.compare) {
         radv_cmd_set_stencil_op(cmd_buffer, VK_STENCIL_FACE_FRONT_BIT, src->vk.ds.stencil.front.op.fail,
                                 src->vk.ds.stencil.front.op.pass, src->vk.ds.stencil.front.op.depth_fail,
                                 src->vk.ds.stencil.front.op.compare);
      }
      if (dest->vk.ds.stencil.back.op.fail != src->vk.ds.stencil.back.op.fail ||
          dest->vk.ds.stencil.back.op.pass != src->vk.ds.stencil.back.op.pass ||
          dest->vk.ds.stencil.back.op.depth_fail != src->vk.ds.stencil.back.op.depth_fail ||
          dest->vk.ds.stencil.back.op.compare != src->vk.ds.stencil.back.op.compare) {
         radv_cmd_set_stencil_op(cmd_buffer, VK_STENCIL_FACE_BACK_BIT, src->vk.ds.stencil.back.op.fail,
                                 src->vk.ds.stencil.back.op.pass, src->vk.ds.stencil.back.op.depth_fail,
                                 src->vk.ds.stencil.back.op.compare);
      }
   }

   if (copy_mask & RADV_DYNAMIC_LOGIC_OP) {
      if (dest->vk.cb.logic_op != src->vk.cb.logic_op) {
         radv_cmd_set_logic_op(cmd_buffer, src->vk.cb.logic_op);
      }
   }

   if (copy_mask & RADV_DYNAMIC_COLOR_WRITE_ENABLE) {
      if (dest->color_write_enable != src->color_write_enable) {
         radv_cmd_set_color_write_enable(cmd_buffer, src->color_write_enable);
      }
   }

   if (copy_mask & RADV_DYNAMIC_COLOR_WRITE_MASK) {
      if (dest->color_write_mask != src->color_write_mask) {
         radv_cmd_set_color_write_mask(cmd_buffer, src->color_write_mask);
      }
   }

   if (copy_mask & RADV_DYNAMIC_LOGIC_OP_ENABLE) {
      if (dest->vk.cb.logic_op_enable != src->vk.cb.logic_op_enable) {
         radv_cmd_set_logic_op_enable(cmd_buffer, src->vk.cb.logic_op_enable);
      }
   }

   if (copy_mask & RADV_DYNAMIC_FRAGMENT_SHADING_RATE) {
      if (dest->vk.fsr.fragment_size.width != src->vk.fsr.fragment_size.width ||
          dest->vk.fsr.fragment_size.height != src->vk.fsr.fragment_size.height ||
          dest->vk.fsr.combiner_ops[0] != src->vk.fsr.combiner_ops[0] ||
          dest->vk.fsr.combiner_ops[1] != src->vk.fsr.combiner_ops[1]) {
         radv_cmd_set_fragment_shading_rate(cmd_buffer, &src->vk.fsr.fragment_size, src->vk.fsr.combiner_ops);
      }
   }

   if (copy_mask & RADV_DYNAMIC_DISCARD_RECTANGLE_ENABLE) {
      if (dest->vk.dr.enable != src->vk.dr.enable) {
         radv_cmd_set_discard_rectangle_enable(cmd_buffer, src->vk.dr.enable);
      }
   }

   if (copy_mask & RADV_DYNAMIC_DISCARD_RECTANGLE_MODE) {
      if (dest->vk.dr.mode != src->vk.dr.mode) {
         radv_cmd_set_discard_rectangle_mode(cmd_buffer, src->vk.dr.mode);
      }
   }

   if (copy_mask & RADV_DYNAMIC_ATTACHMENT_FEEDBACK_LOOP_ENABLE) {
      if (dest->feedback_loop_aspects != src->feedback_loop_aspects) {
         radv_cmd_set_attachment_feedback_loop_enable(cmd_buffer, src->feedback_loop_aspects);
      }
   }

   if (copy_mask & RADV_DYNAMIC_VERTEX_INPUT_BINDING_STRIDE) {
      if (memcmp(dest->vk.vi_binding_strides, src->vk.vi_binding_strides, sizeof(src->vk.vi_binding_strides))) {
         radv_cmd_set_vertex_binding_strides(cmd_buffer, 0, MESA_VK_MAX_VERTEX_BINDINGS, src->vk.vi_binding_strides);
      }
   }

   if (copy_mask & RADV_DYNAMIC_VERTEX_INPUT) {
      if (memcmp(&dest->vertex_input, &src->vertex_input, sizeof(src->vertex_input))) {
         radv_cmd_set_vertex_input(cmd_buffer, &src->vertex_input);
      }
   }
}

bool
radv_cmd_buffer_uses_mec(struct radv_cmd_buffer *cmd_buffer)
{
   struct radv_device *device = radv_cmd_buffer_device(cmd_buffer);
   const struct radv_physical_device *pdev = radv_device_physical(device);
   return cmd_buffer->qf == RADV_QUEUE_COMPUTE && pdev->info.gfx_level >= GFX7;
}

static void
radv_write_data(struct radv_cmd_buffer *cmd_buffer, const unsigned engine_sel, const uint64_t va, const unsigned count,
                const uint32_t *data, const bool predicating)
{
   struct radv_device *device = radv_cmd_buffer_device(cmd_buffer);

   radv_cs_write_data(device, cmd_buffer->cs, engine_sel, va, count, data, predicating);
}

static void
radv_emit_clear_data(struct radv_cmd_buffer *cmd_buffer, unsigned engine_sel, uint64_t va, unsigned size)
{
   uint32_t *zeroes = alloca(size);
   memset(zeroes, 0, size);
   radv_write_data(cmd_buffer, engine_sel, va, size / 4, zeroes, false);
}

static void
radv_cmd_buffer_finish_shader_part_cache(struct radv_cmd_buffer *cmd_buffer)
{
   _mesa_set_fini(&cmd_buffer->vs_prologs, NULL);
   _mesa_set_fini(&cmd_buffer->ps_epilogs, NULL);
}

static void
radv_cmd_buffer_init_shader_part_cache(struct radv_device *device, struct radv_cmd_buffer *cmd_buffer)
{
   if (device->vs_prologs.ops)
      _mesa_set_init(&cmd_buffer->vs_prologs, NULL, device->vs_prologs.ops->hash, device->vs_prologs.ops->equals);
   if (device->ps_epilogs.ops)
      _mesa_set_init(&cmd_buffer->ps_epilogs, NULL, device->ps_epilogs.ops->hash, device->ps_epilogs.ops->equals);
}

static void
radv_destroy_cmd_buffer(struct vk_command_buffer *vk_cmd_buffer)
{
   struct radv_cmd_buffer *cmd_buffer = container_of(vk_cmd_buffer, struct radv_cmd_buffer, vk);
   struct radv_device *device = radv_cmd_buffer_device(cmd_buffer);

   if (cmd_buffer->qf != RADV_QUEUE_SPARSE) {
      util_dynarray_fini(&cmd_buffer->ray_history);

      radv_rra_accel_struct_buffers_unref(device, cmd_buffer->accel_struct_buffers);
      _mesa_set_destroy(cmd_buffer->accel_struct_buffers, NULL);

      list_for_each_entry_safe (struct radv_cmd_buffer_upload, up, &cmd_buffer->upload.list, list) {
         radv_rmv_log_command_buffer_bo_destroy(device, up->upload_bo);
         radv_bo_destroy(device, &cmd_buffer->vk.base, up->upload_bo);
         list_del(&up->list);
         free(up);
      }

      if (cmd_buffer->upload.upload_bo) {
         radv_rmv_log_command_buffer_bo_destroy(device, cmd_buffer->upload.upload_bo);
         radv_bo_destroy(device, &cmd_buffer->vk.base, cmd_buffer->upload.upload_bo);
      }

      if (cmd_buffer->cs)
         radv_destroy_cmd_stream(device, cmd_buffer->cs);
      if (cmd_buffer->gang.cs)
         radv_destroy_cmd_stream(device, cmd_buffer->gang.cs);

      if (cmd_buffer->transfer.copy_temp)
         radv_bo_destroy(device, &cmd_buffer->vk.base, cmd_buffer->transfer.copy_temp);

      radv_cmd_buffer_finish_shader_part_cache(cmd_buffer);

      for (unsigned i = 0; i < MAX_BIND_POINTS; i++) {
         struct radv_descriptor_set_header *set = &cmd_buffer->descriptors[i].push_set.set;
         free(set->mapped_ptr);
         if (set->layout)
            vk_descriptor_set_layout_unref(&device->vk, &set->layout->vk);
         vk_object_base_finish(&set->base);
      }
   }

   vk_command_buffer_finish(&cmd_buffer->vk);
   vk_free(&cmd_buffer->vk.pool->alloc, cmd_buffer);
}

static VkResult
radv_create_cmd_buffer(struct vk_command_pool *pool, VkCommandBufferLevel level,
                       struct vk_command_buffer **cmd_buffer_out)
{
   struct radv_device *device = container_of(pool->base.device, struct radv_device, vk);
   const struct radv_physical_device *pdev = radv_device_physical(device);
   struct radv_cmd_buffer *cmd_buffer;

   cmd_buffer = vk_zalloc(&pool->alloc, sizeof(*cmd_buffer), 8, VK_SYSTEM_ALLOCATION_SCOPE_OBJECT);
   if (cmd_buffer == NULL)
      return vk_error(device, VK_ERROR_OUT_OF_HOST_MEMORY);

   VkResult result = vk_command_buffer_init(pool, &cmd_buffer->vk, &radv_cmd_buffer_ops, level);
   if (result != VK_SUCCESS) {
      vk_free(&cmd_buffer->vk.pool->alloc, cmd_buffer);
      return result;
   }

   cmd_buffer->qf = vk_queue_to_radv(pdev, pool->queue_family_index);

   if (cmd_buffer->qf != RADV_QUEUE_SPARSE) {
      const enum amd_ip_type ip = radv_queue_family_to_ring(pdev, cmd_buffer->qf);
      list_inithead(&cmd_buffer->upload.list);

      radv_cmd_buffer_init_shader_part_cache(device, cmd_buffer);
      result =
         radv_create_cmd_stream(device, ip, cmd_buffer->vk.level == VK_COMMAND_BUFFER_LEVEL_SECONDARY, &cmd_buffer->cs);
      if (result != VK_SUCCESS) {
         radv_destroy_cmd_buffer(&cmd_buffer->vk);
         return vk_error(device, VK_ERROR_OUT_OF_DEVICE_MEMORY);
      }

      for (unsigned i = 0; i < MAX_BIND_POINTS; i++)
         vk_object_base_init(&device->vk, &cmd_buffer->descriptors[i].push_set.set.base, VK_OBJECT_TYPE_DESCRIPTOR_SET);

      cmd_buffer->accel_struct_buffers = _mesa_pointer_set_create(NULL);
      util_dynarray_init(&cmd_buffer->ray_history, NULL);
   }

   *cmd_buffer_out = &cmd_buffer->vk;

   return VK_SUCCESS;
}

void
radv_cmd_buffer_reset_rendering(struct radv_cmd_buffer *cmd_buffer)
{
   memset(&cmd_buffer->state.render, 0, sizeof(cmd_buffer->state.render));
}

static void
radv_reset_cmd_buffer(struct vk_command_buffer *vk_cmd_buffer, UNUSED VkCommandBufferResetFlags flags)
{
   struct radv_cmd_buffer *cmd_buffer = container_of(vk_cmd_buffer, struct radv_cmd_buffer, vk);
   struct radv_device *device = radv_cmd_buffer_device(cmd_buffer);
   struct radv_cmd_stream *cs = cmd_buffer->cs;

   vk_command_buffer_reset(&cmd_buffer->vk);

   if (cmd_buffer->qf == RADV_QUEUE_SPARSE)
      return;

   radv_reset_cmd_stream(device, cs);
   if (cmd_buffer->gang.cs)
      radv_reset_cmd_stream(device, cmd_buffer->gang.cs);

   list_for_each_entry_safe (struct radv_cmd_buffer_upload, up, &cmd_buffer->upload.list, list) {
      radv_rmv_log_command_buffer_bo_destroy(device, up->upload_bo);
      radv_bo_destroy(device, &cmd_buffer->vk.base, up->upload_bo);
      list_del(&up->list);
      free(up);
   }

   util_dynarray_clear(&cmd_buffer->ray_history);

   radv_rra_accel_struct_buffers_unref(device, cmd_buffer->accel_struct_buffers);

   cmd_buffer->push_constant_stages = 0;
   cmd_buffer->scratch_size_per_wave_needed = 0;
   cmd_buffer->scratch_waves_wanted = 0;
   cmd_buffer->compute_scratch_size_per_wave_needed = 0;
   cmd_buffer->compute_scratch_waves_wanted = 0;
   cmd_buffer->esgs_ring_size_needed = 0;
   cmd_buffer->gsvs_ring_size_needed = 0;
   cmd_buffer->tess_rings_needed = false;
   cmd_buffer->task_rings_needed = false;
   cmd_buffer->mesh_scratch_ring_needed = false;
   cmd_buffer->gds_needed = false;
   cmd_buffer->gds_oa_needed = false;
   cmd_buffer->sample_positions_needed = false;
   cmd_buffer->gang.sem.leader_value = 0;
   cmd_buffer->gang.sem.emitted_leader_value = 0;
   cmd_buffer->gang.sem.va = 0;
   cmd_buffer->shader_upload_seq = 0;

   if (cmd_buffer->upload.upload_bo)
      radv_cs_add_buffer(device->ws, cs->b, cmd_buffer->upload.upload_bo);
   cmd_buffer->upload.offset = 0;

   for (unsigned i = 0; i < MAX_BIND_POINTS; i++) {
      cmd_buffer->descriptors[i].dirty = 0;
      cmd_buffer->descriptors[i].valid = 0;
      cmd_buffer->descriptors[i].dirty_dynamic = false;
   }

   radv_cmd_buffer_reset_rendering(cmd_buffer);
}

const struct vk_command_buffer_ops radv_cmd_buffer_ops = {
   .create = radv_create_cmd_buffer,
   .reset = radv_reset_cmd_buffer,
   .destroy = radv_destroy_cmd_buffer,
};

static bool
radv_cmd_buffer_resize_upload_buf(struct radv_cmd_buffer *cmd_buffer, uint64_t min_needed)
{
   struct radv_device *device = radv_cmd_buffer_device(cmd_buffer);
   struct radv_cmd_stream *cs = cmd_buffer->cs;
   uint64_t new_size;
   struct radeon_winsys_bo *bo = NULL;
   struct radv_cmd_buffer_upload *upload;

   new_size = MAX2(min_needed, 16 * 1024);
   new_size = MAX2(new_size, 2 * cmd_buffer->upload.size);

   VkResult result = radv_bo_create(
      device, &cmd_buffer->vk.base, new_size, 4096, device->ws->cs_domain(device->ws),
      RADEON_FLAG_CPU_ACCESS | RADEON_FLAG_NO_INTERPROCESS_SHARING | RADEON_FLAG_32BIT | RADEON_FLAG_GTT_WC,
      RADV_BO_PRIORITY_UPLOAD_BUFFER, 0, true, &bo);

   if (result != VK_SUCCESS) {
      vk_command_buffer_set_error(&cmd_buffer->vk, result);
      return false;
   }

   radv_cs_add_buffer(device->ws, cs->b, bo);
   if (cmd_buffer->upload.upload_bo) {
      upload = malloc(sizeof(*upload));

      if (!upload) {
         vk_command_buffer_set_error(&cmd_buffer->vk, VK_ERROR_OUT_OF_HOST_MEMORY);
         radv_bo_destroy(device, &cmd_buffer->vk.base, bo);
         return false;
      }

      memcpy(upload, &cmd_buffer->upload, sizeof(*upload));
      list_add(&upload->list, &cmd_buffer->upload.list);
   }

   cmd_buffer->upload.upload_bo = bo;
   cmd_buffer->upload.size = new_size;
   cmd_buffer->upload.offset = 0;
   cmd_buffer->upload.map = radv_buffer_map(device->ws, cmd_buffer->upload.upload_bo);

   if (!cmd_buffer->upload.map) {
      vk_command_buffer_set_error(&cmd_buffer->vk, VK_ERROR_OUT_OF_DEVICE_MEMORY);
      return false;
   }

   radv_rmv_log_command_buffer_bo_create(device, cmd_buffer->upload.upload_bo, 0, cmd_buffer->upload.size, 0);

   return true;
}

bool
radv_cmd_buffer_upload_alloc_aligned(struct radv_cmd_buffer *cmd_buffer, unsigned size, unsigned alignment,
                                     unsigned *out_offset, void **ptr)
{
   assert(size % 4 == 0);

   struct radv_device *device = radv_cmd_buffer_device(cmd_buffer);
   const struct radv_physical_device *pdev = radv_device_physical(device);
   const struct radeon_info *gpu_info = &pdev->info;

   /* Align to the scalar cache line size if it results in this allocation
    * being placed in less of them.
    */
   unsigned offset = cmd_buffer->upload.offset;
   unsigned line_size = gpu_info->gfx_level >= GFX10 ? 64 : 32;
   unsigned gap = align(offset, line_size) - offset;
   if ((size & (line_size - 1)) > gap)
      offset = align(offset, line_size);

   if (alignment)
      offset = align(offset, alignment);
   if (offset + size > cmd_buffer->upload.size) {
      if (!radv_cmd_buffer_resize_upload_buf(cmd_buffer, size))
         return false;
      offset = 0;
   }

   *out_offset = offset;
   *ptr = cmd_buffer->upload.map + offset;

   cmd_buffer->upload.offset = offset + size;
   return true;
}

bool
radv_cmd_buffer_upload_alloc(struct radv_cmd_buffer *cmd_buffer, unsigned size, unsigned *out_offset, void **ptr)
{
   return radv_cmd_buffer_upload_alloc_aligned(cmd_buffer, size, 0, out_offset, ptr);
}

bool
radv_cmd_buffer_upload_data(struct radv_cmd_buffer *cmd_buffer, unsigned size, const void *data, unsigned *out_offset)
{
   uint8_t *ptr;

   if (!radv_cmd_buffer_upload_alloc(cmd_buffer, size, out_offset, (void **)&ptr))
      return false;
   assert(ptr);

   memcpy(ptr, data, size);
   return true;
}

void
radv_cmd_buffer_trace_emit(struct radv_cmd_buffer *cmd_buffer)
{
   struct radv_device *device = radv_cmd_buffer_device(cmd_buffer);
   struct radv_cmd_stream *cs = cmd_buffer->cs;
   uint64_t va;

   if (cmd_buffer->qf != RADV_QUEUE_GENERAL && cmd_buffer->qf != RADV_QUEUE_COMPUTE)
      return;

   va = radv_buffer_get_va(device->trace_bo);
   if (cmd_buffer->vk.level == VK_COMMAND_BUFFER_LEVEL_PRIMARY)
      va += offsetof(struct radv_trace_data, primary_id);
   else
      va += offsetof(struct radv_trace_data, secondary_id);

   ++cmd_buffer->state.trace_id;
   radv_write_data(cmd_buffer, V_370_ME, va, 1, &cmd_buffer->state.trace_id, false);

   radeon_check_space(device->ws, cs->b, 2);

   radeon_begin(cs);
   radeon_emit(PKT3(PKT3_NOP, 0, 0));
   radeon_emit(AC_ENCODE_TRACE_POINT(cmd_buffer->state.trace_id));
   radeon_end();
}

void
radv_cmd_buffer_annotate(struct radv_cmd_buffer *cmd_buffer, const char *annotation)
{
   struct radv_device *device = radv_cmd_buffer_device(cmd_buffer);
   struct radv_cmd_stream *cs = cmd_buffer->cs;

   device->ws->cs_annotate(cs->b, annotation);
}

#define RADV_TASK_SHADER_SENSITIVE_STAGES                                                                              \
   (VK_PIPELINE_STAGE_2_TASK_SHADER_BIT_EXT | VK_PIPELINE_STAGE_2_ALL_GRAPHICS_BIT |                                   \
    VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT | VK_PIPELINE_STAGE_2_PRE_RASTERIZATION_SHADERS_BIT)

static void
radv_gang_barrier(struct radv_cmd_buffer *cmd_buffer, VkPipelineStageFlags2 src_stage_mask,
                  VkPipelineStageFlags2 dst_stage_mask)
{
   /* Update flush bits from the main cmdbuf, except the stage flush. */
   cmd_buffer->gang.flush_bits |=
      cmd_buffer->state.flush_bits & RADV_CMD_FLUSH_ALL_COMPUTE & ~RADV_CMD_FLAG_CS_PARTIAL_FLUSH;

   /* Add stage flush only when necessary. */
   if (src_stage_mask & (VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT | VK_PIPELINE_STAGE_2_COMMAND_PREPROCESS_BIT_EXT |
                         VK_PIPELINE_STAGE_2_BOTTOM_OF_PIPE_BIT | RADV_TASK_SHADER_SENSITIVE_STAGES |
                         VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT))
      cmd_buffer->gang.flush_bits |= RADV_CMD_FLAG_CS_PARTIAL_FLUSH;

   /* Block task shaders when we have to wait for CP DMA on the GFX cmdbuf. */
   if (src_stage_mask &
       (VK_PIPELINE_STAGE_2_COPY_BIT | VK_PIPELINE_STAGE_2_CLEAR_BIT | VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT |
        VK_PIPELINE_STAGE_2_BOTTOM_OF_PIPE_BIT | VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT))
      dst_stage_mask |= cmd_buffer->state.dma_is_busy ? VK_PIPELINE_STAGE_2_TASK_SHADER_BIT_EXT : 0;

   /* Increment the GFX/ACE semaphore when task shaders are blocked. */
   if (dst_stage_mask & (VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT | VK_PIPELINE_STAGE_2_DRAW_INDIRECT_BIT |
                         RADV_TASK_SHADER_SENSITIVE_STAGES))
      cmd_buffer->gang.sem.leader_value++;
}

void
radv_gang_cache_flush(struct radv_cmd_buffer *cmd_buffer)
{
   struct radv_device *device = radv_cmd_buffer_device(cmd_buffer);
   const struct radv_physical_device *pdev = radv_device_physical(device);
   struct radv_cmd_stream *ace_cs = cmd_buffer->gang.cs;
   const uint32_t flush_bits = cmd_buffer->gang.flush_bits;
   enum rgp_flush_bits sqtt_flush_bits = 0;

   radv_cs_emit_cache_flush(device->ws, ace_cs, pdev->info.gfx_level, NULL, 0, flush_bits, &sqtt_flush_bits, 0);

   cmd_buffer->gang.flush_bits = 0;
}

static bool
radv_gang_sem_init(struct radv_cmd_buffer *cmd_buffer)
{
   if (cmd_buffer->gang.sem.va)
      return true;

   /* DWORD 0: GFX->ACE semaphore (GFX blocks ACE, ie. ACE waits for GFX)
    * DWORD 1: ACE->GFX semaphore
    */
   uint64_t sem_init = 0;
   uint32_t va_off = 0;
   if (!radv_cmd_buffer_upload_data(cmd_buffer, sizeof(uint64_t), &sem_init, &va_off)) {
      vk_command_buffer_set_error(&cmd_buffer->vk, VK_ERROR_OUT_OF_HOST_MEMORY);
      return false;
   }

   cmd_buffer->gang.sem.va = radv_buffer_get_va(cmd_buffer->upload.upload_bo) + va_off;
   return true;
}

static bool
radv_gang_leader_sem_dirty(const struct radv_cmd_buffer *cmd_buffer)
{
   return cmd_buffer->gang.sem.leader_value != cmd_buffer->gang.sem.emitted_leader_value;
}

static bool
radv_gang_follower_sem_dirty(const struct radv_cmd_buffer *cmd_buffer)
{
   return cmd_buffer->gang.sem.follower_value != cmd_buffer->gang.sem.emitted_follower_value;
}

ALWAYS_INLINE static bool
radv_flush_gang_semaphore(struct radv_cmd_buffer *cmd_buffer, struct radv_cmd_stream *cs, const uint32_t va_off,
                          const uint32_t value)
{
   struct radv_device *device = radv_cmd_buffer_device(cmd_buffer);
   const struct radv_physical_device *pdev = radv_device_physical(device);

   if (!radv_gang_sem_init(cmd_buffer))
      return false;

   ASSERTED unsigned cdw_max = radeon_check_space(device->ws, cs->b, 12);

   radv_cs_emit_write_event_eop(cs, pdev->info.gfx_level, V_028A90_BOTTOM_OF_PIPE_TS, 0, EOP_DST_SEL_MEM,
                                EOP_DATA_SEL_VALUE_32BIT, cmd_buffer->gang.sem.va + va_off, value,
                                cmd_buffer->gfx9_eop_bug_va);

   assert(cs->b->cdw <= cdw_max);
   return true;
}

ALWAYS_INLINE static bool
radv_flush_gang_leader_semaphore(struct radv_cmd_buffer *cmd_buffer)
{
   if (!radv_gang_leader_sem_dirty(cmd_buffer))
      return false;

   /* Gang leader writes a value to the semaphore which the follower can wait for. */
   cmd_buffer->gang.sem.emitted_leader_value = cmd_buffer->gang.sem.leader_value;
   return radv_flush_gang_semaphore(cmd_buffer, cmd_buffer->cs, 0, cmd_buffer->gang.sem.leader_value);
}

ALWAYS_INLINE static bool
radv_flush_gang_follower_semaphore(struct radv_cmd_buffer *cmd_buffer)
{
   if (!radv_gang_follower_sem_dirty(cmd_buffer))
      return false;

   /* Follower writes a value to the semaphore which the gang leader can wait for. */
   cmd_buffer->gang.sem.emitted_follower_value = cmd_buffer->gang.sem.follower_value;
   return radv_flush_gang_semaphore(cmd_buffer, cmd_buffer->gang.cs, 4, cmd_buffer->gang.sem.follower_value);
}

ALWAYS_INLINE static void
radv_wait_gang_semaphore(struct radv_cmd_buffer *cmd_buffer, struct radv_cmd_stream *cs, const uint32_t va_off,
                         const uint32_t value)
{
   struct radv_device *device = radv_cmd_buffer_device(cmd_buffer);

   assert(cmd_buffer->gang.sem.va);
   radeon_check_space(device->ws, cs->b, 7);
   radv_cp_wait_mem(cs, WAIT_REG_MEM_GREATER_OR_EQUAL, cmd_buffer->gang.sem.va + va_off, value, 0xffffffff);
}

ALWAYS_INLINE static void
radv_wait_gang_leader(struct radv_cmd_buffer *cmd_buffer)
{
   /* Follower waits for the semaphore which the gang leader wrote. */
   radv_wait_gang_semaphore(cmd_buffer, cmd_buffer->gang.cs, 0, cmd_buffer->gang.sem.leader_value);
}

ALWAYS_INLINE static void
radv_wait_gang_follower(struct radv_cmd_buffer *cmd_buffer)
{
   /* Gang leader waits for the semaphore which the follower wrote. */
   radv_wait_gang_semaphore(cmd_buffer, cmd_buffer->cs, 4, cmd_buffer->gang.sem.follower_value);
}

bool
radv_gang_init(struct radv_cmd_buffer *cmd_buffer)
{
   struct radv_device *device = radv_cmd_buffer_device(cmd_buffer);
   VkResult result;

   if (cmd_buffer->gang.cs)
      return true;

   result = radv_create_cmd_stream(device, AMD_IP_COMPUTE, cmd_buffer->vk.level == VK_COMMAND_BUFFER_LEVEL_SECONDARY,
                                   &cmd_buffer->gang.cs);
   if (result != VK_SUCCESS) {
      vk_command_buffer_set_error(&cmd_buffer->vk, result);
      return false;
   }

   return true;
}

static VkResult
radv_gang_finalize(struct radv_cmd_buffer *cmd_buffer)
{
   struct radv_device *device = radv_cmd_buffer_device(cmd_buffer);

   assert(cmd_buffer->gang.cs);
   struct radv_cmd_stream *ace_cs = cmd_buffer->gang.cs;

   /* Emit pending cache flush. */
   radv_gang_cache_flush(cmd_buffer);

   /* Clear the leader<->follower semaphores if they exist.
    * This is necessary in case the same cmd buffer is submitted again in the future.
    */
   if (cmd_buffer->gang.sem.va) {
      uint64_t leader2follower_va = cmd_buffer->gang.sem.va;
      uint64_t follower2leader_va = cmd_buffer->gang.sem.va + 4;
      const uint32_t zero = 0;

      /* Follower: write 0 to the leader->follower semaphore. */
      radv_cs_write_data(device, ace_cs, V_370_ME, leader2follower_va, 1, &zero, false);

      /* Leader: write 0 to the follower->leader semaphore. */
      radv_write_data(cmd_buffer, V_370_ME, follower2leader_va, 1, &zero, false);
   }

   return radv_finalize_cmd_stream(device, cmd_buffer->gang.cs);
}

static void
radv_cmd_buffer_after_draw(struct radv_cmd_buffer *cmd_buffer, enum radv_cmd_flush_bits flags, bool dgc)
{
   struct radv_device *device = radv_cmd_buffer_device(cmd_buffer);
   const struct radv_physical_device *pdev = radv_device_physical(device);
   const struct radv_instance *instance = radv_physical_device_instance(pdev);
   struct radv_cmd_stream *cs = cmd_buffer->cs;

   if (unlikely(device->sqtt.bo) && !dgc) {
      radeon_check_space(device->ws, cs->b, 2);
      radeon_begin(cs);
      radeon_event_write_predicate(V_028A90_THREAD_TRACE_MARKER, cmd_buffer->state.predicating);
      radeon_end();
   }

   if (instance->debug_flags & RADV_DEBUG_SYNC_SHADERS) {
      enum rgp_flush_bits sqtt_flush_bits = 0;
      assert(flags & (RADV_CMD_FLAG_PS_PARTIAL_FLUSH | RADV_CMD_FLAG_CS_PARTIAL_FLUSH));

      /* Force wait for graphics or compute engines to be idle. */
      radv_cs_emit_cache_flush(device->ws, cs, pdev->info.gfx_level, &cmd_buffer->gfx9_fence_idx,
                               cmd_buffer->gfx9_fence_va, flags, &sqtt_flush_bits, cmd_buffer->gfx9_eop_bug_va);

      if ((flags & RADV_CMD_FLAG_PS_PARTIAL_FLUSH) && radv_cmdbuf_has_stage(cmd_buffer, MESA_SHADER_TASK)) {
         /* Force wait for compute engines to be idle on the internal cmdbuf. */
         radv_cs_emit_cache_flush(device->ws, cmd_buffer->gang.cs, pdev->info.gfx_level, NULL, 0,
                                  RADV_CMD_FLAG_CS_PARTIAL_FLUSH, &sqtt_flush_bits, 0);
      }
   }

   if (radv_device_fault_detection_enabled(device))
      radv_cmd_buffer_trace_emit(cmd_buffer);
}

static void
radv_save_pipeline(struct radv_cmd_buffer *cmd_buffer, struct radv_pipeline *pipeline)
{
   struct radv_device *device = radv_cmd_buffer_device(cmd_buffer);
   const struct radv_physical_device *pdev = radv_device_physical(device);
   enum amd_ip_type ring;
   uint32_t data[2];
   uint64_t va;

   va = radv_buffer_get_va(device->trace_bo);

   ring = radv_queue_family_to_ring(pdev, cmd_buffer->qf);

   switch (ring) {
   case AMD_IP_GFX:
      va += offsetof(struct radv_trace_data, gfx_ring_pipeline);
      break;
   case AMD_IP_COMPUTE:
      va += offsetof(struct radv_trace_data, comp_ring_pipeline);
      break;
   default:
      assert(!"invalid IP type");
   }

   uint64_t pipeline_address = (uintptr_t)pipeline;
   data[0] = pipeline_address;
   data[1] = pipeline_address >> 32;

   radv_write_data(cmd_buffer, V_370_ME, va, 2, data, false);
}

static void
radv_save_vertex_descriptors(struct radv_cmd_buffer *cmd_buffer, uint64_t vb_ptr)
{
   struct radv_device *device = radv_cmd_buffer_device(cmd_buffer);
   uint32_t data[2];
   uint64_t va;

   va = radv_buffer_get_va(device->trace_bo) + offsetof(struct radv_trace_data, vertex_descriptors);

   data[0] = vb_ptr;
   data[1] = vb_ptr >> 32;

   radv_write_data(cmd_buffer, V_370_ME, va, 2, data, false);
}

static void
radv_save_vs_prolog(struct radv_cmd_buffer *cmd_buffer, const struct radv_shader_part *prolog)
{
   struct radv_device *device = radv_cmd_buffer_device(cmd_buffer);
   uint32_t data[2];
   uint64_t va;

   va = radv_buffer_get_va(device->trace_bo) + offsetof(struct radv_trace_data, vertex_prolog);

   uint64_t prolog_address = (uintptr_t)prolog;
   data[0] = prolog_address;
   data[1] = prolog_address >> 32;

   radv_write_data(cmd_buffer, V_370_ME, va, 2, data, false);
}

void
radv_set_descriptor_set(struct radv_cmd_buffer *cmd_buffer, VkPipelineBindPoint bind_point,
                        struct radv_descriptor_set *set, unsigned idx)
{
   struct radv_descriptor_state *descriptors_state = radv_get_descriptors_state(cmd_buffer, bind_point);

   descriptors_state->sets[idx] = set;

   descriptors_state->valid |= (1u << idx); /* active descriptors */
   descriptors_state->dirty |= (1u << idx);
}

static void
radv_save_descriptors(struct radv_cmd_buffer *cmd_buffer, VkPipelineBindPoint bind_point)
{
   struct radv_descriptor_state *descriptors_state = radv_get_descriptors_state(cmd_buffer, bind_point);
   struct radv_device *device = radv_cmd_buffer_device(cmd_buffer);
   uint32_t data[MAX_SETS * 2] = {0};
   uint64_t va;
   va = radv_buffer_get_va(device->trace_bo) + offsetof(struct radv_trace_data, descriptor_sets);

   u_foreach_bit (i, descriptors_state->valid) {
      struct radv_descriptor_set *set = descriptors_state->sets[i];
      data[i * 2] = (uint64_t)(uintptr_t)set;
      data[i * 2 + 1] = (uint64_t)(uintptr_t)set >> 32;
   }

   radv_write_data(cmd_buffer, V_370_ME, va, MAX_SETS * 2, data, false);
}

static void
radv_emit_userdata_address(const struct radv_device *device, struct radv_cmd_stream *cs,
                           const struct radv_shader *shader, int idx, uint64_t va)
{
   const struct radv_physical_device *pdev = radv_device_physical(device);
   const uint32_t offset = radv_get_user_sgpr_loc(shader, idx);

   if (!offset)
      return;

   radeon_begin(cs);
   if (pdev->info.gfx_level >= GFX12) {
      gfx12_push_32bit_pointer(offset, va, &pdev->info);
   } else {
      radeon_emit_32bit_pointer(offset, va, &pdev->info);
   }
   radeon_end();
}

static uint64_t
radv_descriptor_get_va(const struct radv_descriptor_state *descriptors_state, unsigned set_idx)
{
   struct radv_descriptor_set *set = descriptors_state->sets[set_idx];
   uint64_t va;

   if (set) {
      va = set->header.va;
   } else {
      va = descriptors_state->descriptor_buffers[set_idx];
   }

   return va;
}

static void
radv_emit_descriptors_per_stage(const struct radv_device *device, struct radv_cmd_stream *cs,
                                const struct radv_shader *shader, const struct radv_descriptor_state *descriptors_state)
{
   const struct radv_physical_device *pdev = radv_device_physical(device);
   const uint32_t indirect_descriptors_offset = radv_get_user_sgpr_loc(shader, AC_UD_INDIRECT_DESCRIPTORS);

   if (indirect_descriptors_offset) {
      radeon_begin(cs);
      if (pdev->info.gfx_level >= GFX12) {
         gfx12_push_32bit_pointer(indirect_descriptors_offset, descriptors_state->indirect_descriptor_sets_va,
                                  &pdev->info);
      } else {
         radeon_emit_32bit_pointer(indirect_descriptors_offset, descriptors_state->indirect_descriptor_sets_va,
                                   &pdev->info);
      }
      radeon_end();
   } else {
      const struct radv_userdata_locations *locs = &shader->info.user_sgprs_locs;
      const uint32_t sh_base = shader->info.user_data_0;
      unsigned mask = locs->descriptor_sets_enabled;

      mask &= descriptors_state->dirty & descriptors_state->valid;

      while (mask) {
         int start, count;

         u_bit_scan_consecutive_range(&mask, &start, &count);

         const struct radv_userdata_info *loc = &locs->descriptor_sets[start];
         const unsigned sh_offset = sh_base + loc->sgpr_idx * 4;

         radeon_begin(cs);
         if (pdev->info.gfx_level >= GFX12) {
            for (int i = 0; i < count; i++) {
               const uint64_t va = radv_descriptor_get_va(descriptors_state, start + i);

               gfx12_push_sh_reg(sh_offset + i * 4, va);
            }
         } else {
            radeon_set_sh_reg_seq(sh_offset, count);

            for (int i = 0; i < count; i++) {
               const uint64_t va = radv_descriptor_get_va(descriptors_state, start + i);

               radeon_emit(va);
            }
         }
         radeon_end();
      }
   }
}

static unsigned
radv_get_vgt_outprim_type(const struct radv_cmd_buffer *cmd_buffer)
{
   const struct radv_shader *last_vgt_shader = cmd_buffer->state.last_vgt_shader;
   const struct radv_dynamic_state *d = &cmd_buffer->state.dynamic;

   /* Ignore dynamic primitive topology for TES/GS/MS stages. */
   if (cmd_buffer->state.active_stages &
       (VK_SHADER_STAGE_TESSELLATION_EVALUATION_BIT | VK_SHADER_STAGE_GEOMETRY_BIT | VK_SHADER_STAGE_MESH_BIT_EXT)) {
      if (cmd_buffer->state.shaders[MESA_SHADER_GEOMETRY]) {
         return radv_conv_gl_prim_to_gs_out(cmd_buffer->state.shaders[MESA_SHADER_GEOMETRY]->info.gs.output_prim);
      } else if (cmd_buffer->state.shaders[MESA_SHADER_TESS_EVAL]) {
         if (cmd_buffer->state.shaders[MESA_SHADER_TESS_EVAL]->info.tes.point_mode) {
            return V_028A6C_POINTLIST;
         } else {
            return radv_conv_tess_prim_to_gs_out(
               cmd_buffer->state.shaders[MESA_SHADER_TESS_EVAL]->info.tes._primitive_mode);
         }
      } else {
         assert(cmd_buffer->state.shaders[MESA_SHADER_MESH]);
         return radv_conv_gl_prim_to_gs_out(cmd_buffer->state.shaders[MESA_SHADER_MESH]->info.ms.output_prim);
      }
   }

   return radv_conv_prim_to_gs_out(d->vk.ia.primitive_topology, last_vgt_shader->info.is_ngg);
}

static ALWAYS_INLINE VkLineRasterizationModeEXT
radv_get_line_mode(const struct radv_cmd_buffer *cmd_buffer)
{
   const struct radv_dynamic_state *d = &cmd_buffer->state.dynamic;

   const unsigned vgt_outprim_type = cmd_buffer->state.vgt_outprim_type;

   const bool draw_lines =
      (radv_vgt_outprim_is_line(vgt_outprim_type) && !radv_polygon_mode_is_point(d->vk.rs.polygon_mode)) ||
      (radv_polygon_mode_is_line(d->vk.rs.polygon_mode) && !radv_vgt_outprim_is_point(vgt_outprim_type));
   if (draw_lines)
      return d->vk.rs.line.mode;

   return VK_LINE_RASTERIZATION_MODE_DEFAULT;
}

static ALWAYS_INLINE unsigned
radv_get_rasterization_samples(struct radv_cmd_buffer *cmd_buffer)
{
   const struct radv_dynamic_state *d = &cmd_buffer->state.dynamic;

   const VkLineRasterizationModeEXT line_mode = cmd_buffer->state.line_rast_mode;

   if (line_mode == VK_LINE_RASTERIZATION_MODE_BRESENHAM) {
      /* From the Vulkan spec 1.3.221:
       *
       * "When Bresenham lines are being rasterized, sample locations may all be treated as being at
       * the pixel center (this may affect attribute and depth interpolation)."
       *
       * "One consequence of this is that Bresenham lines cover the same pixels regardless of the
       * number of rasterization samples, and cover all samples in those pixels (unless masked out
       * or killed)."
       */
      return 1;
   }

   if (line_mode == VK_LINE_RASTERIZATION_MODE_RECTANGULAR_SMOOTH) {
      return RADV_NUM_SMOOTH_AA_SAMPLES;
   }

   return MAX2(1, d->vk.ms.rasterization_samples);
}

static ALWAYS_INLINE bool
radv_is_sample_shading_enabled(struct radv_cmd_buffer *cmd_buffer, float *min_sample_shading)
{
   const struct radv_shader *ps = cmd_buffer->state.shaders[MESA_SHADER_FRAGMENT];

   if (min_sample_shading)
      *min_sample_shading = 1.0f;

   if (cmd_buffer->state.ms.sample_shading_enable) {
      if (min_sample_shading)
         *min_sample_shading = cmd_buffer->state.ms.min_sample_shading;
      return true;
   }

   return ps ? ps->info.ps.uses_sample_shading : false;
}

static ALWAYS_INLINE unsigned
radv_get_ps_iter_samples(struct radv_cmd_buffer *cmd_buffer)
{
   const struct radv_rendering_state *render = &cmd_buffer->state.render;
   unsigned ps_iter_samples = 1;
   float min_sample_shading;

   if (radv_is_sample_shading_enabled(cmd_buffer, &min_sample_shading)) {
      unsigned rasterization_samples = cmd_buffer->state.num_rast_samples;
      unsigned color_samples = MAX2(render->color_samples, rasterization_samples);

      ps_iter_samples = ceilf(min_sample_shading * color_samples);
      ps_iter_samples = util_next_power_of_two(ps_iter_samples);
   }

   return ps_iter_samples;
}

/**
 * Convert the user sample locations to hardware sample locations (the values
 * that will be emitted by PA_SC_AA_SAMPLE_LOCS_PIXEL_*).
 */
static void
radv_convert_user_sample_locs(const struct radv_sample_locations_state *state, uint32_t x, uint32_t y,
                              VkOffset2D *sample_locs)
{
   uint32_t x_offset = x % state->grid_size.width;
   uint32_t y_offset = y % state->grid_size.height;
   uint32_t num_samples = (uint32_t)state->per_pixel;
   uint32_t pixel_offset;

   pixel_offset = (x_offset + y_offset * state->grid_size.width) * num_samples;

   assert(pixel_offset <= MAX_SAMPLE_LOCATIONS);
   const VkSampleLocationEXT *user_locs = &state->locations[pixel_offset];

   for (uint32_t i = 0; i < num_samples; i++) {
      float shifted_pos_x = user_locs[i].x - 0.5;
      float shifted_pos_y = user_locs[i].y - 0.5;

      int32_t scaled_pos_x = floorf(shifted_pos_x * 16);
      int32_t scaled_pos_y = floorf(shifted_pos_y * 16);

      sample_locs[i].x = CLAMP(scaled_pos_x, -8, 7);
      sample_locs[i].y = CLAMP(scaled_pos_y, -8, 7);
   }
}

/**
 * Compute the PA_SC_AA_SAMPLE_LOCS_PIXEL_* mask based on hardware sample
 * locations.
 */
static void
radv_compute_sample_locs_pixel(uint32_t num_samples, VkOffset2D *sample_locs, uint32_t *sample_locs_pixel)
{
   for (uint32_t i = 0; i < num_samples; i++) {
      uint32_t sample_reg_idx = i / 4;
      uint32_t sample_loc_idx = i % 4;
      int32_t pos_x = sample_locs[i].x;
      int32_t pos_y = sample_locs[i].y;

      uint32_t shift_x = 8 * sample_loc_idx;
      uint32_t shift_y = shift_x + 4;

      sample_locs_pixel[sample_reg_idx] |= (pos_x & 0xf) << shift_x;
      sample_locs_pixel[sample_reg_idx] |= (pos_y & 0xf) << shift_y;
   }
}

/**
 * Compute the PA_SC_CENTROID_PRIORITY_* mask based on the top left hardware
 * sample locations.
 */
static uint64_t
radv_compute_centroid_priority(struct radv_cmd_buffer *cmd_buffer, VkOffset2D *sample_locs, uint32_t num_samples)
{
   uint32_t *centroid_priorities = alloca(num_samples * sizeof(*centroid_priorities));
   uint32_t sample_mask = num_samples - 1;
   uint32_t *distances = alloca(num_samples * sizeof(*distances));
   uint64_t centroid_priority = 0;

   /* Compute the distances from center for each sample. */
   for (int i = 0; i < num_samples; i++) {
      distances[i] = (sample_locs[i].x * sample_locs[i].x) + (sample_locs[i].y * sample_locs[i].y);
   }

   /* Compute the centroid priorities by looking at the distances array. */
   for (int i = 0; i < num_samples; i++) {
      uint32_t min_idx = 0;

      for (int j = 1; j < num_samples; j++) {
         if (distances[j] < distances[min_idx])
            min_idx = j;
      }

      centroid_priorities[i] = min_idx;
      distances[min_idx] = 0xffffffff;
   }

   /* Compute the final centroid priority. */
   for (int i = 0; i < 8; i++) {
      centroid_priority |= centroid_priorities[i & sample_mask] << (i * 4);
   }

   return centroid_priority << 32 | centroid_priority;
}

/**
 * Emit the sample locations that are specified with VK_EXT_sample_locations.
 */
static void
radv_emit_sample_locations_state(struct radv_cmd_buffer *cmd_buffer)
{
   const struct radv_device *device = radv_cmd_buffer_device(cmd_buffer);
   const struct radv_physical_device *pdev = radv_device_physical(device);
   const struct radv_dynamic_state *d = &cmd_buffer->state.dynamic;
   uint32_t num_samples = (uint32_t)d->sample_location.per_pixel;
   struct radv_cmd_stream *cs = cmd_buffer->cs;
   uint32_t sample_locs_pixel[4][2] = {0};
   VkOffset2D sample_locs[4][8]; /* 8 is the max. sample count supported */
   uint64_t centroid_priority;

   if (!d->sample_location.count || !d->vk.ms.sample_locations_enable)
      return;

   /* Convert the user sample locations to hardware sample locations. */
   radv_convert_user_sample_locs(&d->sample_location, 0, 0, sample_locs[0]);
   radv_convert_user_sample_locs(&d->sample_location, 1, 0, sample_locs[1]);
   radv_convert_user_sample_locs(&d->sample_location, 0, 1, sample_locs[2]);
   radv_convert_user_sample_locs(&d->sample_location, 1, 1, sample_locs[3]);

   /* Compute the PA_SC_AA_SAMPLE_LOCS_PIXEL_* mask. */
   for (uint32_t i = 0; i < 4; i++) {
      radv_compute_sample_locs_pixel(num_samples, sample_locs[i], sample_locs_pixel[i]);
   }

   /* Compute the PA_SC_CENTROID_PRIORITY_* mask. */
   centroid_priority = radv_compute_centroid_priority(cmd_buffer, sample_locs[0], num_samples);

   radeon_begin(cs);

   /* Emit the specified user sample locations. */
   switch (num_samples) {
   case 1:
   case 2:
   case 4:
      radeon_set_context_reg(R_028BF8_PA_SC_AA_SAMPLE_LOCS_PIXEL_X0Y0_0, sample_locs_pixel[0][0]);
      radeon_set_context_reg(R_028C08_PA_SC_AA_SAMPLE_LOCS_PIXEL_X1Y0_0, sample_locs_pixel[1][0]);
      radeon_set_context_reg(R_028C18_PA_SC_AA_SAMPLE_LOCS_PIXEL_X0Y1_0, sample_locs_pixel[2][0]);
      radeon_set_context_reg(R_028C28_PA_SC_AA_SAMPLE_LOCS_PIXEL_X1Y1_0, sample_locs_pixel[3][0]);
      break;
   case 8:
      radeon_set_context_reg_seq(R_028BF8_PA_SC_AA_SAMPLE_LOCS_PIXEL_X0Y0_0, 2);
      radeon_emit(sample_locs_pixel[0][0]);
      radeon_emit(sample_locs_pixel[0][1]);
      radeon_set_context_reg_seq(R_028C08_PA_SC_AA_SAMPLE_LOCS_PIXEL_X1Y0_0, 2);
      radeon_emit(sample_locs_pixel[1][0]);
      radeon_emit(sample_locs_pixel[1][1]);
      radeon_set_context_reg_seq(R_028C18_PA_SC_AA_SAMPLE_LOCS_PIXEL_X0Y1_0, 2);
      radeon_emit(sample_locs_pixel[2][0]);
      radeon_emit(sample_locs_pixel[2][1]);
      radeon_set_context_reg_seq(R_028C28_PA_SC_AA_SAMPLE_LOCS_PIXEL_X1Y1_0, 2);
      radeon_emit(sample_locs_pixel[3][0]);
      radeon_emit(sample_locs_pixel[3][1]);
      break;
   default:
      UNREACHABLE("invalid number of samples");
   }

   if (pdev->info.gfx_level >= GFX12) {
      radeon_set_context_reg_seq(R_028BF0_PA_SC_CENTROID_PRIORITY_0, 2);
   } else {
      radeon_set_context_reg_seq(R_028BD4_PA_SC_CENTROID_PRIORITY_0, 2);
   }
   radeon_emit(centroid_priority);
   radeon_emit(centroid_priority >> 32);

   if (pdev->info.gfx_level >= GFX7 && pdev->info.gfx_level < GFX12) {
      /* The exclusion bits can be set to improve rasterization efficiency if no sample lies on the pixel boundary
       * (-8 sample offset).
       */
      uint32_t pa_su_prim_filter_cntl = S_02882C_XMAX_RIGHT_EXCLUSION(1) | S_02882C_YMAX_BOTTOM_EXCLUSION(1);
      for (uint32_t i = 0; i < 4; ++i) {
         for (uint32_t j = 0; j < num_samples; ++j) {
            if (sample_locs[i][j].x <= -8)
               pa_su_prim_filter_cntl &= C_02882C_XMAX_RIGHT_EXCLUSION;
            if (sample_locs[i][j].y <= -8)
               pa_su_prim_filter_cntl &= C_02882C_YMAX_BOTTOM_EXCLUSION;
         }
      }

      radeon_set_context_reg(R_02882C_PA_SU_PRIM_FILTER_CNTL, pa_su_prim_filter_cntl);
   }

   radeon_end();
}

static void
radv_emit_inline_push_consts(const struct radv_device *device, struct radv_cmd_stream *cs,
                             const struct radv_shader *shader, int idx, const uint32_t *values)
{
   const struct radv_physical_device *pdev = radv_device_physical(device);
   const struct radv_userdata_info *loc = &shader->info.user_sgprs_locs.shader_data[idx];
   const uint32_t base_reg = shader->info.user_data_0;
   const uint32_t sh_offset = base_reg + loc->sgpr_idx * 4;

   if (loc->sgpr_idx == -1)
      return;

   radeon_check_space(device->ws, cs->b, 2 + loc->num_sgprs);

   radeon_begin(cs);
   if (pdev->info.gfx_level >= GFX12) {
      for (uint32_t i = 0; i < loc->num_sgprs; i++) {
         gfx12_push_sh_reg(sh_offset + i * 4, values[i]);
      }
   } else {
      radeon_set_sh_reg_seq(sh_offset, loc->num_sgprs);
      radeon_emit_array(values, loc->num_sgprs);
   }
   radeon_end();
}

struct radv_bin_size_entry {
   unsigned bpp;
   VkExtent2D extent;
};

static VkExtent2D
radv_gfx10_compute_bin_size(struct radv_cmd_buffer *cmd_buffer)
{
   struct radv_device *device = radv_cmd_buffer_device(cmd_buffer);
   const struct radv_physical_device *pdev = radv_device_physical(device);
   const struct radv_rendering_state *render = &cmd_buffer->state.render;
   const struct radv_dynamic_state *d = &cmd_buffer->state.dynamic;
   VkExtent2D extent = {512, 512};

   const unsigned db_tag_size = 64;
   const unsigned db_tag_count = 312;
   const unsigned color_tag_size = 1024;
   const unsigned color_tag_count = 31;
   const unsigned fmask_tag_size = 256;
   const unsigned fmask_tag_count = 44;

   const unsigned rb_count = pdev->info.max_render_backends;
   const unsigned pipe_count = MAX2(rb_count, pdev->info.num_tcc_blocks);

   const unsigned db_tag_part = (db_tag_count * rb_count / pipe_count) * db_tag_size * pipe_count;
   const unsigned color_tag_part = (color_tag_count * rb_count / pipe_count) * color_tag_size * pipe_count;
   const unsigned fmask_tag_part = (fmask_tag_count * rb_count / pipe_count) * fmask_tag_size * pipe_count;

   const unsigned total_samples = cmd_buffer->state.num_rast_samples;
   const unsigned samples_log = util_logbase2_ceil(total_samples);

   unsigned color_bytes_per_pixel = 0;
   unsigned fmask_bytes_per_pixel = 0;

   for (unsigned i = 0; i < render->color_att_count; ++i) {
      struct radv_image_view *iview = render->color_att[i].iview;

      if (!iview)
         continue;

      if (!((d->color_write_mask >> (4 * i)) & 0xfu))
         continue;

      color_bytes_per_pixel += vk_format_get_blocksize(render->color_att[i].format);

      if (total_samples > 1) {
         assert(samples_log <= 3);
         const unsigned fmask_array[] = {0, 1, 1, 4};
         fmask_bytes_per_pixel += fmask_array[samples_log];
      }
   }

   color_bytes_per_pixel *= total_samples;
   color_bytes_per_pixel = MAX2(color_bytes_per_pixel, 1);

   const unsigned color_pixel_count_log = util_logbase2(color_tag_part / color_bytes_per_pixel);
   extent.width = 1ull << ((color_pixel_count_log + 1) / 2);
   extent.height = 1ull << (color_pixel_count_log / 2);

   if (fmask_bytes_per_pixel) {
      const unsigned fmask_pixel_count_log = util_logbase2(fmask_tag_part / fmask_bytes_per_pixel);

      const VkExtent2D fmask_extent = (VkExtent2D){.width = 1ull << ((fmask_pixel_count_log + 1) / 2),
                                                   .height = 1ull << (color_pixel_count_log / 2)};

      if (fmask_extent.width * fmask_extent.height < extent.width * extent.height)
         extent = fmask_extent;
   }

   if (render->ds_att.iview) {
      /* Coefficients taken from AMDVLK */
      unsigned depth_coeff = vk_format_has_depth(render->ds_att.format) ? 5 : 0;
      unsigned stencil_coeff = vk_format_has_stencil(render->ds_att.format) ? 1 : 0;
      unsigned db_bytes_per_pixel = (depth_coeff + stencil_coeff) * total_samples;

      const unsigned db_pixel_count_log = util_logbase2(db_tag_part / db_bytes_per_pixel);

      const VkExtent2D db_extent =
         (VkExtent2D){.width = 1ull << ((db_pixel_count_log + 1) / 2), .height = 1ull << (color_pixel_count_log / 2)};

      if (db_extent.width * db_extent.height < extent.width * extent.height)
         extent = db_extent;
   }

   extent.width = MAX2(extent.width, 128);
   extent.height = MAX2(extent.width, pdev->info.gfx_level >= GFX12 ? 128 : 64);

   return extent;
}

static VkExtent2D
radv_gfx9_compute_bin_size(struct radv_cmd_buffer *cmd_buffer)
{
   struct radv_device *device = radv_cmd_buffer_device(cmd_buffer);
   const struct radv_physical_device *pdev = radv_device_physical(device);
   const struct radv_rendering_state *render = &cmd_buffer->state.render;
   const struct radv_dynamic_state *d = &cmd_buffer->state.dynamic;
   static const struct radv_bin_size_entry color_size_table[][3][9] = {
      {
         /* One RB / SE */
         {
            /* One shader engine */
            {0, {128, 128}},
            {1, {64, 128}},
            {2, {32, 128}},
            {3, {16, 128}},
            {17, {0, 0}},
            {UINT_MAX, {0, 0}},
         },
         {
            /* Two shader engines */
            {0, {128, 128}},
            {2, {64, 128}},
            {3, {32, 128}},
            {5, {16, 128}},
            {17, {0, 0}},
            {UINT_MAX, {0, 0}},
         },
         {
            /* Four shader engines */
            {0, {128, 128}},
            {3, {64, 128}},
            {5, {16, 128}},
            {17, {0, 0}},
            {UINT_MAX, {0, 0}},
         },
      },
      {
         /* Two RB / SE */
         {
            /* One shader engine */
            {0, {128, 128}},
            {2, {64, 128}},
            {3, {32, 128}},
            {5, {16, 128}},
            {33, {0, 0}},
            {UINT_MAX, {0, 0}},
         },
         {
            /* Two shader engines */
            {0, {128, 128}},
            {3, {64, 128}},
            {5, {32, 128}},
            {9, {16, 128}},
            {33, {0, 0}},
            {UINT_MAX, {0, 0}},
         },
         {
            /* Four shader engines */
            {0, {256, 256}},
            {2, {128, 256}},
            {3, {128, 128}},
            {5, {64, 128}},
            {9, {16, 128}},
            {33, {0, 0}},
            {UINT_MAX, {0, 0}},
         },
      },
      {
         /* Four RB / SE */
         {
            /* One shader engine */
            {0, {128, 256}},
            {2, {128, 128}},
            {3, {64, 128}},
            {5, {32, 128}},
            {9, {16, 128}},
            {33, {0, 0}},
            {UINT_MAX, {0, 0}},
         },
         {
            /* Two shader engines */
            {0, {256, 256}},
            {2, {128, 256}},
            {3, {128, 128}},
            {5, {64, 128}},
            {9, {32, 128}},
            {17, {16, 128}},
            {33, {0, 0}},
            {UINT_MAX, {0, 0}},
         },
         {
            /* Four shader engines */
            {0, {256, 512}},
            {2, {256, 256}},
            {3, {128, 256}},
            {5, {128, 128}},
            {9, {64, 128}},
            {17, {16, 128}},
            {33, {0, 0}},
            {UINT_MAX, {0, 0}},
         },
      },
   };
   static const struct radv_bin_size_entry ds_size_table[][3][9] = {
      {
         // One RB / SE
         {
            // One shader engine
            {0, {128, 256}},
            {2, {128, 128}},
            {4, {64, 128}},
            {7, {32, 128}},
            {13, {16, 128}},
            {49, {0, 0}},
            {UINT_MAX, {0, 0}},
         },
         {
            // Two shader engines
            {0, {256, 256}},
            {2, {128, 256}},
            {4, {128, 128}},
            {7, {64, 128}},
            {13, {32, 128}},
            {25, {16, 128}},
            {49, {0, 0}},
            {UINT_MAX, {0, 0}},
         },
         {
            // Four shader engines
            {0, {256, 512}},
            {2, {256, 256}},
            {4, {128, 256}},
            {7, {128, 128}},
            {13, {64, 128}},
            {25, {16, 128}},
            {49, {0, 0}},
            {UINT_MAX, {0, 0}},
         },
      },
      {
         // Two RB / SE
         {
            // One shader engine
            {0, {256, 256}},
            {2, {128, 256}},
            {4, {128, 128}},
            {7, {64, 128}},
            {13, {32, 128}},
            {25, {16, 128}},
            {97, {0, 0}},
            {UINT_MAX, {0, 0}},
         },
         {
            // Two shader engines
            {0, {256, 512}},
            {2, {256, 256}},
            {4, {128, 256}},
            {7, {128, 128}},
            {13, {64, 128}},
            {25, {32, 128}},
            {49, {16, 128}},
            {97, {0, 0}},
            {UINT_MAX, {0, 0}},
         },
         {
            // Four shader engines
            {0, {512, 512}},
            {2, {256, 512}},
            {4, {256, 256}},
            {7, {128, 256}},
            {13, {128, 128}},
            {25, {64, 128}},
            {49, {16, 128}},
            {97, {0, 0}},
            {UINT_MAX, {0, 0}},
         },
      },
      {
         // Four RB / SE
         {
            // One shader engine
            {0, {256, 512}},
            {2, {256, 256}},
            {4, {128, 256}},
            {7, {128, 128}},
            {13, {64, 128}},
            {25, {32, 128}},
            {49, {16, 128}},
            {UINT_MAX, {0, 0}},
         },
         {
            // Two shader engines
            {0, {512, 512}},
            {2, {256, 512}},
            {4, {256, 256}},
            {7, {128, 256}},
            {13, {128, 128}},
            {25, {64, 128}},
            {49, {32, 128}},
            {97, {16, 128}},
            {UINT_MAX, {0, 0}},
         },
         {
            // Four shader engines
            {0, {512, 512}},
            {4, {256, 512}},
            {7, {256, 256}},
            {13, {128, 256}},
            {25, {128, 128}},
            {49, {64, 128}},
            {97, {16, 128}},
            {UINT_MAX, {0, 0}},
         },
      },
   };

   VkExtent2D extent = {512, 512};

   unsigned log_num_rb_per_se = util_logbase2_ceil(pdev->info.max_render_backends / pdev->info.max_se);
   unsigned log_num_se = util_logbase2_ceil(pdev->info.max_se);

   unsigned total_samples = cmd_buffer->state.num_rast_samples;
   unsigned ps_iter_samples = radv_get_ps_iter_samples(cmd_buffer);
   unsigned effective_samples = total_samples;
   unsigned color_bytes_per_pixel = 0;

   for (unsigned i = 0; i < render->color_att_count; ++i) {
      struct radv_image_view *iview = render->color_att[i].iview;

      if (!iview)
         continue;

      if (!((d->color_write_mask >> (4 * i)) & 0xfu))
         continue;

      color_bytes_per_pixel += vk_format_get_blocksize(render->color_att[i].format);
   }

   /* MSAA images typically don't use all samples all the time. */
   if (effective_samples >= 2 && ps_iter_samples <= 1)
      effective_samples = 2;
   color_bytes_per_pixel *= effective_samples;

   const struct radv_bin_size_entry *color_entry = color_size_table[log_num_rb_per_se][log_num_se];
   while (color_entry[1].bpp <= color_bytes_per_pixel)
      ++color_entry;

   extent = color_entry->extent;

   if (render->ds_att.iview) {
      /* Coefficients taken from AMDVLK */
      unsigned depth_coeff = vk_format_has_depth(render->ds_att.format) ? 5 : 0;
      unsigned stencil_coeff = vk_format_has_stencil(render->ds_att.format) ? 1 : 0;
      unsigned ds_bytes_per_pixel = 4 * (depth_coeff + stencil_coeff) * total_samples;

      const struct radv_bin_size_entry *ds_entry = ds_size_table[log_num_rb_per_se][log_num_se];
      while (ds_entry[1].bpp <= ds_bytes_per_pixel)
         ++ds_entry;

      if (ds_entry->extent.width * ds_entry->extent.height < extent.width * extent.height)
         extent = ds_entry->extent;
   }

   return extent;
}

static unsigned
radv_get_disabled_binning_state(struct radv_cmd_buffer *cmd_buffer)
{
   struct radv_device *device = radv_cmd_buffer_device(cmd_buffer);
   const struct radv_physical_device *pdev = radv_device_physical(device);
   const struct radv_rendering_state *render = &cmd_buffer->state.render;
   const struct radv_dynamic_state *d = &cmd_buffer->state.dynamic;
   uint32_t pa_sc_binner_cntl_0;

   if (pdev->info.gfx_level >= GFX12) {
      const uint32_t bin_size_x = 128, bin_size_y = 128;

      pa_sc_binner_cntl_0 =
         S_028C44_BINNING_MODE(V_028C44_BINNING_DISABLED) | S_028C44_BIN_SIZE_X_EXTEND(util_logbase2(bin_size_x) - 5) |
         S_028C44_BIN_SIZE_Y_EXTEND(util_logbase2(bin_size_y) - 5) | S_028C44_DISABLE_START_OF_PRIM(1) |
         S_028C44_FPOVS_PER_BATCH(63) | S_028C44_OPTIMAL_BIN_SELECTION(1) | S_028C44_FLUSH_ON_BINNING_TRANSITION(1);
   } else if (pdev->info.gfx_level >= GFX10) {
      const unsigned binning_disabled =
         pdev->info.gfx_level >= GFX11_5 ? V_028C44_BINNING_DISABLED : V_028C44_DISABLE_BINNING_USE_NEW_SC;
      unsigned min_bytes_per_pixel = 0;

      for (unsigned i = 0; i < render->color_att_count; ++i) {
         struct radv_image_view *iview = render->color_att[i].iview;

         if (!iview)
            continue;

         if (!((d->color_write_mask >> (4 * i)) & 0xfu))
            continue;

         unsigned bytes = vk_format_get_blocksize(render->color_att[i].format);
         if (!min_bytes_per_pixel || bytes < min_bytes_per_pixel)
            min_bytes_per_pixel = bytes;
      }

      pa_sc_binner_cntl_0 = S_028C44_BINNING_MODE(binning_disabled) | S_028C44_BIN_SIZE_X(0) | S_028C44_BIN_SIZE_Y(0) |
                            S_028C44_BIN_SIZE_X_EXTEND(2) |                                /* 128 */
                            S_028C44_BIN_SIZE_Y_EXTEND(min_bytes_per_pixel <= 4 ? 2 : 1) | /* 128 or 64 */
                            S_028C44_DISABLE_START_OF_PRIM(1) | S_028C44_FLUSH_ON_BINNING_TRANSITION(1);
   } else {
      pa_sc_binner_cntl_0 =
         S_028C44_BINNING_MODE(V_028C44_DISABLE_BINNING_USE_LEGACY_SC) | S_028C44_DISABLE_START_OF_PRIM(1) |
         S_028C44_FLUSH_ON_BINNING_TRANSITION(pdev->info.family == CHIP_VEGA12 || pdev->info.family == CHIP_VEGA20 ||
                                              pdev->info.family >= CHIP_RAVEN2);
   }

   return pa_sc_binner_cntl_0;
}

static unsigned
radv_get_binning_state(struct radv_cmd_buffer *cmd_buffer)
{
   struct radv_device *device = radv_cmd_buffer_device(cmd_buffer);
   const struct radv_physical_device *pdev = radv_device_physical(device);
   unsigned pa_sc_binner_cntl_0;
   VkExtent2D bin_size;

   if (pdev->info.gfx_level >= GFX10) {
      bin_size = radv_gfx10_compute_bin_size(cmd_buffer);
   } else {
      assert(pdev->info.gfx_level == GFX9);
      bin_size = radv_gfx9_compute_bin_size(cmd_buffer);
   }

   if (device->pbb_allowed && bin_size.width && bin_size.height) {
      const struct radv_binning_settings *settings = &pdev->binning_settings;

      pa_sc_binner_cntl_0 =
         S_028C44_BINNING_MODE(V_028C44_BINNING_ALLOWED) | S_028C44_BIN_SIZE_X(bin_size.width == 16) |
         S_028C44_BIN_SIZE_Y(bin_size.height == 16) |
         S_028C44_BIN_SIZE_X_EXTEND(util_logbase2(MAX2(bin_size.width, 32)) - 5) |
         S_028C44_BIN_SIZE_Y_EXTEND(util_logbase2(MAX2(bin_size.height, 32)) - 5) |
         S_028C44_CONTEXT_STATES_PER_BIN(settings->context_states_per_bin - 1) |
         S_028C44_PERSISTENT_STATES_PER_BIN(settings->persistent_states_per_bin - 1) |
         S_028C44_DISABLE_START_OF_PRIM(1) | S_028C44_FPOVS_PER_BATCH(settings->fpovs_per_batch) |
         S_028C44_OPTIMAL_BIN_SELECTION(1) |
         S_028C44_FLUSH_ON_BINNING_TRANSITION(pdev->info.family == CHIP_VEGA12 || pdev->info.family == CHIP_VEGA20 ||
                                              pdev->info.family >= CHIP_RAVEN2);
   } else {
      pa_sc_binner_cntl_0 = radv_get_disabled_binning_state(cmd_buffer);
   }

   return pa_sc_binner_cntl_0;
}

static void
radv_emit_binning_state(struct radv_cmd_buffer *cmd_buffer)
{
   struct radv_device *device = radv_cmd_buffer_device(cmd_buffer);
   const struct radv_physical_device *pdev = radv_device_physical(device);
   struct radv_cmd_stream *cs = cmd_buffer->cs;

   if (pdev->info.gfx_level >= GFX9) {
      const uint32_t pa_sc_binner_cntl_0 = radv_get_binning_state(cmd_buffer);

      radeon_begin(cs);
      radeon_opt_set_context_reg(R_028C44_PA_SC_BINNER_CNTL_0, RADV_TRACKED_PA_SC_BINNER_CNTL_0, pa_sc_binner_cntl_0);
      radeon_end();
   }
}

static void
radv_emit_shader_prefetch(struct radv_cmd_buffer *cmd_buffer, struct radv_shader *shader)
{
   uint64_t va;

   if (!shader)
      return;

   va = radv_shader_get_va(shader);

   radv_cp_dma_prefetch(cmd_buffer, va, shader->code_size);
}

ALWAYS_INLINE static void
radv_emit_graphics_prefetch(struct radv_cmd_buffer *cmd_buffer, bool first_stage_only)
{
   struct radv_cmd_state *state = &cmd_buffer->state;
   uint32_t mask = state->prefetch_L2_mask & RADV_PREFETCH_GRAPHICS;

   if (!mask)
      return;

   /* Fast prefetch path for starting draws as soon as possible. */
   if (first_stage_only)
      mask &= RADV_PREFETCH_VS | RADV_PREFETCH_VBO_DESCRIPTORS | RADV_PREFETCH_MS;

   if (mask & RADV_PREFETCH_VS)
      radv_emit_shader_prefetch(cmd_buffer, cmd_buffer->state.shaders[MESA_SHADER_VERTEX]);

   if (mask & RADV_PREFETCH_MS)
      radv_emit_shader_prefetch(cmd_buffer, cmd_buffer->state.shaders[MESA_SHADER_MESH]);

   if (mask & RADV_PREFETCH_VBO_DESCRIPTORS)
      radv_cp_dma_prefetch(cmd_buffer, state->vb_va, state->vb_size);

   if (mask & RADV_PREFETCH_TCS)
      radv_emit_shader_prefetch(cmd_buffer, cmd_buffer->state.shaders[MESA_SHADER_TESS_CTRL]);

   if (mask & RADV_PREFETCH_TES)
      radv_emit_shader_prefetch(cmd_buffer, cmd_buffer->state.shaders[MESA_SHADER_TESS_EVAL]);

   if (mask & RADV_PREFETCH_GS) {
      radv_emit_shader_prefetch(cmd_buffer, cmd_buffer->state.shaders[MESA_SHADER_GEOMETRY]);
      if (cmd_buffer->state.gs_copy_shader)
         radv_emit_shader_prefetch(cmd_buffer, cmd_buffer->state.gs_copy_shader);
   }

   if (mask & RADV_PREFETCH_PS) {
      radv_emit_shader_prefetch(cmd_buffer, cmd_buffer->state.shaders[MESA_SHADER_FRAGMENT]);
   }

   state->prefetch_L2_mask &= ~mask;
}

ALWAYS_INLINE static void
radv_emit_compute_prefetch(struct radv_cmd_buffer *cmd_buffer)
{
   struct radv_cmd_state *state = &cmd_buffer->state;
   uint32_t mask = state->prefetch_L2_mask & RADV_PREFETCH_CS;

   if (!mask)
      return;

   radv_emit_shader_prefetch(cmd_buffer, cmd_buffer->state.shaders[MESA_SHADER_COMPUTE]);

   state->prefetch_L2_mask &= ~mask;
}

ALWAYS_INLINE static void
radv_emit_ray_tracing_prefetch(struct radv_cmd_buffer *cmd_buffer)
{
   struct radv_cmd_state *state = &cmd_buffer->state;
   uint32_t mask = state->prefetch_L2_mask & RADV_PREFETCH_RT;

   if (!mask)
      return;

   radv_emit_shader_prefetch(cmd_buffer, cmd_buffer->state.rt_prolog);

   state->prefetch_L2_mask &= ~mask;
}

static void
radv_emit_rbplus_state(struct radv_cmd_buffer *cmd_buffer)
{
   struct radv_device *device = radv_cmd_buffer_device(cmd_buffer);
   const struct radv_physical_device *pdev = radv_device_physical(device);
   struct radv_cmd_stream *cs = cmd_buffer->cs;

   assert(pdev->info.rbplus_allowed);

   const struct radv_dynamic_state *d = &cmd_buffer->state.dynamic;
   struct radv_rendering_state *render = &cmd_buffer->state.render;

   unsigned sx_ps_downconvert = 0;
   unsigned sx_blend_opt_epsilon = 0;
   unsigned sx_blend_opt_control = 0;

   for (unsigned i = 0; i < render->color_att_count; i++) {
      unsigned format, swap;
      bool has_alpha, has_rgb;
      if (render->color_att[i].iview == NULL) {
         /* We don't set the DISABLE bits, because the HW can't have holes,
          * so the SPI color format is set to 32-bit 1-component. */
         sx_ps_downconvert |= V_028754_SX_RT_EXPORT_32_R << (i * 4);
         continue;
      }

      struct radv_color_buffer_info *cb = &render->color_att[i].cb;

      format = pdev->info.gfx_level >= GFX11 ? G_028C70_FORMAT_GFX11(cb->ac.cb_color_info)
                                             : G_028C70_FORMAT_GFX6(cb->ac.cb_color_info);
      swap = G_028C70_COMP_SWAP(cb->ac.cb_color_info);
      has_alpha = pdev->info.gfx_level >= GFX11 ? !G_028C74_FORCE_DST_ALPHA_1_GFX11(cb->ac.cb_color_attrib)
                                                : !G_028C74_FORCE_DST_ALPHA_1_GFX6(cb->ac.cb_color_attrib);

      uint32_t spi_format = (cmd_buffer->state.spi_shader_col_format >> (i * 4)) & 0xf;
      uint32_t colormask = (d->color_write_mask >> (4 * i)) & 0xfu;

      if (format == V_028C70_COLOR_8 || format == V_028C70_COLOR_16 || format == V_028C70_COLOR_32)
         has_rgb = !has_alpha;
      else
         has_rgb = true;

      /* Check the colormask and export format. */
      if (!(colormask & 0x7))
         has_rgb = false;
      if (!(colormask & 0x8))
         has_alpha = false;

      if (spi_format == V_028714_SPI_SHADER_ZERO) {
         has_rgb = false;
         has_alpha = false;
      }

      /* Disable value checking for disabled channels. */
      if (!has_rgb)
         sx_blend_opt_control |= S_02875C_MRT0_COLOR_OPT_DISABLE(1) << (i * 4);
      if (!has_alpha)
         sx_blend_opt_control |= S_02875C_MRT0_ALPHA_OPT_DISABLE(1) << (i * 4);

      /* Enable down-conversion for 32bpp and smaller formats. */
      switch (format) {
      case V_028C70_COLOR_8:
      case V_028C70_COLOR_8_8:
      case V_028C70_COLOR_8_8_8_8:
         /* For 1 and 2-channel formats, use the superset thereof. */
         if (spi_format == V_028714_SPI_SHADER_FP16_ABGR || spi_format == V_028714_SPI_SHADER_UINT16_ABGR ||
             spi_format == V_028714_SPI_SHADER_SINT16_ABGR) {
            sx_ps_downconvert |= V_028754_SX_RT_EXPORT_8_8_8_8 << (i * 4);

            if (G_028C70_NUMBER_TYPE(cb->ac.cb_color_info) != V_028C70_NUMBER_SRGB)
               sx_blend_opt_epsilon |= V_028758_8BIT_FORMAT_0_5 << (i * 4);
         }
         break;

      case V_028C70_COLOR_5_6_5:
         if (spi_format == V_028714_SPI_SHADER_FP16_ABGR) {
            sx_ps_downconvert |= V_028754_SX_RT_EXPORT_5_6_5 << (i * 4);
            sx_blend_opt_epsilon |= V_028758_6BIT_FORMAT_0_5 << (i * 4);
         }
         break;

      case V_028C70_COLOR_1_5_5_5:
         if (spi_format == V_028714_SPI_SHADER_FP16_ABGR) {
            sx_ps_downconvert |= V_028754_SX_RT_EXPORT_1_5_5_5 << (i * 4);
            sx_blend_opt_epsilon |= V_028758_5BIT_FORMAT_0_5 << (i * 4);
         }
         break;

      case V_028C70_COLOR_4_4_4_4:
         if (spi_format == V_028714_SPI_SHADER_FP16_ABGR) {
            sx_ps_downconvert |= V_028754_SX_RT_EXPORT_4_4_4_4 << (i * 4);
            sx_blend_opt_epsilon |= V_028758_4BIT_FORMAT_0_5 << (i * 4);
         }
         break;

      case V_028C70_COLOR_32:
         if (swap == V_028C70_SWAP_STD && spi_format == V_028714_SPI_SHADER_32_R)
            sx_ps_downconvert |= V_028754_SX_RT_EXPORT_32_R << (i * 4);
         else if (swap == V_028C70_SWAP_ALT_REV && spi_format == V_028714_SPI_SHADER_32_AR)
            sx_ps_downconvert |= V_028754_SX_RT_EXPORT_32_A << (i * 4);
         break;

      case V_028C70_COLOR_16:
      case V_028C70_COLOR_16_16:
         /* For 1-channel formats, use the superset thereof. */
         if (spi_format == V_028714_SPI_SHADER_UNORM16_ABGR || spi_format == V_028714_SPI_SHADER_SNORM16_ABGR ||
             spi_format == V_028714_SPI_SHADER_UINT16_ABGR || spi_format == V_028714_SPI_SHADER_SINT16_ABGR) {
            if (swap == V_028C70_SWAP_STD || swap == V_028C70_SWAP_STD_REV)
               sx_ps_downconvert |= V_028754_SX_RT_EXPORT_16_16_GR << (i * 4);
            else
               sx_ps_downconvert |= V_028754_SX_RT_EXPORT_16_16_AR << (i * 4);
         }
         break;

      case V_028C70_COLOR_10_11_11:
         if (spi_format == V_028714_SPI_SHADER_FP16_ABGR)
            sx_ps_downconvert |= V_028754_SX_RT_EXPORT_10_11_11 << (i * 4);
         break;

      case V_028C70_COLOR_2_10_10_10:
         if (spi_format == V_028714_SPI_SHADER_FP16_ABGR) {
            sx_ps_downconvert |= V_028754_SX_RT_EXPORT_2_10_10_10 << (i * 4);
            sx_blend_opt_epsilon |= V_028758_10BIT_FORMAT_0_5 << (i * 4);
         }
         break;
      case V_028C70_COLOR_5_9_9_9:
         if (spi_format == V_028714_SPI_SHADER_FP16_ABGR) {
            if (pdev->info.gfx_level >= GFX12) {
               sx_ps_downconvert |= V_028754_SX_RT_EXPORT_9_9_9_E5 << (i * 4);
            } else if (pdev->info.gfx_level >= GFX10_3) {
               if (colormask == 0xf) {
                  sx_ps_downconvert |= V_028754_SX_RT_EXPORT_9_9_9_E5 << (i * 4);
               } else {
                  /* On GFX10_3+, RB+ with E5B9G9R9 seems broken in the hardware when not all
                   * channels are written. Disable RB+ to workaround it.
                   */
                  sx_ps_downconvert |= V_028754_SX_RT_EXPORT_NO_CONVERSION << (i * 4);
               }
            }
         }
         break;
      }
   }

   /* If there are no color outputs, the first color export is always enabled as 32_R, so also set
    * this to enable RB+.
    */
   if (!sx_ps_downconvert)
      sx_ps_downconvert = V_028754_SX_RT_EXPORT_32_R;

   /* Do not set the DISABLE bits for the unused attachments, as that
    * breaks dual source blending in SkQP and does not seem to improve
    * performance. */

   radeon_begin(cs);
   radeon_opt_set_context_reg3(R_028754_SX_PS_DOWNCONVERT, RADV_TRACKED_SX_PS_DOWNCONVERT, sx_ps_downconvert,
                               sx_blend_opt_epsilon, sx_blend_opt_control);
   radeon_end();
}

static void
radv_emit_ps_epilog_state(struct radv_cmd_buffer *cmd_buffer)
{
   struct radv_device *device = radv_cmd_buffer_device(cmd_buffer);
   const struct radv_physical_device *pdev = radv_device_physical(device);
   struct radv_shader *ps_shader = cmd_buffer->state.shaders[MESA_SHADER_FRAGMENT];
   const struct radv_shader_part *ps_epilog = cmd_buffer->state.ps_epilog;
   struct radv_cmd_stream *cs = cmd_buffer->cs;
   uint32_t pgm_rsrc1 = 0;

   /* This state might be dirty with a NULL PS when states are saved/restored for meta operations. */
   if (!ps_shader || !ps_shader->info.ps.has_epilog)
      return;

   assert(ps_shader->config.num_shared_vgprs == 0);
   if (G_00B848_VGPRS(ps_epilog->rsrc1) > G_00B848_VGPRS(ps_shader->config.rsrc1)) {
      pgm_rsrc1 = (ps_shader->config.rsrc1 & C_00B848_VGPRS) | (ps_epilog->rsrc1 & ~C_00B848_VGPRS);
   }

   const uint32_t epilog_pc_offset = radv_get_user_sgpr_loc(ps_shader, AC_UD_EPILOG_PC);

   radeon_begin(cs);
   if (pdev->info.gfx_level >= GFX12) {
      if (pgm_rsrc1)
         gfx12_push_sh_reg(ps_shader->info.regs.pgm_rsrc1, pgm_rsrc1);
      gfx12_push_32bit_pointer(epilog_pc_offset, ps_epilog->va, &pdev->info);
   } else {
      if (pgm_rsrc1)
         radeon_set_sh_reg(ps_shader->info.regs.pgm_rsrc1, pgm_rsrc1);
      radeon_emit_32bit_pointer(epilog_pc_offset, ps_epilog->va, &pdev->info);
   }
   radeon_end();
}

void
radv_emit_compute_shader(const struct radv_physical_device *pdev, struct radv_cmd_stream *cs,
                         const struct radv_shader *shader)
{
   uint64_t va = radv_shader_get_va(shader);

   radeon_begin(cs);
   if (pdev->info.gfx_level >= GFX12) {
      gfx12_push_sh_reg(shader->info.regs.pgm_lo, va >> 8);
      gfx12_push_sh_reg(shader->info.regs.pgm_rsrc1, shader->config.rsrc1);
      gfx12_push_sh_reg(shader->info.regs.pgm_rsrc2, shader->config.rsrc2);
      gfx12_push_sh_reg(shader->info.regs.pgm_rsrc3, shader->config.rsrc3);
      gfx12_push_sh_reg(R_00B854_COMPUTE_RESOURCE_LIMITS, shader->info.regs.cs.compute_resource_limits);
      gfx12_push_sh_reg(R_00B81C_COMPUTE_NUM_THREAD_X, shader->info.regs.cs.compute_num_thread_x);
      gfx12_push_sh_reg(R_00B820_COMPUTE_NUM_THREAD_Y, shader->info.regs.cs.compute_num_thread_y);
      gfx12_push_sh_reg(R_00B824_COMPUTE_NUM_THREAD_Z, shader->info.regs.cs.compute_num_thread_z);
   } else {
      radeon_set_sh_reg(shader->info.regs.pgm_lo, va >> 8);
      radeon_set_sh_reg_seq(shader->info.regs.pgm_rsrc1, 2);
      radeon_emit(shader->config.rsrc1);
      radeon_emit(shader->config.rsrc2);
      if (pdev->info.gfx_level >= GFX10)
         radeon_set_sh_reg(shader->info.regs.pgm_rsrc3, shader->config.rsrc3);

      radeon_set_sh_reg(R_00B854_COMPUTE_RESOURCE_LIMITS, shader->info.regs.cs.compute_resource_limits);
      radeon_set_sh_reg_seq(R_00B81C_COMPUTE_NUM_THREAD_X, 3);
      radeon_emit(shader->info.regs.cs.compute_num_thread_x);
      radeon_emit(shader->info.regs.cs.compute_num_thread_y);
      radeon_emit(shader->info.regs.cs.compute_num_thread_z);
   }
   radeon_end();
}

static void
radv_emit_vgt_gs_mode(struct radv_cmd_buffer *cmd_buffer)
{
   const struct radv_device *device = radv_cmd_buffer_device(cmd_buffer);
   const struct radv_physical_device *pdev = radv_device_physical(device);
   const struct radv_shader_info *info = &cmd_buffer->state.last_vgt_shader->info;
   struct radv_cmd_stream *cs = cmd_buffer->cs;
   unsigned vgt_primitiveid_en = 0;
   uint32_t vgt_gs_mode = 0;

   if (info->is_ngg)
      return;

   if (info->stage == MESA_SHADER_GEOMETRY) {
      vgt_gs_mode = ac_vgt_gs_mode(info->gs.vertices_out, pdev->info.gfx_level);
   } else if (info->outinfo.export_prim_id || info->uses_prim_id) {
      vgt_gs_mode = S_028A40_MODE(V_028A40_GS_SCENARIO_A);
      vgt_primitiveid_en |= S_028A84_PRIMITIVEID_EN(1);
   }

   radeon_begin(cs);
   radeon_opt_set_context_reg(R_028A84_VGT_PRIMITIVEID_EN, RADV_TRACKED_VGT_PRIMITIVEID_EN, vgt_primitiveid_en);
   radeon_opt_set_context_reg(R_028A40_VGT_GS_MODE, RADV_TRACKED_VGT_GS_MODE, vgt_gs_mode);
   radeon_end();
}

static void
radv_emit_hw_vs(struct radv_cmd_buffer *cmd_buffer, const struct radv_shader *shader)
{
   const struct radv_device *device = radv_cmd_buffer_device(cmd_buffer);
   const struct radv_physical_device *pdev = radv_device_physical(device);
   struct radv_cmd_stream *cs = cmd_buffer->cs;
   const uint64_t va = radv_shader_get_va(shader);

   radeon_begin(cs);
   radeon_set_sh_reg_seq(shader->info.regs.pgm_lo, 4);
   radeon_emit(va >> 8);
   radeon_emit(S_00B124_MEM_BASE(va >> 40));
   radeon_emit(shader->config.rsrc1);
   radeon_emit(shader->config.rsrc2);

   radeon_opt_set_context_reg(R_0286C4_SPI_VS_OUT_CONFIG, RADV_TRACKED_SPI_VS_OUT_CONFIG,
                              shader->info.regs.spi_vs_out_config);
   radeon_opt_set_context_reg(R_02870C_SPI_SHADER_POS_FORMAT, RADV_TRACKED_SPI_SHADER_POS_FORMAT,
                              shader->info.regs.spi_shader_pos_format);
   radeon_opt_set_context_reg(R_02881C_PA_CL_VS_OUT_CNTL, RADV_TRACKED_PA_CL_VS_OUT_CNTL,
                              shader->info.regs.pa_cl_vs_out_cntl);

   if (pdev->info.gfx_level <= GFX8)
      radeon_opt_set_context_reg(R_028AB4_VGT_REUSE_OFF, RADV_TRACKED_VGT_REUSE_OFF,
                                 shader->info.regs.vs.vgt_reuse_off);

   if (pdev->info.gfx_level >= GFX7) {
      radeon_set_sh_reg_idx(&pdev->info, R_00B118_SPI_SHADER_PGM_RSRC3_VS, 3,
                            shader->info.regs.vs.spi_shader_pgm_rsrc3_vs);
      radeon_set_sh_reg(R_00B11C_SPI_SHADER_LATE_ALLOC_VS, shader->info.regs.vs.spi_shader_late_alloc_vs);

      if (pdev->info.gfx_level >= GFX10) {
         radeon_set_uconfig_reg(R_030980_GE_PC_ALLOC, shader->info.regs.ge_pc_alloc);

         if (shader->info.stage == MESA_SHADER_TESS_EVAL) {
            radeon_opt_set_context_reg(R_028A44_VGT_GS_ONCHIP_CNTL, RADV_TRACKED_VGT_GS_ONCHIP_CNTL,
                                       shader->info.regs.vgt_gs_onchip_cntl);
         }
      }
   }

   radeon_end();
}

static void
radv_emit_hw_es(struct radv_cmd_buffer *cmd_buffer, const struct radv_shader *shader)
{
   const struct radv_device *device = radv_cmd_buffer_device(cmd_buffer);
   const struct radv_physical_device *pdev = radv_device_physical(device);
   const uint64_t va = radv_shader_get_va(shader);
   struct radv_cmd_stream *cs = cmd_buffer->cs;

   assert(pdev->info.gfx_level < GFX11);

   radeon_begin(cs);
   radeon_set_sh_reg_seq(shader->info.regs.pgm_lo, 4);
   radeon_emit(va >> 8);
   radeon_emit(S_00B324_MEM_BASE(va >> 40));
   radeon_emit(shader->config.rsrc1);
   radeon_emit(shader->config.rsrc2);
   radeon_end();
}

static void
radv_emit_hw_ls(struct radv_cmd_buffer *cmd_buffer, const struct radv_shader *shader)
{
   const struct radv_device *device = radv_cmd_buffer_device(cmd_buffer);
   const struct radv_physical_device *pdev = radv_device_physical(device);
   struct radv_cmd_stream *cs = cmd_buffer->cs;
   const uint64_t va = radv_shader_get_va(shader);

   radeon_begin(cs);
   if (pdev->info.gfx_level >= GFX12) {
      gfx12_push_sh_reg(shader->info.regs.pgm_lo, va >> 8);
      gfx12_push_sh_reg(shader->info.regs.pgm_rsrc1, shader->config.rsrc1);
   } else {
      radeon_set_sh_reg(shader->info.regs.pgm_lo, va >> 8);
      radeon_set_sh_reg(shader->info.regs.pgm_rsrc1, shader->config.rsrc1);
   }
   radeon_end();
}

static void
radv_emit_hw_ngg(struct radv_cmd_buffer *cmd_buffer, const struct radv_shader *es, const struct radv_shader *shader)
{
   const struct radv_device *device = radv_cmd_buffer_device(cmd_buffer);
   const struct radv_physical_device *pdev = radv_device_physical(device);
   struct radv_cmd_stream *cs = cmd_buffer->cs;
   const uint64_t va = radv_shader_get_va(shader);
   mesa_shader_stage es_type;
   const struct gfx10_ngg_info *ngg_state = &shader->info.ngg_info;

   if (shader->info.stage == MESA_SHADER_GEOMETRY) {
      if (shader->info.merged_shader_compiled_separately) {
         es_type = es->info.stage;
      } else {
         es_type = shader->info.gs.es_type;
      }
   } else {
      es_type = shader->info.stage;
   }

   if (!shader->info.merged_shader_compiled_separately) {
      radeon_begin(cs);
      if (pdev->info.gfx_level >= GFX12) {
         gfx12_push_sh_reg(shader->info.regs.pgm_lo, va >> 8);
         gfx12_push_sh_reg(shader->info.regs.pgm_rsrc1, shader->config.rsrc1);
         gfx12_push_sh_reg(shader->info.regs.pgm_rsrc2, shader->config.rsrc2);
         gfx12_push_sh_reg(R_00B220_SPI_SHADER_PGM_RSRC4_GS, shader->info.regs.spi_shader_pgm_rsrc4_gs);
      } else {
         radeon_set_sh_reg(shader->info.regs.pgm_lo, va >> 8);
         radeon_set_sh_reg_seq(shader->info.regs.pgm_rsrc1, 2);
         radeon_emit(shader->config.rsrc1);
         radeon_emit(shader->config.rsrc2);
      }
      radeon_end();
   }

   const struct radv_vs_output_info *outinfo = &shader->info.outinfo;

   const bool es_enable_prim_id = outinfo->export_prim_id || (es && es->info.uses_prim_id);
   bool break_wave_at_eoi = false;

   if (es_type == MESA_SHADER_TESS_EVAL) {
      if (es_enable_prim_id || (shader->info.uses_prim_id))
         break_wave_at_eoi = true;
   }

   if (pdev->info.gfx_level >= GFX12) {
      radeon_begin(cs);
      gfx12_begin_context_regs();
      gfx12_opt_set_context_reg(R_028818_PA_CL_VS_OUT_CNTL, RADV_TRACKED_PA_CL_VS_OUT_CNTL,
                                shader->info.regs.pa_cl_vs_out_cntl);
      gfx12_opt_set_context_reg(R_028B3C_VGT_GS_INSTANCE_CNT, RADV_TRACKED_VGT_GS_INSTANCE_CNT,
                                shader->info.regs.vgt_gs_instance_cnt);
      gfx12_opt_set_context_reg2(R_028648_SPI_SHADER_IDX_FORMAT, RADV_TRACKED_SPI_SHADER_IDX_FORMAT,
                                 shader->info.regs.ngg.spi_shader_idx_format, shader->info.regs.spi_shader_pos_format);
      gfx12_opt_set_context_reg(R_0287FC_GE_MAX_OUTPUT_PER_SUBGROUP, RADV_TRACKED_GE_MAX_OUTPUT_PER_SUBGROUP,
                                shader->info.regs.ngg.ge_max_output_per_subgroup);
      gfx12_opt_set_context_reg(R_028B4C_GE_NGG_SUBGRP_CNTL, RADV_TRACKED_GE_NGG_SUBGRP_CNTL,
                                shader->info.regs.ngg.ge_ngg_subgrp_cntl);
      gfx12_end_context_regs();
      radeon_end();
   } else {
      radeon_begin(cs);
      radeon_opt_set_context_reg(R_02881C_PA_CL_VS_OUT_CNTL, RADV_TRACKED_PA_CL_VS_OUT_CNTL,
                                 shader->info.regs.pa_cl_vs_out_cntl);
      radeon_opt_set_context_reg(R_028B90_VGT_GS_INSTANCE_CNT, RADV_TRACKED_VGT_GS_INSTANCE_CNT,
                                 shader->info.regs.vgt_gs_instance_cnt);
      radeon_opt_set_context_reg(R_028A84_VGT_PRIMITIVEID_EN, RADV_TRACKED_VGT_PRIMITIVEID_EN,
                                 shader->info.regs.ngg.vgt_primitiveid_en | S_028A84_PRIMITIVEID_EN(es_enable_prim_id));
      radeon_opt_set_context_reg2(R_028708_SPI_SHADER_IDX_FORMAT, RADV_TRACKED_SPI_SHADER_IDX_FORMAT,
                                  shader->info.regs.ngg.spi_shader_idx_format, shader->info.regs.spi_shader_pos_format);
      radeon_opt_set_context_reg(R_0286C4_SPI_VS_OUT_CONFIG, RADV_TRACKED_SPI_VS_OUT_CONFIG,
                                 shader->info.regs.spi_vs_out_config);
      radeon_opt_set_context_reg(R_0287FC_GE_MAX_OUTPUT_PER_SUBGROUP, RADV_TRACKED_GE_MAX_OUTPUT_PER_SUBGROUP,
                                 shader->info.regs.ngg.ge_max_output_per_subgroup);
      radeon_opt_set_context_reg(R_028B4C_GE_NGG_SUBGRP_CNTL, RADV_TRACKED_GE_NGG_SUBGRP_CNTL,
                                 shader->info.regs.ngg.ge_ngg_subgrp_cntl);
      radeon_end();
   }

   radeon_begin(cs);

   uint32_t ge_cntl = shader->info.regs.ngg.ge_cntl;
   if (pdev->info.gfx_level >= GFX11) {
      ge_cntl |= S_03096C_BREAK_PRIMGRP_AT_EOI(break_wave_at_eoi);
   } else {
      ge_cntl |= S_03096C_BREAK_WAVE_AT_EOI(break_wave_at_eoi);

      /* Bug workaround for a possible hang with non-tessellation cases.
       * Tessellation always sets GE_CNTL.VERT_GRP_SIZE = 0
       *
       * Requirement: GE_CNTL.VERT_GRP_SIZE = VGT_GS_ONCHIP_CNTL.ES_VERTS_PER_SUBGRP - 5
       */
      if (pdev->info.gfx_level == GFX10 && es_type != MESA_SHADER_TESS_EVAL && ngg_state->hw_max_esverts != 256) {
         ge_cntl &= C_03096C_VERT_GRP_SIZE;

         if (ngg_state->hw_max_esverts > 5) {
            ge_cntl |= S_03096C_VERT_GRP_SIZE(ngg_state->hw_max_esverts - 5);
         }
      }

      radeon_opt_set_context_reg(R_028A44_VGT_GS_ONCHIP_CNTL, RADV_TRACKED_VGT_GS_ONCHIP_CNTL,
                                 shader->info.regs.vgt_gs_onchip_cntl);
   }

   radeon_set_uconfig_reg(R_03096C_GE_CNTL, ge_cntl);

   const uint32_t ngg_lds_layout_offset = radv_get_user_sgpr_loc(shader, AC_UD_NGG_LDS_LAYOUT);
   assert(ngg_lds_layout_offset);
   assert(!(shader->info.ngg_info.esgs_ring_size & 0xffff0000));

   if (pdev->info.gfx_level >= GFX12) {
      radeon_set_uconfig_reg(R_030988_VGT_PRIMITIVEID_EN, shader->info.regs.ngg.vgt_primitiveid_en);
      gfx12_push_sh_reg(ngg_lds_layout_offset,
                        SET_SGPR_FIELD(NGG_LDS_LAYOUT_GS_OUT_VERTEX_BASE, shader->info.ngg_info.esgs_ring_size));
   } else {
      if (pdev->info.gfx_level >= GFX7) {
         radeon_set_sh_reg_idx(&pdev->info, R_00B21C_SPI_SHADER_PGM_RSRC3_GS, 3,
                               shader->info.regs.spi_shader_pgm_rsrc3_gs);
      }

      radeon_set_sh_reg_idx(&pdev->info, R_00B204_SPI_SHADER_PGM_RSRC4_GS, 3,
                            shader->info.regs.spi_shader_pgm_rsrc4_gs);

      radeon_set_uconfig_reg(R_030980_GE_PC_ALLOC, shader->info.regs.ge_pc_alloc);

      radeon_set_sh_reg(ngg_lds_layout_offset,
                        SET_SGPR_FIELD(NGG_LDS_LAYOUT_GS_OUT_VERTEX_BASE, shader->info.ngg_info.esgs_ring_size));
   }

   radeon_end();
}

static void
radv_emit_hw_hs(struct radv_cmd_buffer *cmd_buffer, const struct radv_shader *shader)
{
   const struct radv_device *device = radv_cmd_buffer_device(cmd_buffer);
   const struct radv_physical_device *pdev = radv_device_physical(device);
   struct radv_cmd_stream *cs = cmd_buffer->cs;
   const uint64_t va = radv_shader_get_va(shader);

   radeon_begin(cs);
   if (pdev->info.gfx_level >= GFX12) {
      gfx12_push_sh_reg(shader->info.regs.pgm_lo, va >> 8);
      gfx12_push_sh_reg(shader->info.regs.pgm_rsrc1, shader->config.rsrc1);
   } else {
      if (pdev->info.gfx_level >= GFX9) {
         radeon_set_sh_reg(shader->info.regs.pgm_lo, va >> 8);
         radeon_set_sh_reg(shader->info.regs.pgm_rsrc1, shader->config.rsrc1);
      } else {
         radeon_set_sh_reg_seq(shader->info.regs.pgm_lo, 4);
         radeon_emit(va >> 8);
         radeon_emit(S_00B424_MEM_BASE(va >> 40));
         radeon_emit(shader->config.rsrc1);
         radeon_emit(shader->config.rsrc2);
      }
   }
   radeon_end();
}

static void
radv_emit_vertex_shader(struct radv_cmd_buffer *cmd_buffer)
{
   const struct radv_device *device = radv_cmd_buffer_device(cmd_buffer);
   const struct radv_physical_device *pdev = radv_device_physical(device);
   const struct radv_shader *vs = cmd_buffer->state.shaders[MESA_SHADER_VERTEX];
   struct radv_cmd_stream *cs = cmd_buffer->cs;

   if (vs->info.merged_shader_compiled_separately) {
      assert(vs->info.next_stage == MESA_SHADER_TESS_CTRL || vs->info.next_stage == MESA_SHADER_GEOMETRY);

      const struct radv_shader *next_stage = cmd_buffer->state.shaders[vs->info.next_stage];
      uint32_t rsrc1, rsrc2;

      if (!vs->info.vs.has_prolog) {
         if (vs->info.next_stage == MESA_SHADER_TESS_CTRL) {
            radv_shader_combine_cfg_vs_tcs(vs, next_stage, &rsrc1, NULL);
         } else {
            radv_shader_combine_cfg_vs_gs(device, vs, next_stage, &rsrc1, &rsrc2);
         }
      }

      const uint32_t next_stage_pc_offset = radv_get_user_sgpr_loc(vs, AC_UD_NEXT_STAGE_PC);

      radeon_begin(cs);
      if (pdev->info.gfx_level >= GFX12) {
         gfx12_push_32bit_pointer(next_stage_pc_offset, next_stage->va, &pdev->info);

         if (!vs->info.vs.has_prolog) {
            gfx12_push_sh_reg(vs->info.regs.pgm_lo, vs->va >> 8);
            if (vs->info.next_stage == MESA_SHADER_TESS_CTRL) {
               gfx12_push_sh_reg(vs->info.regs.pgm_rsrc1, rsrc1);
            } else {
               gfx12_push_sh_reg(vs->info.regs.pgm_rsrc1, rsrc1);
               gfx12_push_sh_reg(vs->info.regs.pgm_rsrc2, rsrc2);
            }
         }
      } else {
         radeon_emit_32bit_pointer(next_stage_pc_offset, next_stage->va, &pdev->info);

         if (!vs->info.vs.has_prolog) {
            radeon_set_sh_reg(vs->info.regs.pgm_lo, vs->va >> 8);
            if (vs->info.next_stage == MESA_SHADER_TESS_CTRL) {
               radeon_set_sh_reg(vs->info.regs.pgm_rsrc1, rsrc1);
            } else {
               radeon_set_sh_reg_seq(vs->info.regs.pgm_rsrc1, 2);
               radeon_emit(rsrc1);
               radeon_emit(rsrc2);
            }
         }
      }
      radeon_end();
      return;
   }

   if (vs->info.vs.as_ls)
      radv_emit_hw_ls(cmd_buffer, vs);
   else if (vs->info.vs.as_es)
      radv_emit_hw_es(cmd_buffer, vs);
   else if (vs->info.is_ngg)
      radv_emit_hw_ngg(cmd_buffer, NULL, vs);
   else
      radv_emit_hw_vs(cmd_buffer, vs);
}

static void
radv_emit_tess_ctrl_shader(struct radv_cmd_buffer *cmd_buffer)
{
   const struct radv_shader *tcs = cmd_buffer->state.shaders[MESA_SHADER_TESS_CTRL];

   if (tcs->info.merged_shader_compiled_separately) {
      /* When VS+TCS are compiled separately on GFX9+, the VS will jump to the TCS and everything is
       * emitted as part of the VS.
       */
      return;
   }

   radv_emit_hw_hs(cmd_buffer, tcs);
}

static void
radv_emit_tess_eval_shader(struct radv_cmd_buffer *cmd_buffer)
{
   const struct radv_device *device = radv_cmd_buffer_device(cmd_buffer);
   const struct radv_physical_device *pdev = radv_device_physical(device);
   const struct radv_shader *tes = cmd_buffer->state.shaders[MESA_SHADER_TESS_EVAL];
   struct radv_cmd_stream *cs = cmd_buffer->cs;

   if (tes->info.merged_shader_compiled_separately) {
      assert(tes->info.next_stage == MESA_SHADER_GEOMETRY);

      const struct radv_shader *gs = cmd_buffer->state.shaders[MESA_SHADER_GEOMETRY];
      uint32_t rsrc1, rsrc2;

      radv_shader_combine_cfg_tes_gs(device, tes, gs, &rsrc1, &rsrc2);

      const uint32_t next_stage_pc_offset = radv_get_user_sgpr_loc(tes, AC_UD_NEXT_STAGE_PC);

      radeon_begin(cs);
      if (pdev->info.gfx_level >= GFX12) {
         gfx12_push_sh_reg(tes->info.regs.pgm_lo, tes->va >> 8);
         gfx12_push_sh_reg(tes->info.regs.pgm_rsrc1, rsrc1);
         gfx12_push_sh_reg(tes->info.regs.pgm_rsrc2, rsrc2);
         gfx12_push_32bit_pointer(next_stage_pc_offset, gs->va, &pdev->info);
      } else {
         radeon_set_sh_reg(tes->info.regs.pgm_lo, tes->va >> 8);
         radeon_set_sh_reg_seq(tes->info.regs.pgm_rsrc1, 2);
         radeon_emit(rsrc1);
         radeon_emit(rsrc2);
         radeon_emit_32bit_pointer(next_stage_pc_offset, gs->va, &pdev->info);
      }
      radeon_end();
      return;
   }

   if (tes->info.is_ngg) {
      radv_emit_hw_ngg(cmd_buffer, NULL, tes);
   } else if (tes->info.tes.as_es) {
      radv_emit_hw_es(cmd_buffer, tes);
   } else {
      radv_emit_hw_vs(cmd_buffer, tes);
   }
}

static void
radv_emit_hw_gs(struct radv_cmd_buffer *cmd_buffer, const struct radv_shader *gs)
{
   const struct radv_device *device = radv_cmd_buffer_device(cmd_buffer);
   const struct radv_physical_device *pdev = radv_device_physical(device);
   const struct radv_legacy_gs_info *gs_state = &gs->info.gs_ring_info;
   struct radv_cmd_stream *cs = cmd_buffer->cs;
   const uint64_t va = radv_shader_get_va(gs);

   radeon_begin(cs);

   radeon_opt_set_context_reg3(R_028A60_VGT_GSVS_RING_OFFSET_1, RADV_TRACKED_VGT_GSVS_RING_OFFSET_1,
                               gs->info.regs.gs.vgt_gsvs_ring_offset[0], gs->info.regs.gs.vgt_gsvs_ring_offset[1],
                               gs->info.regs.gs.vgt_gsvs_ring_offset[2]);

   radeon_opt_set_context_reg(R_028AB0_VGT_GSVS_RING_ITEMSIZE, RADV_TRACKED_VGT_GSVS_RING_ITEMSIZE,
                              gs->info.regs.gs.vgt_gsvs_ring_itemsize);

   radeon_opt_set_context_reg4(R_028B5C_VGT_GS_VERT_ITEMSIZE, RADV_TRACKED_VGT_GS_VERT_ITEMSIZE,
                               gs->info.regs.gs.vgt_gs_vert_itemsize[0], gs->info.regs.gs.vgt_gs_vert_itemsize[1],
                               gs->info.regs.gs.vgt_gs_vert_itemsize[2], gs->info.regs.gs.vgt_gs_vert_itemsize[3]);

   radeon_opt_set_context_reg(R_028B90_VGT_GS_INSTANCE_CNT, RADV_TRACKED_VGT_GS_INSTANCE_CNT,
                              gs->info.regs.gs.vgt_gs_instance_cnt);

   if (pdev->info.gfx_level >= GFX9) {
      if (!gs->info.merged_shader_compiled_separately) {
         radeon_set_sh_reg(gs->info.regs.pgm_lo, va >> 8);

         radeon_set_sh_reg_seq(gs->info.regs.pgm_rsrc1, 2);
         radeon_emit(gs->config.rsrc1);
         radeon_emit(gs->config.rsrc2 | S_00B22C_LDS_SIZE(ac_shader_encode_lds_size(
                                           gs_state->lds_size, pdev->info.gfx_level, MESA_SHADER_GEOMETRY)));
      }

      radeon_opt_set_context_reg(R_028A44_VGT_GS_ONCHIP_CNTL, RADV_TRACKED_VGT_GS_ONCHIP_CNTL,
                                 gs->info.regs.vgt_gs_onchip_cntl);

      if (pdev->info.gfx_level == GFX9) {
         radeon_opt_set_context_reg(R_028A94_VGT_GS_MAX_PRIMS_PER_SUBGROUP, RADV_TRACKED_VGT_GS_MAX_PRIMS_PER_SUBGROUP,
                                    gs->info.regs.gs.vgt_gs_max_prims_per_subgroup);
      }
   } else {
      radeon_set_sh_reg_seq(gs->info.regs.pgm_lo, 4);
      radeon_emit(va >> 8);
      radeon_emit(S_00B224_MEM_BASE(va >> 40));
      radeon_emit(gs->config.rsrc1);
      radeon_emit(gs->config.rsrc2);

      /* GFX6-8: ESGS offchip ring buffer is allocated according to VGT_ESGS_RING_ITEMSIZE.
       * GFX9+: Only used to set the GS input VGPRs, emulated in shaders.
       */
      radeon_opt_set_context_reg(R_028AAC_VGT_ESGS_RING_ITEMSIZE, RADV_TRACKED_VGT_ESGS_RING_ITEMSIZE,
                                 gs->info.regs.gs.vgt_esgs_ring_itemsize);
   }

   if (pdev->info.gfx_level >= GFX7) {
      radeon_set_sh_reg_idx(&pdev->info, R_00B21C_SPI_SHADER_PGM_RSRC3_GS, 3, gs->info.regs.spi_shader_pgm_rsrc3_gs);
   }

   if (pdev->info.gfx_level >= GFX10) {
      radeon_set_sh_reg_idx(&pdev->info, R_00B204_SPI_SHADER_PGM_RSRC4_GS, 3, gs->info.regs.spi_shader_pgm_rsrc4_gs);
   }

   radeon_end();
}

static void
radv_emit_geometry_shader(struct radv_cmd_buffer *cmd_buffer)
{
   const struct radv_shader *gs = cmd_buffer->state.shaders[MESA_SHADER_GEOMETRY];
   const struct radv_shader *es = cmd_buffer->state.shaders[MESA_SHADER_TESS_EVAL]
                                     ? cmd_buffer->state.shaders[MESA_SHADER_TESS_EVAL]
                                     : cmd_buffer->state.shaders[MESA_SHADER_VERTEX];
   struct radv_cmd_stream *cs = cmd_buffer->cs;

   if (gs->info.is_ngg) {
      radv_emit_hw_ngg(cmd_buffer, es, gs);
   } else {
      radv_emit_hw_gs(cmd_buffer, gs);
      radv_emit_hw_vs(cmd_buffer, cmd_buffer->state.gs_copy_shader);
   }

   radeon_begin(cs);

   radeon_opt_set_context_reg(R_028B38_VGT_GS_MAX_VERT_OUT, RADV_TRACKED_VGT_GS_MAX_VERT_OUT,
                              gs->info.regs.vgt_gs_max_vert_out);

   if (gs->info.merged_shader_compiled_separately) {
      const uint32_t vgt_esgs_ring_itemsize_offset = radv_get_user_sgpr_loc(gs, AC_UD_VGT_ESGS_RING_ITEMSIZE);
      assert(vgt_esgs_ring_itemsize_offset);

      radeon_set_sh_reg(vgt_esgs_ring_itemsize_offset, es->info.esgs_itemsize / 4);
   }

   radeon_end();
}

static void
radv_emit_vgt_gs_out(struct radv_cmd_buffer *cmd_buffer, uint32_t vgt_gs_out_prim_type)
{
   const struct radv_device *device = radv_cmd_buffer_device(cmd_buffer);
   const struct radv_physical_device *pdev = radv_device_physical(device);
   struct radv_cmd_stream *cs = cmd_buffer->cs;

   radeon_begin(cs);
   if (pdev->info.gfx_level >= GFX11) {
      radeon_set_uconfig_reg(R_030998_VGT_GS_OUT_PRIM_TYPE, vgt_gs_out_prim_type);
   } else {
      radeon_opt_set_context_reg(R_028A6C_VGT_GS_OUT_PRIM_TYPE, RADV_TRACKED_VGT_GS_OUT_PRIM_TYPE,
                                 vgt_gs_out_prim_type);
   }
   radeon_end();
}

static void
radv_gfx11_emit_meshlet(struct radv_cmd_buffer *cmd_buffer, const struct radv_shader *ms)
{
   const struct radv_device *device = radv_cmd_buffer_device(cmd_buffer);
   const struct radv_physical_device *pdev = radv_device_physical(device);
   struct radv_cmd_stream *cs = cmd_buffer->cs;

   assert(pdev->info.gfx_level >= GFX11);

   radeon_begin(cs);
   if (pdev->info.gfx_level >= GFX12) {
      gfx12_push_sh_reg(R_00B2B0_SPI_SHADER_GS_MESHLET_DIM, ms->info.regs.ms.spi_shader_gs_meshlet_dim);
      gfx12_push_sh_reg(R_00B2B4_SPI_SHADER_GS_MESHLET_EXP_ALLOC, ms->info.regs.ms.spi_shader_gs_meshlet_exp_alloc);
      gfx12_push_sh_reg(R_00B2B8_SPI_SHADER_GS_MESHLET_CTRL, ms->info.regs.ms.spi_shader_gs_meshlet_ctrl);
   } else {
      radeon_set_sh_reg_seq(R_00B2B0_SPI_SHADER_GS_MESHLET_DIM, 2);
      radeon_emit(ms->info.regs.ms.spi_shader_gs_meshlet_dim);
      radeon_emit(ms->info.regs.ms.spi_shader_gs_meshlet_exp_alloc);
   }
   radeon_end();
}

static void
radv_emit_mesh_shader(struct radv_cmd_buffer *cmd_buffer)
{
   const struct radv_device *device = radv_cmd_buffer_device(cmd_buffer);
   const struct radv_physical_device *pdev = radv_device_physical(device);
   const struct radv_shader *ms = cmd_buffer->state.shaders[MESA_SHADER_MESH];
   const uint32_t gs_out = radv_conv_gl_prim_to_gs_out(ms->info.ms.output_prim);
   struct radv_cmd_stream *cs = cmd_buffer->cs;

   radv_emit_hw_ngg(cmd_buffer, NULL, ms);

   radeon_begin(cs);

   radeon_opt_set_context_reg(R_028B38_VGT_GS_MAX_VERT_OUT, RADV_TRACKED_VGT_GS_MAX_VERT_OUT,
                              ms->info.regs.vgt_gs_max_vert_out);
   radeon_set_uconfig_reg_idx(&pdev->info, R_030908_VGT_PRIMITIVE_TYPE, 1, V_008958_DI_PT_POINTLIST);
   radeon_end();

   if (pdev->info.mesh_fast_launch_2)
      radv_gfx11_emit_meshlet(cmd_buffer, ms);

   radv_emit_vgt_gs_out(cmd_buffer, gs_out);
}

enum radv_ps_in_type {
   radv_ps_in_interpolated,
   radv_ps_in_flat,
   radv_ps_in_explicit,
   radv_ps_in_explicit_strict,
   radv_ps_in_interpolated_fp16,
   radv_ps_in_interpolated_fp16_hi,
   radv_ps_in_per_prim_gfx103,
   radv_ps_in_per_prim_gfx11,
};

static uint32_t
offset_to_ps_input(const uint32_t offset, const enum radv_ps_in_type type)
{
   if (offset == AC_EXP_PARAM_UNDEFINED) {
      /* The input is UNDEFINED, use zero. */
      return S_028644_OFFSET(0x20) | S_028644_DEFAULT_VAL(0);
   } else if (offset >= AC_EXP_PARAM_DEFAULT_VAL_0000 && offset <= AC_EXP_PARAM_DEFAULT_VAL_1111) {
      /* The input is a DEFAULT_VAL constant. */
      return S_028644_OFFSET(0x20) | S_028644_DEFAULT_VAL(offset - AC_EXP_PARAM_DEFAULT_VAL_0000);
   }

   assert(offset <= AC_EXP_PARAM_OFFSET_31);
   uint32_t ps_input_cntl = S_028644_OFFSET(offset);

   switch (type) {
   case radv_ps_in_explicit_strict:
      /* Rotate parameter cache contents to strict vertex order. */
      ps_input_cntl |= S_028644_ROTATE_PC_PTR(1);
      FALLTHROUGH;
   case radv_ps_in_explicit:
      /* Force parameter cache to be read in passthrough mode. */
      ps_input_cntl |= S_028644_OFFSET(1 << 5);
      FALLTHROUGH;
   case radv_ps_in_flat:
      ps_input_cntl |= S_028644_FLAT_SHADE(1);
      break;
   case radv_ps_in_interpolated_fp16_hi:
      ps_input_cntl |= S_028644_ATTR1_VALID(1);
      FALLTHROUGH;
   case radv_ps_in_interpolated_fp16:
      /* These must be set even if only the high 16 bits are used. */
      ps_input_cntl |= S_028644_FP16_INTERP_MODE(1) | S_028644_ATTR0_VALID(1);
      break;
   case radv_ps_in_per_prim_gfx11:
      ps_input_cntl |= S_028644_PRIM_ATTR(1);
      break;
   case radv_ps_in_interpolated:
   case radv_ps_in_per_prim_gfx103:
      break;
   }

   return ps_input_cntl;
}

static void
input_mask_to_ps_inputs(const struct radv_vs_output_info *outinfo, const struct radv_shader *ps, uint32_t input_mask,
                        uint32_t *ps_input_cntl, unsigned *ps_offset, const enum radv_ps_in_type default_type)
{
   u_foreach_bit (i, input_mask) {
      const unsigned vs_offset = outinfo->vs_output_param_offset[VARYING_SLOT_VAR0 + i];
      enum radv_ps_in_type type = default_type;

      if (ps->info.ps.explicit_shaded_mask & BITFIELD_BIT(*ps_offset))
         type = radv_ps_in_explicit;
      else if (ps->info.ps.explicit_strict_shaded_mask & BITFIELD_BIT(*ps_offset))
         type = radv_ps_in_explicit_strict;
      else if (ps->info.ps.float16_hi_shaded_mask & BITFIELD_BIT(*ps_offset))
         type = radv_ps_in_interpolated_fp16_hi;
      else if (ps->info.ps.float16_shaded_mask & BITFIELD_BIT(*ps_offset))
         type = radv_ps_in_interpolated_fp16;
      else if (ps->info.ps.float32_shaded_mask & BITFIELD_BIT(*ps_offset))
         type = radv_ps_in_interpolated;

      ps_input_cntl[*ps_offset] = offset_to_ps_input(vs_offset, type);
      ++(*ps_offset);
   }
}

static void
radv_emit_ps_inputs(struct radv_cmd_buffer *cmd_buffer)
{
   const struct radv_device *device = radv_cmd_buffer_device(cmd_buffer);
   const struct radv_physical_device *pdev = radv_device_physical(device);
   const struct radv_shader *ps = cmd_buffer->state.shaders[MESA_SHADER_FRAGMENT];
   const struct radv_shader *last_vgt_shader = cmd_buffer->state.last_vgt_shader;
   const struct radv_vs_output_info *outinfo = &last_vgt_shader->info.outinfo;
   const bool gfx11plus = pdev->info.gfx_level >= GFX11;
   const enum radv_ps_in_type per_prim = gfx11plus ? radv_ps_in_per_prim_gfx11 : radv_ps_in_per_prim_gfx103;
   struct radv_cmd_stream *cs = cmd_buffer->cs;
   unsigned num_per_primitive_params = 0;
   uint32_t ps_input_cntl[32];
   unsigned ps_offset = 0;

   if (ps->info.ps.has_pcoord)
      ps_input_cntl[ps_offset++] = S_028644_PT_SPRITE_TEX(1) | S_028644_OFFSET(0x20);

   if (ps->info.ps.input_clips_culls_mask & 0x0f)
      ps_input_cntl[ps_offset++] =
         offset_to_ps_input(outinfo->vs_output_param_offset[VARYING_SLOT_CLIP_DIST0], radv_ps_in_interpolated);

   if (ps->info.ps.input_clips_culls_mask & 0xf0)
      ps_input_cntl[ps_offset++] =
         offset_to_ps_input(outinfo->vs_output_param_offset[VARYING_SLOT_CLIP_DIST1], radv_ps_in_interpolated);

   input_mask_to_ps_inputs(outinfo, ps, ps->info.ps.input_mask, ps_input_cntl, &ps_offset, radv_ps_in_flat);

   /* Potentially per-primitive PS inputs */
   if (ps->info.ps.viewport_index_input) {
      num_per_primitive_params += !!outinfo->writes_viewport_index_per_primitive;
      const enum radv_ps_in_type t = outinfo->writes_viewport_index_per_primitive ? per_prim : radv_ps_in_flat;
      ps_input_cntl[ps_offset++] = offset_to_ps_input(outinfo->vs_output_param_offset[VARYING_SLOT_VIEWPORT], t);
   }
   if (ps->info.ps.prim_id_input) {
      num_per_primitive_params += !!outinfo->export_prim_id_per_primitive;
      const enum radv_ps_in_type t = outinfo->export_prim_id_per_primitive ? per_prim : radv_ps_in_flat;
      ps_input_cntl[ps_offset++] = offset_to_ps_input(outinfo->vs_output_param_offset[VARYING_SLOT_PRIMITIVE_ID], t);
   }

   /* Per-primitive PS inputs: the HW needs these to be last. */
   num_per_primitive_params += util_bitcount(ps->info.ps.input_per_primitive_mask);
   input_mask_to_ps_inputs(outinfo, ps, ps->info.ps.input_per_primitive_mask, ps_input_cntl, &ps_offset, per_prim);

   /* Only GFX10.3+ support per-primitive params */
   assert(pdev->info.gfx_level >= GFX10_3 || num_per_primitive_params == 0);

   radeon_begin(cs);

   if (pdev->info.gfx_level >= GFX12) {
      radeon_opt_set_context_regn(R_028664_SPI_PS_INPUT_CNTL_0, ps_input_cntl, cs->tracked_regs.spi_ps_input_cntl,
                                  ps_offset);
   } else {
      if (pdev->info.gfx_level == GFX10_3) {
         /* NUM_INTERP / NUM_PRIM_INTERP separately contain
          * the number of per-vertex and per-primitive PS input attributes.
          * These are only exactly known here so couldn't be precomputed.
          */
         const unsigned num_per_vertex_params = ps->info.ps.num_inputs - num_per_primitive_params;
         radeon_opt_set_context_reg(R_0286D8_SPI_PS_IN_CONTROL, RADV_TRACKED_SPI_PS_IN_CONTROL,
                                    ps->info.regs.ps.spi_ps_in_control | S_0286D8_NUM_INTERP(num_per_vertex_params) |
                                       S_0286D8_NUM_PRIM_INTERP(num_per_primitive_params));
      }

      radeon_opt_set_context_regn(R_028644_SPI_PS_INPUT_CNTL_0, ps_input_cntl, cs->tracked_regs.spi_ps_input_cntl,
                                  ps_offset);
   }

   radeon_end();
}

static void
radv_emit_fragment_shader_state(struct radv_cmd_buffer *cmd_buffer, const struct radv_shader *ps)
{
   struct radv_device *device = radv_cmd_buffer_device(cmd_buffer);
   const struct radv_physical_device *pdev = radv_device_physical(device);
   const uint32_t spi_ps_input_ena = ps ? ps->config.spi_ps_input_ena : 0;
   const uint32_t spi_ps_input_addr = ps ? ps->config.spi_ps_input_addr : 0;
   const uint32_t spi_ps_in_control = ps ? ps->info.regs.ps.spi_ps_in_control : 0;
   struct radv_cmd_stream *cs = cmd_buffer->cs;

   if (pdev->info.gfx_level >= GFX12) {
      const uint32_t pa_sc_hisz_control = ps ? ps->info.regs.ps.pa_sc_hisz_control : 0;

      radeon_begin(cs);
      gfx12_begin_context_regs();
      gfx12_opt_set_context_reg2(R_02865C_SPI_PS_INPUT_ENA, RADV_TRACKED_SPI_PS_INPUT_ENA, spi_ps_input_ena,
                                 spi_ps_input_addr);

      gfx12_opt_set_context_reg(R_028640_SPI_PS_IN_CONTROL, RADV_TRACKED_SPI_PS_IN_CONTROL, spi_ps_in_control);

      gfx12_opt_set_context_reg(R_028BBC_PA_SC_HISZ_CONTROL, RADV_TRACKED_PA_SC_HISZ_CONTROL, pa_sc_hisz_control);
      gfx12_end_context_regs();
      radeon_end();
   } else {
      const uint32_t pa_sc_shader_control = ps ? ps->info.regs.ps.pa_sc_shader_control : 0;

      radeon_begin(cs);
      radeon_opt_set_context_reg2(R_0286CC_SPI_PS_INPUT_ENA, RADV_TRACKED_SPI_PS_INPUT_ENA, spi_ps_input_ena,
                                  spi_ps_input_addr);

      if (pdev->info.gfx_level != GFX10_3) {
         radeon_opt_set_context_reg(R_0286D8_SPI_PS_IN_CONTROL, RADV_TRACKED_SPI_PS_IN_CONTROL, spi_ps_in_control);
      }

      if (pdev->info.gfx_level >= GFX9 && pdev->info.gfx_level < GFX11)
         radeon_opt_set_context_reg(R_028C40_PA_SC_SHADER_CONTROL, RADV_TRACKED_PA_SC_SHADER_CONTROL,
                                    pa_sc_shader_control);
      radeon_end();
   }
}

static void
radv_emit_fragment_shader(struct radv_cmd_buffer *cmd_buffer)
{
   const struct radv_device *device = radv_cmd_buffer_device(cmd_buffer);
   const struct radv_physical_device *pdev = radv_device_physical(device);
   struct radv_shader *ps = cmd_buffer->state.shaders[MESA_SHADER_FRAGMENT];
   struct radv_cmd_stream *cs = cmd_buffer->cs;
   const uint64_t va = radv_shader_get_va(ps);

   if (device->pbb_allowed) {
      const struct radv_binning_settings *settings = &pdev->binning_settings;

      if (cmd_buffer->state.emitted_ps != ps &&
          (settings->context_states_per_bin > 1 || settings->persistent_states_per_bin > 1)) {
         /* Break the batch on PS changes. */
         radeon_begin(cs);
         radeon_event_write(V_028A90_BREAK_BATCH);
         radeon_end();

         cmd_buffer->state.emitted_ps = ps;
      }
   }

   radeon_begin(cs);
   if (pdev->info.gfx_level >= GFX12) {
      gfx12_push_sh_reg(ps->info.regs.pgm_lo, va >> 8);
      gfx12_push_sh_reg(ps->info.regs.pgm_rsrc1, ps->config.rsrc1);
      gfx12_push_sh_reg(ps->info.regs.pgm_rsrc2, ps->config.rsrc2);
   } else {
      radeon_set_sh_reg_seq(ps->info.regs.pgm_lo, 4);
      radeon_emit(va >> 8);
      radeon_emit(S_00B024_MEM_BASE(va >> 40));
      radeon_emit(ps->config.rsrc1);
      radeon_emit(ps->config.rsrc2);
   }
   radeon_end();

   radv_emit_fragment_shader_state(cmd_buffer, ps);
}

static void
radv_emit_vgt_reuse(struct radv_cmd_buffer *cmd_buffer, const struct radv_vgt_shader_key *key)
{
   const struct radv_device *device = radv_cmd_buffer_device(cmd_buffer);
   const struct radv_physical_device *pdev = radv_device_physical(device);
   const struct radv_shader *tes = radv_get_shader(cmd_buffer->state.shaders, MESA_SHADER_TESS_EVAL);
   struct radv_cmd_stream *cs = cmd_buffer->cs;

   if (pdev->info.gfx_level == GFX10_3) {
      /* Legacy Tess+GS should disable reuse to prevent hangs on GFX10.3. */
      const bool has_legacy_tess_gs = key->tess && key->gs && !key->ngg;

      radeon_begin(cs);
      radeon_opt_set_context_reg(R_028AB4_VGT_REUSE_OFF, RADV_TRACKED_VGT_REUSE_OFF,
                                 S_028AB4_REUSE_OFF(has_legacy_tess_gs));
      radeon_end();
   }

   if (pdev->info.family >= CHIP_POLARIS10 && pdev->info.gfx_level < GFX10) {
      unsigned vtx_reuse_depth = 30;
      if (tes && tes->info.tes.spacing == TESS_SPACING_FRACTIONAL_ODD) {
         vtx_reuse_depth = 14;
      }

      radeon_begin(cs);
      radeon_opt_set_context_reg(R_028C58_VGT_VERTEX_REUSE_BLOCK_CNTL, RADV_TRACKED_VGT_VERTEX_REUSE_BLOCK_CNTL,
                                 S_028C58_VTX_REUSE_DEPTH(vtx_reuse_depth));
      radeon_end();
   }
}

static void
radv_emit_vgt_shader_config_gfx12(struct radv_cmd_buffer *cmd_buffer, const struct radv_vgt_shader_key *key)
{
   const bool ngg_wave_id_en = key->ngg_streamout || (key->mesh && key->mesh_scratch_ring);
   struct radv_cmd_stream *cs = cmd_buffer->cs;
   uint32_t stages = 0;

   stages |= S_028A98_GS_EN(key->gs) | S_028A98_GS_FAST_LAUNCH(key->mesh) | S_028A98_GS_W32_EN(key->gs_wave32) |
             S_028A98_NGG_WAVE_ID_EN(ngg_wave_id_en) | S_028A98_PRIMGEN_PASSTHRU_NO_MSG(key->ngg_passthrough);

   if (key->tess)
      stages |= S_028A98_HS_EN(1) | S_028A98_HS_W32_EN(key->hs_wave32);

   radeon_begin(cs);
   radeon_opt_set_context_reg(R_028A98_VGT_SHADER_STAGES_EN, RADV_TRACKED_VGT_SHADER_STAGES_EN, stages);
   radeon_end();
}

static void
radv_emit_vgt_shader_config_gfx6(struct radv_cmd_buffer *cmd_buffer, const struct radv_vgt_shader_key *key)
{
   const struct radv_device *device = radv_cmd_buffer_device(cmd_buffer);
   const struct radv_physical_device *pdev = radv_device_physical(device);
   struct radv_cmd_stream *cs = cmd_buffer->cs;
   uint32_t stages = 0;

   if (key->tess) {
      stages |=
         S_028B54_LS_EN(V_028B54_LS_STAGE_ON) | S_028B54_HS_EN(1) | S_028B54_DYNAMIC_HS(pdev->info.gfx_level != GFX9);

      if (key->gs)
         stages |= S_028B54_ES_EN(V_028B54_ES_STAGE_DS) | S_028B54_GS_EN(1);
      else if (key->ngg)
         stages |= S_028B54_ES_EN(V_028B54_ES_STAGE_DS);
      else
         stages |= S_028B54_VS_EN(V_028B54_VS_STAGE_DS);
   } else if (key->gs) {
      stages |= S_028B54_ES_EN(V_028B54_ES_STAGE_REAL) | S_028B54_GS_EN(1);
   } else if (key->mesh) {
      assert(!key->ngg_passthrough);
      unsigned gs_fast_launch = pdev->info.mesh_fast_launch_2 ? 2 : 1;
      stages |=
         S_028B54_GS_EN(1) | S_028B54_GS_FAST_LAUNCH(gs_fast_launch) | S_028B54_NGG_WAVE_ID_EN(key->mesh_scratch_ring);
   } else if (key->ngg) {
      stages |= S_028B54_ES_EN(V_028B54_ES_STAGE_REAL);
   }

   if (key->ngg) {
      stages |= S_028B54_PRIMGEN_EN(1) | S_028B54_NGG_WAVE_ID_EN(key->ngg_streamout) |
                S_028B54_PRIMGEN_PASSTHRU_EN(key->ngg_passthrough) |
                S_028B54_PRIMGEN_PASSTHRU_NO_MSG(key->ngg_passthrough && pdev->info.family >= CHIP_NAVI23);
   } else if (key->gs) {
      stages |= S_028B54_VS_EN(V_028B54_VS_STAGE_COPY_SHADER);
   }

   if (pdev->info.gfx_level >= GFX9)
      stages |= S_028B54_MAX_PRIMGRP_IN_WAVE(2);

   if (pdev->info.gfx_level >= GFX10) {
      stages |= S_028B54_HS_W32_EN(key->hs_wave32) | S_028B54_GS_W32_EN(key->gs_wave32) |
                S_028B54_VS_W32_EN(pdev->info.gfx_level < GFX11 && key->vs_wave32);
      /* Legacy GS only supports Wave64. Read it as an implication. */
      assert(!(key->gs && !key->ngg) || !key->gs_wave32);
   }

   radeon_begin(cs);
   radeon_opt_set_context_reg(R_028B54_VGT_SHADER_STAGES_EN, RADV_TRACKED_VGT_SHADER_STAGES_EN, stages);
   radeon_end();
}

static void
radv_emit_vgt_shader_config(struct radv_cmd_buffer *cmd_buffer, const struct radv_vgt_shader_key *key)
{
   const struct radv_device *device = radv_cmd_buffer_device(cmd_buffer);
   const struct radv_physical_device *pdev = radv_device_physical(device);

   if (pdev->info.gfx_level >= GFX12) {
      radv_emit_vgt_shader_config_gfx12(cmd_buffer, key);
   } else {
      radv_emit_vgt_shader_config_gfx6(cmd_buffer, key);
   }
}

static void
gfx103_emit_vgt_draw_payload_cntl(struct radv_cmd_buffer *cmd_buffer)
{
   const struct radv_device *device = radv_cmd_buffer_device(cmd_buffer);
   const struct radv_physical_device *pdev = radv_device_physical(device);
   const struct radv_shader *mesh_shader = cmd_buffer->state.shaders[MESA_SHADER_MESH];
   const bool enable_vrs = cmd_buffer->state.uses_vrs;
   struct radv_cmd_stream *cs = cmd_buffer->cs;
   bool enable_prim_payload = false;

   /* Enables the second channel of the primitive export instruction.
    * This channel contains: VRS rate x, y, viewport and layer.
    */
   if (mesh_shader) {
      const struct radv_vs_output_info *outinfo = &mesh_shader->info.outinfo;

      enable_prim_payload = (outinfo->writes_viewport_index_per_primitive || outinfo->writes_layer_per_primitive ||
                             outinfo->writes_primitive_shading_rate_per_primitive);
   }

   const uint32_t vgt_draw_payload_cntl =
      S_028A98_EN_VRS_RATE(enable_vrs) | S_028A98_EN_PRIM_PAYLOAD(enable_prim_payload);

   radeon_begin(cs);

   if (pdev->info.gfx_level >= GFX12) {
      radeon_opt_set_context_reg(R_028AA0_VGT_DRAW_PAYLOAD_CNTL, RADV_TRACKED_VGT_DRAW_PAYLOAD_CNTL,
                                 vgt_draw_payload_cntl);
   } else {
      radeon_opt_set_context_reg(R_028A98_VGT_DRAW_PAYLOAD_CNTL, RADV_TRACKED_VGT_DRAW_PAYLOAD_CNTL,
                                 vgt_draw_payload_cntl);
   }

   radeon_end();
}

static void
gfx103_emit_vrs_state(struct radv_cmd_buffer *cmd_buffer)
{
   const struct radv_device *device = radv_cmd_buffer_device(cmd_buffer);
   const struct radv_physical_device *pdev = radv_device_physical(device);
   const struct radv_shader *ps = cmd_buffer->state.shaders[MESA_SHADER_FRAGMENT];
   const bool force_vrs_per_vertex = cmd_buffer->state.last_vgt_shader->info.force_vrs_per_vertex;
   const bool enable_vrs_coarse_shading = cmd_buffer->state.uses_vrs_coarse_shading;
   struct radv_cmd_stream *cs = cmd_buffer->cs;
   uint32_t mode = V_028064_SC_VRS_COMB_MODE_PASSTHRU;
   uint8_t rate_x = 0, rate_y = 0;

   if (enable_vrs_coarse_shading) {
      /* When per-draw VRS is not enabled at all, try enabling VRS coarse shading 2x2 if the driver
       * determined that it's safe to enable.
       */
      mode = V_028064_SC_VRS_COMB_MODE_OVERRIDE;
      rate_x = rate_y = 1;
   } else if (force_vrs_per_vertex) {
      /* Otherwise, if per-draw VRS is not enabled statically, try forcing per-vertex VRS if
       * requested by the user. Note that vkd3d-proton always has to declare VRS as dynamic because
       * in DX12 it's fully dynamic.
       */
      radeon_begin(cs);
      radeon_opt_set_context_reg(R_028848_PA_CL_VRS_CNTL, RADV_TRACKED_PA_CL_VRS_CNTL,
                                 S_028848_SAMPLE_ITER_COMBINER_MODE(V_028848_SC_VRS_COMB_MODE_OVERRIDE) |
                                    S_028848_VERTEX_RATE_COMBINER_MODE(V_028848_SC_VRS_COMB_MODE_OVERRIDE));
      radeon_end();

      /* If the shader is using discard, turn off coarse shading because discard at 2x2 pixel
       * granularity degrades quality too much. MIN allows sample shading but not coarse shading.
       */
      mode = ps->info.ps.can_discard ? V_028064_SC_VRS_COMB_MODE_MIN : V_028064_SC_VRS_COMB_MODE_PASSTHRU;
   }

   if (pdev->info.gfx_level < GFX11) {
      radeon_begin(cs);
      radeon_opt_set_context_reg(R_028064_DB_VRS_OVERRIDE_CNTL, RADV_TRACKED_DB_VRS_OVERRIDE_CNTL,
                                 S_028064_VRS_OVERRIDE_RATE_COMBINER_MODE(mode) | S_028064_VRS_OVERRIDE_RATE_X(rate_x) |
                                    S_028064_VRS_OVERRIDE_RATE_Y(rate_y));
      radeon_end();
   }
}

static void
radv_emit_graphics_shaders(struct radv_cmd_buffer *cmd_buffer)
{
   struct radv_device *device = radv_cmd_buffer_device(cmd_buffer);
   const struct radv_physical_device *pdev = radv_device_physical(device);

   radv_foreach_stage (s, cmd_buffer->state.active_stages & RADV_GRAPHICS_STAGE_BITS) {
      switch (s) {
      case MESA_SHADER_VERTEX:
         radv_emit_vertex_shader(cmd_buffer);
         break;
      case MESA_SHADER_TESS_CTRL:
         radv_emit_tess_ctrl_shader(cmd_buffer);
         break;
      case MESA_SHADER_TESS_EVAL:
         radv_emit_tess_eval_shader(cmd_buffer);
         break;
      case MESA_SHADER_GEOMETRY:
         radv_emit_geometry_shader(cmd_buffer);
         break;
      case MESA_SHADER_FRAGMENT:
         radv_emit_fragment_shader(cmd_buffer);
         radv_emit_ps_inputs(cmd_buffer);
         break;
      case MESA_SHADER_MESH:
         radv_emit_mesh_shader(cmd_buffer);
         break;
      case MESA_SHADER_TASK:
         radv_emit_compute_shader(pdev, cmd_buffer->gang.cs, cmd_buffer->state.shaders[MESA_SHADER_TASK]);
         break;
      default:
         UNREACHABLE("invalid bind stage");
      }
   }

   if (pdev->info.gfx_level >= GFX12) {
      const struct radv_shader *ps = cmd_buffer->state.shaders[MESA_SHADER_FRAGMENT];
      const struct radv_shader *last_vgt_shader = cmd_buffer->state.last_vgt_shader;
      uint32_t gs_out_config_ps = last_vgt_shader->info.regs.spi_vs_out_config;

      if (ps) {
         gs_out_config_ps |= ps->info.regs.ps.spi_gs_out_config_ps;
      } else {
         /* GFX12 seems to require a dummy FS state otherwise it might just hang. */
         radv_emit_fragment_shader_state(cmd_buffer, NULL);
      }

      radeon_begin(cmd_buffer->cs);
      gfx12_push_sh_reg(R_00B0C4_SPI_SHADER_GS_OUT_CONFIG_PS, gs_out_config_ps);
      radeon_end();
   }

   const struct radv_vgt_shader_key vgt_shader_cfg_key =
      radv_get_vgt_shader_key(device, cmd_buffer->state.shaders, cmd_buffer->state.gs_copy_shader);

   radv_emit_vgt_gs_mode(cmd_buffer);
   radv_emit_vgt_reuse(cmd_buffer, &vgt_shader_cfg_key);
   radv_emit_vgt_shader_config(cmd_buffer, &vgt_shader_cfg_key);

   if (pdev->info.gfx_level >= GFX10_3) {
      gfx103_emit_vgt_draw_payload_cntl(cmd_buffer);
      gfx103_emit_vrs_state(cmd_buffer);
   }
}

static void
radv_emit_graphics_pipeline(struct radv_cmd_buffer *cmd_buffer)
{
   struct radv_graphics_pipeline *pipeline = cmd_buffer->state.graphics_pipeline;
   struct radv_device *device = radv_cmd_buffer_device(cmd_buffer);

   if (cmd_buffer->state.emitted_graphics_pipeline == pipeline)
      return;

   radv_emit_graphics_shaders(cmd_buffer);

   if (pipeline->sqtt_shaders_reloc) {
      /* Emit shaders relocation because RGP requires them to be contiguous in memory. */
      radv_sqtt_emit_relocated_shaders(cmd_buffer, pipeline);
   }

   if (radv_device_fault_detection_enabled(device))
      radv_save_pipeline(cmd_buffer, &pipeline->base);

   cmd_buffer->state.emitted_graphics_pipeline = pipeline;
}

static bool
radv_get_depth_clip_enable(struct radv_cmd_buffer *cmd_buffer)
{
   const struct radv_dynamic_state *d = &cmd_buffer->state.dynamic;

   return d->vk.rs.depth_clip_enable == VK_MESA_DEPTH_CLIP_ENABLE_TRUE ||
          (d->vk.rs.depth_clip_enable == VK_MESA_DEPTH_CLIP_ENABLE_NOT_CLAMP && !d->vk.rs.depth_clamp_enable);
}

static enum radv_depth_clamp_mode
radv_get_depth_clamp_mode(struct radv_cmd_buffer *cmd_buffer)
{
   const struct radv_dynamic_state *d = &cmd_buffer->state.dynamic;
   const bool depth_clip_enable = cmd_buffer->state.depth_clip_enable;
   struct radv_device *device = radv_cmd_buffer_device(cmd_buffer);
   enum radv_depth_clamp_mode mode;

   switch (d->vk.vp.depth_clamp_mode) {
   case VK_DEPTH_CLAMP_MODE_VIEWPORT_RANGE_EXT:
      mode = RADV_DEPTH_CLAMP_MODE_VIEWPORT;
      break;
   case VK_DEPTH_CLAMP_MODE_USER_DEFINED_RANGE_EXT:
      mode = RADV_DEPTH_CLAMP_MODE_USER_DEFINED;
      break;
   default:
      UNREACHABLE("invalid depth clamp mode\n");
   }

   if (!d->vk.rs.depth_clamp_enable) {
      /* For optimal performance, depth clamping should always be enabled except if the application
       * disables clamping explicitly or uses depth values outside of the [0.0, 1.0] range.
       */
      if (!depth_clip_enable || device->vk.enabled_extensions.EXT_depth_range_unrestricted) {
         mode = RADV_DEPTH_CLAMP_MODE_DISABLED;
      } else {
         mode = RADV_DEPTH_CLAMP_MODE_ZERO_TO_ONE;
      }
   }

   return mode;
}

static void
radv_get_viewport_zscale_ztranslate(struct radv_cmd_buffer *cmd_buffer, uint32_t vp_idx, float *zscale,
                                    float *ztranslate)
{
   const struct radv_dynamic_state *d = &cmd_buffer->state.dynamic;

   if (d->vk.vp.depth_clip_negative_one_to_one) {
      *zscale = d->vp_xform[vp_idx].scale[2] * 0.5f;
      *ztranslate = (d->vp_xform[vp_idx].translate[2] + d->vk.vp.viewports[vp_idx].maxDepth) * 0.5f;
   } else {
      *zscale = d->vp_xform[vp_idx].scale[2];
      *ztranslate = d->vp_xform[vp_idx].translate[2];
   }
}

static void
radv_get_viewport_zmin_zmax(struct radv_cmd_buffer *cmd_buffer, const VkViewport *viewport,
                            const enum radv_depth_clamp_mode depth_clamp_mode, float *zmin, float *zmax)
{
   if (depth_clamp_mode == RADV_DEPTH_CLAMP_MODE_ZERO_TO_ONE) {
      *zmin = 0.0f;
      *zmax = 1.0f;
   } else if (depth_clamp_mode == RADV_DEPTH_CLAMP_MODE_USER_DEFINED) {
      const struct radv_dynamic_state *d = &cmd_buffer->state.dynamic;
      *zmin = d->vk.vp.depth_clamp_range.minDepthClamp;
      *zmax = d->vk.vp.depth_clamp_range.maxDepthClamp;
   } else {
      *zmin = MIN2(viewport->minDepth, viewport->maxDepth);
      *zmax = MAX2(viewport->minDepth, viewport->maxDepth);
   }
}

static void
radv_emit_viewport_state(struct radv_cmd_buffer *cmd_buffer)
{
   const struct radv_device *device = radv_cmd_buffer_device(cmd_buffer);
   const struct radv_physical_device *pdev = radv_device_physical(device);
   const enum radv_depth_clamp_mode depth_clamp_mode = cmd_buffer->state.depth_clamp_mode;
   const struct radv_dynamic_state *d = &cmd_buffer->state.dynamic;
   struct radv_cmd_stream *cs = cmd_buffer->cs;

   assert(d->vk.vp.viewport_count);

   radeon_begin(cs);

   if (pdev->info.gfx_level >= GFX12) {
      radeon_set_context_reg_seq(R_02843C_PA_CL_VPORT_XSCALE, d->vk.vp.viewport_count * 8);

      for (unsigned i = 0; i < d->vk.vp.viewport_count; i++) {
         float zscale, ztranslate, zmin, zmax;

         radv_get_viewport_zscale_ztranslate(cmd_buffer, i, &zscale, &ztranslate);
         radv_get_viewport_zmin_zmax(cmd_buffer, &d->vk.vp.viewports[i], depth_clamp_mode, &zmin, &zmax);

         radeon_emit(fui(d->vp_xform[i].scale[0]));
         radeon_emit(fui(d->vp_xform[i].translate[0]));
         radeon_emit(fui(d->vp_xform[i].scale[1]));
         radeon_emit(fui(d->vp_xform[i].translate[1]));
         radeon_emit(fui(zscale));
         radeon_emit(fui(ztranslate));
         radeon_emit(fui(zmin));
         radeon_emit(fui(zmax));
      }

      radeon_set_context_reg(R_028064_DB_VIEWPORT_CONTROL,
                             S_028064_DISABLE_VIEWPORT_CLAMP(depth_clamp_mode == RADV_DEPTH_CLAMP_MODE_DISABLED));
   } else {
      radeon_set_context_reg_seq(R_02843C_PA_CL_VPORT_XSCALE, d->vk.vp.viewport_count * 6);

      for (unsigned i = 0; i < d->vk.vp.viewport_count; i++) {
         float zscale, ztranslate;

         radv_get_viewport_zscale_ztranslate(cmd_buffer, i, &zscale, &ztranslate);

         radeon_emit(fui(d->vp_xform[i].scale[0]));
         radeon_emit(fui(d->vp_xform[i].translate[0]));
         radeon_emit(fui(d->vp_xform[i].scale[1]));
         radeon_emit(fui(d->vp_xform[i].translate[1]));
         radeon_emit(fui(zscale));
         radeon_emit(fui(ztranslate));
      }

      radeon_set_context_reg_seq(R_0282D0_PA_SC_VPORT_ZMIN_0, d->vk.vp.viewport_count * 2);
      for (unsigned i = 0; i < d->vk.vp.viewport_count; i++) {
         float zmin, zmax;

         radv_get_viewport_zmin_zmax(cmd_buffer, &d->vk.vp.viewports[i], depth_clamp_mode, &zmin, &zmax);

         radeon_emit(fui(zmin));
         radeon_emit(fui(zmax));
      }

      radeon_set_context_reg(R_02800C_DB_RENDER_OVERRIDE,
                             S_02800C_DISABLE_VIEWPORT_CLAMP(depth_clamp_mode == RADV_DEPTH_CLAMP_MODE_DISABLED));
   }

   radeon_end();
}

static VkRect2D
radv_scissor_from_viewport(const float scale[3], const float translate[3])
{
   VkRect2D rect;

   rect.offset.x = translate[0] - fabsf(scale[0]);
   rect.offset.y = translate[1] - fabsf(scale[1]);
   rect.extent.width = ceilf(translate[0] + fabsf(scale[0])) - rect.offset.x;
   rect.extent.height = ceilf(translate[1] + fabsf(scale[1])) - rect.offset.y;

   return rect;
}

static VkRect2D
radv_intersect_scissor(const VkRect2D *a, const VkRect2D *b)
{
   VkRect2D ret;
   ret.offset.x = MAX2(a->offset.x, b->offset.x);
   ret.offset.y = MAX2(a->offset.y, b->offset.y);
   ret.extent.width = MIN2(a->offset.x + a->extent.width, b->offset.x + b->extent.width) - ret.offset.x;
   ret.extent.height = MIN2(a->offset.y + a->extent.height, b->offset.y + b->extent.height) - ret.offset.y;
   return ret;
}

static void
radv_emit_scissor_state(struct radv_cmd_buffer *cmd_buffer)
{
   const struct radv_device *device = radv_cmd_buffer_device(cmd_buffer);
   const struct radv_physical_device *pdev = radv_device_physical(device);
   const struct radv_dynamic_state *d = &cmd_buffer->state.dynamic;
   struct radv_cmd_stream *cs = cmd_buffer->cs;

   if (!d->vk.vp.scissor_count)
      return;

   radeon_begin(cs);
   radeon_set_context_reg_seq(R_028250_PA_SC_VPORT_SCISSOR_0_TL, d->vk.vp.scissor_count * 2);

   for (unsigned i = 0; i < d->vk.vp.scissor_count; i++) {
      VkRect2D viewport_scissor = radv_scissor_from_viewport(d->vp_xform[i].scale, d->vp_xform[i].translate);
      VkRect2D scissor = radv_intersect_scissor(&d->vk.vp.scissors[i], &viewport_scissor);

      uint32_t minx = scissor.offset.x;
      uint32_t miny = scissor.offset.y;
      uint32_t maxx = minx + scissor.extent.width;
      uint32_t maxy = miny + scissor.extent.height;

      if (pdev->info.gfx_level >= GFX12) {
         /* On GFX12, an empty scissor must be done like this because the bottom-right bounds are inclusive. */
         if (maxx == 0 || maxy == 0) {
            minx = miny = maxx = maxy = 1;
         }

         radeon_emit(S_028250_TL_X(minx) | S_028250_TL_Y_GFX12(miny));
         radeon_emit(S_028254_BR_X(maxx - 1) | S_028254_BR_Y(maxy - 1));
      } else {
         radeon_emit(S_028250_TL_X(minx) | S_028250_TL_Y_GFX6(miny) | S_028250_WINDOW_OFFSET_DISABLE(1));
         radeon_emit(S_028254_BR_X(maxx) | S_028254_BR_Y(maxy));
      }
   }

   radeon_end();
}

static void
radv_emit_blend_constants_state(struct radv_cmd_buffer *cmd_buffer)
{
   const struct radv_dynamic_state *d = &cmd_buffer->state.dynamic;
   struct radv_cmd_stream *cs = cmd_buffer->cs;

   radeon_begin(cs);
   radeon_set_context_reg_seq(R_028414_CB_BLEND_RED, 4);
   radeon_emit_array((uint32_t *)d->vk.cb.blend_constants, 4);
   radeon_end();
}

static void
radv_emit_depth_bias_state(struct radv_cmd_buffer *cmd_buffer)
{
   const struct radv_dynamic_state *d = &cmd_buffer->state.dynamic;
   struct radv_rendering_state *render = &cmd_buffer->state.render;
   unsigned slope = fui(d->vk.rs.depth_bias.slope_factor * 16.0f);
   struct radv_cmd_stream *cs = cmd_buffer->cs;
   unsigned pa_su_poly_offset_db_fmt_cntl = 0;

   if (vk_format_has_depth(render->ds_att.format) &&
       d->vk.rs.depth_bias.representation != VK_DEPTH_BIAS_REPRESENTATION_FLOAT_EXT) {
      VkFormat format = vk_format_depth_only(render->ds_att.format);

      if (format == VK_FORMAT_D16_UNORM) {
         pa_su_poly_offset_db_fmt_cntl = S_028B78_POLY_OFFSET_NEG_NUM_DB_BITS(-16);
      } else {
         assert(format == VK_FORMAT_D32_SFLOAT);
         if (d->vk.rs.depth_bias.representation ==
             VK_DEPTH_BIAS_REPRESENTATION_LEAST_REPRESENTABLE_VALUE_FORCE_UNORM_EXT) {
            pa_su_poly_offset_db_fmt_cntl = S_028B78_POLY_OFFSET_NEG_NUM_DB_BITS(-24);
         } else {
            pa_su_poly_offset_db_fmt_cntl =
               S_028B78_POLY_OFFSET_NEG_NUM_DB_BITS(-23) | S_028B78_POLY_OFFSET_DB_IS_FLOAT_FMT(1);
         }
      }
   }

   radeon_begin(cs);
   radeon_set_context_reg_seq(R_028B7C_PA_SU_POLY_OFFSET_CLAMP, 5);
   radeon_emit(fui(d->vk.rs.depth_bias.clamp));           /* CLAMP */
   radeon_emit(slope);                                    /* FRONT SCALE */
   radeon_emit(fui(d->vk.rs.depth_bias.constant_factor)); /* FRONT OFFSET */
   radeon_emit(slope);                                    /* BACK SCALE */
   radeon_emit(fui(d->vk.rs.depth_bias.constant_factor)); /* BACK OFFSET */

   radeon_set_context_reg(R_028B78_PA_SU_POLY_OFFSET_DB_FMT_CNTL, pa_su_poly_offset_db_fmt_cntl);
   radeon_end();
}

static void
radv_emit_vgt_prim_state(struct radv_cmd_buffer *cmd_buffer)
{
   struct radv_device *device = radv_cmd_buffer_device(cmd_buffer);
   const struct radv_physical_device *pdev = radv_device_physical(device);
   const uint32_t vgt_outprim_type = cmd_buffer->state.vgt_outprim_type;
   const struct radv_dynamic_state *d = &cmd_buffer->state.dynamic;
   struct radv_cmd_stream *cs = cmd_buffer->cs;

   if (cmd_buffer->state.mesh_shading)
      return;

   radeon_begin(cs);
   if (pdev->info.gfx_level >= GFX7) {
      uint32_t vgt_prim = d->vk.ia.primitive_topology;

      if (pdev->info.gfx_level >= GFX12)
         vgt_prim |= S_030908_NUM_INPUT_CP(d->vk.ts.patch_control_points);

      radeon_set_uconfig_reg_idx(&pdev->info, R_030908_VGT_PRIMITIVE_TYPE, 1, vgt_prim);
   } else {
      radeon_set_config_reg(R_008958_VGT_PRIMITIVE_TYPE, d->vk.ia.primitive_topology);
   }
   radeon_end();

   radv_emit_vgt_gs_out(cmd_buffer, vgt_outprim_type);
}

static bool
radv_should_force_vrs1x1(struct radv_cmd_buffer *cmd_buffer)
{
   struct radv_device *device = radv_cmd_buffer_device(cmd_buffer);
   const struct radv_physical_device *pdev = radv_device_physical(device);
   const struct radv_shader *ps = cmd_buffer->state.shaders[MESA_SHADER_FRAGMENT];

   return pdev->info.gfx_level >= GFX10_3 &&
          (radv_is_sample_shading_enabled(cmd_buffer, NULL) || (ps && ps->info.ps.force_sample_iter_shading_rate));
}

static void
radv_emit_fsr_state(struct radv_cmd_buffer *cmd_buffer)
{
   struct radv_device *device = radv_cmd_buffer_device(cmd_buffer);
   const struct radv_physical_device *pdev = radv_device_physical(device);
   const struct radv_dynamic_state *d = &cmd_buffer->state.dynamic;
   struct radv_cmd_stream *cs = cmd_buffer->cs;

   /* When per-vertex VRS is forced and the dynamic fragment shading rate is a no-op, ignore
    * it. This is needed for vkd3d-proton because it always declares per-draw VRS as dynamic.
    */
   if (device->force_vrs != RADV_FORCE_VRS_1x1 && d->vk.fsr.fragment_size.width == 1 &&
       d->vk.fsr.fragment_size.height == 1 &&
       d->vk.fsr.combiner_ops[0] == VK_FRAGMENT_SHADING_RATE_COMBINER_OP_KEEP_KHR &&
       d->vk.fsr.combiner_ops[1] == VK_FRAGMENT_SHADING_RATE_COMBINER_OP_KEEP_KHR)
      return;

   uint32_t rate_x = MIN2(2, d->vk.fsr.fragment_size.width) - 1;
   uint32_t rate_y = MIN2(2, d->vk.fsr.fragment_size.height) - 1;
   uint32_t pipeline_comb_mode = d->vk.fsr.combiner_ops[0];
   uint32_t htile_comb_mode = d->vk.fsr.combiner_ops[1];
   uint32_t pa_cl_vrs_cntl = 0;

   assert(pdev->info.gfx_level >= GFX10_3);

   if (!cmd_buffer->state.render.vrs_att.iview) {
      /* When the current subpass has no VRS attachment, the VRS rates are expected to be 1x1, so we
       * can cheat by tweaking the different combiner modes.
       */
      switch (htile_comb_mode) {
      case VK_FRAGMENT_SHADING_RATE_COMBINER_OP_MIN_KHR:
         /* The result of min(A, 1x1) is always 1x1. */
         FALLTHROUGH;
      case VK_FRAGMENT_SHADING_RATE_COMBINER_OP_REPLACE_KHR:
         /* Force the per-draw VRS rate to 1x1. */
         rate_x = rate_y = 0;

         /* As the result of min(A, 1x1) or replace(A, 1x1) are always 1x1, set the vertex rate
          * combiner mode as passthrough.
          */
         pipeline_comb_mode = V_028848_SC_VRS_COMB_MODE_PASSTHRU;
         break;
      case VK_FRAGMENT_SHADING_RATE_COMBINER_OP_MAX_KHR:
         /* The result of max(A, 1x1) is always A. */
         FALLTHROUGH;
      case VK_FRAGMENT_SHADING_RATE_COMBINER_OP_KEEP_KHR:
         /* Nothing to do here because the SAMPLE_ITER combiner mode should already be passthrough. */
         break;
      default:
         break;
      }
   }

   /* Disable VRS and use the rates from PS_ITER_SAMPLES if:
    *
    * 1) sample shading is enabled or per-sample interpolation is used by the fragment shader
    * 2) the fragment shader requires 1x1 shading rate for some other reason
    */
   if (radv_should_force_vrs1x1(cmd_buffer)) {
      pa_cl_vrs_cntl |= S_028848_SAMPLE_ITER_COMBINER_MODE(V_028848_SC_VRS_COMB_MODE_OVERRIDE);
   }

   /* VERTEX_RATE_COMBINER_MODE controls the combiner mode between the
    * draw rate and the vertex rate.
    */
   if (cmd_buffer->state.mesh_shading) {
      pa_cl_vrs_cntl |= S_028848_VERTEX_RATE_COMBINER_MODE(V_028848_SC_VRS_COMB_MODE_PASSTHRU) |
                        S_028848_PRIMITIVE_RATE_COMBINER_MODE(pipeline_comb_mode);
   } else {
      pa_cl_vrs_cntl |= S_028848_VERTEX_RATE_COMBINER_MODE(pipeline_comb_mode) |
                        S_028848_PRIMITIVE_RATE_COMBINER_MODE(V_028848_SC_VRS_COMB_MODE_PASSTHRU);
   }

   /* HTILE_RATE_COMBINER_MODE controls the combiner mode between the primitive rate and the HTILE
    * rate.
    */
   pa_cl_vrs_cntl |= S_028848_HTILE_RATE_COMBINER_MODE(htile_comb_mode);

   radeon_begin(cs);

   /* Emit per-draw VRS rate which is the first combiner. */
   radeon_set_uconfig_reg(R_03098C_GE_VRS_RATE, S_03098C_RATE_X(rate_x) | S_03098C_RATE_Y(rate_y));

   radeon_set_context_reg(R_028848_PA_CL_VRS_CNTL, pa_cl_vrs_cntl);

   radeon_end();
}

static uint32_t
radv_get_primitive_reset_index(const struct radv_cmd_buffer *cmd_buffer)
{
   const uint32_t index_type = G_028A7C_INDEX_TYPE(cmd_buffer->state.index_type);
   switch (index_type) {
   case V_028A7C_VGT_INDEX_8:
      return 0xffu;
   case V_028A7C_VGT_INDEX_16:
      return 0xffffu;
   case V_028A7C_VGT_INDEX_32:
      return 0xffffffffu;
   default:
      UNREACHABLE("invalid index type");
   }
}

static void
radv_emit_ls_hs_config(struct radv_cmd_buffer *cmd_buffer)
{
   struct radv_device *device = radv_cmd_buffer_device(cmd_buffer);
   const struct radv_physical_device *pdev = radv_device_physical(device);
   const struct radv_shader *tcs = cmd_buffer->state.shaders[MESA_SHADER_TESS_CTRL];
   const struct radv_dynamic_state *d = &cmd_buffer->state.dynamic;
   unsigned ls_hs_config;

   if (!tcs)
      return;

   ls_hs_config = S_028B58_NUM_PATCHES(cmd_buffer->state.tess_num_patches) |
                  /* GFX12 programs patch_vertices in VGT_PRIMITIVE_TYPE.NUM_INPUT_CP. */
                  S_028B58_HS_NUM_INPUT_CP(pdev->info.gfx_level < GFX12 ? d->vk.ts.patch_control_points : 0) |
                  S_028B58_HS_NUM_OUTPUT_CP(tcs->info.tcs.tcs_vertices_out);

   radeon_begin(cmd_buffer->cs);
   if (pdev->info.gfx_level >= GFX7) {
      radeon_set_context_reg_idx(R_028B58_VGT_LS_HS_CONFIG, 2, ls_hs_config);
   } else {
      radeon_set_context_reg(R_028B58_VGT_LS_HS_CONFIG, ls_hs_config);
   }
   radeon_end();
}

static void
radv_emit_rast_samples_state(struct radv_cmd_buffer *cmd_buffer)
{
   struct radv_device *device = radv_cmd_buffer_device(cmd_buffer);
   const struct radv_physical_device *pdev = radv_device_physical(device);
   const unsigned rasterization_samples = cmd_buffer->state.num_rast_samples;
   unsigned ps_iter_samples = radv_get_ps_iter_samples(cmd_buffer);
   const struct radv_dynamic_state *d = &cmd_buffer->state.dynamic;
   unsigned spi_baryc_cntl = S_0286E0_FRONT_FACE_ALL_BITS(0);
   unsigned pa_sc_mode_cntl_1;
   bool walk_align8;

   if (pdev->info.gfx_level >= GFX12) {
      const struct radv_rendering_state *render = &cmd_buffer->state.render;

      walk_align8 = !render->has_hiz_his && !cmd_buffer->state.uses_vrs_attachment;
   } else if (pdev->info.gfx_level >= GFX11) {
      walk_align8 = !cmd_buffer->state.uses_vrs_attachment;
   } else {
      walk_align8 = true;
   }

   pa_sc_mode_cntl_1 = S_028A4C_WALK_FENCE_ENABLE(1) | // TODO linear dst fixes
                       S_028A4C_WALK_FENCE_SIZE(pdev->info.num_tile_pipes == 2 ? 2 : 3) |
                       S_028A4C_OUT_OF_ORDER_PRIMITIVE_ENABLE(cmd_buffer->state.uses_out_of_order_rast) |
                       S_028A4C_OUT_OF_ORDER_WATER_MARK(pdev->info.gfx_level >= GFX12 ? 0 : 0x7) |
                       /* always 1: */
                       S_028A4C_SUPERTILE_WALK_ORDER_ENABLE(1) | S_028A4C_TILE_WALK_ORDER_ENABLE(1) |
                       S_028A4C_MULTI_SHADER_ENGINE_PRIM_DISCARD_ENABLE(1) | S_028A4C_FORCE_EOV_CNTDWN_ENABLE(1) |
                       S_028A4C_FORCE_EOV_REZ_ENABLE(1) | S_028A4C_WALK_ALIGN8_PRIM_FITS_ST(walk_align8);

   if (!d->sample_location.count || !d->vk.ms.sample_locations_enable)
      radv_emit_default_sample_locations(pdev, cmd_buffer->cs, rasterization_samples);

   if (ps_iter_samples > 1) {
      spi_baryc_cntl |= S_0286E0_POS_FLOAT_LOCATION(2);
      pa_sc_mode_cntl_1 |= S_028A4C_PS_ITER_SAMPLE(1);
   }

   if (radv_should_force_vrs1x1(cmd_buffer)) {
      /* Make sure sample shading is enabled even if only MSAA1x is used because the SAMPLE_ITER
       * combiner is in passthrough mode if PS_ITER_SAMPLE is 0, and it uses the per-draw rate. The
       * default VRS rate when sample shading is enabled is 1x1.
       */
      if (!G_028A4C_PS_ITER_SAMPLE(pa_sc_mode_cntl_1))
         pa_sc_mode_cntl_1 |= S_028A4C_PS_ITER_SAMPLE(1);
   }

   if (pdev->info.gfx_level >= GFX12) {
      radeon_begin(cmd_buffer->cs);
      gfx12_begin_context_regs();
      gfx12_set_context_reg(R_028658_SPI_BARYC_CNTL, spi_baryc_cntl);
      gfx12_set_context_reg(R_028A4C_PA_SC_MODE_CNTL_1, pa_sc_mode_cntl_1);
      gfx12_end_context_regs();
      radeon_end();
   } else {
      radeon_begin(cmd_buffer->cs);
      radeon_set_context_reg(R_0286E0_SPI_BARYC_CNTL, spi_baryc_cntl);
      radeon_set_context_reg(R_028A4C_PA_SC_MODE_CNTL_1, pa_sc_mode_cntl_1);
      radeon_end();
   }
}

static void
radv_gfx12_emit_fb_color_state(struct radv_cmd_buffer *cmd_buffer, int index, struct radv_color_buffer_info *cb)
{
   struct radv_cmd_stream *cs = cmd_buffer->cs;

   radeon_begin(cs);
   gfx12_begin_context_regs();
   gfx12_set_context_reg(R_028C60_CB_COLOR0_BASE + index * 0x24, cb->ac.cb_color_base);
   gfx12_set_context_reg(R_028C64_CB_COLOR0_VIEW + index * 0x24, cb->ac.cb_color_view);
   gfx12_set_context_reg(R_028C68_CB_COLOR0_VIEW2 + index * 0x24, cb->ac.cb_color_view2);
   gfx12_set_context_reg(R_028C6C_CB_COLOR0_ATTRIB + index * 0x24, cb->ac.cb_color_attrib);
   gfx12_set_context_reg(R_028C70_CB_COLOR0_FDCC_CONTROL + index * 0x24, cb->ac.cb_dcc_control);
   gfx12_set_context_reg(R_028C78_CB_COLOR0_ATTRIB2 + index * 0x24, cb->ac.cb_color_attrib2);
   gfx12_set_context_reg(R_028C7C_CB_COLOR0_ATTRIB3 + index * 0x24, cb->ac.cb_color_attrib3);
   gfx12_set_context_reg(R_028E40_CB_COLOR0_BASE_EXT + index * 4, S_028E40_BASE_256B(cb->ac.cb_color_base >> 32));
   gfx12_set_context_reg(R_028EC0_CB_COLOR0_INFO + index * 4, cb->ac.cb_color_info);
   gfx12_end_context_regs();
   radeon_end();
}

static void
radv_gfx6_emit_fb_color_state(struct radv_cmd_buffer *cmd_buffer, int index, struct radv_color_buffer_info *cb,
                              struct radv_image_view *iview, VkImageLayout layout)
{
   struct radv_device *device = radv_cmd_buffer_device(cmd_buffer);
   const struct radv_physical_device *pdev = radv_device_physical(device);
   bool is_vi = pdev->info.gfx_level >= GFX8;
   uint32_t cb_fdcc_control = cb->ac.cb_dcc_control;
   uint32_t cb_color_info = cb->ac.cb_color_info;
   struct radv_image *image = iview->image;

   if (!radv_layout_dcc_compressed(device, image, iview->vk.base_mip_level, layout,
                                   radv_image_queue_family_mask(image, cmd_buffer->qf, cmd_buffer->qf))) {
      if (pdev->info.gfx_level >= GFX11) {
         cb_fdcc_control &= C_028C78_FDCC_ENABLE;
      } else {
         cb_color_info &= C_028C70_DCC_ENABLE;
      }
   }

   const enum radv_fmask_compression fmask_comp = radv_layout_fmask_compression(
      device, image, layout, radv_image_queue_family_mask(image, cmd_buffer->qf, cmd_buffer->qf));
   if (fmask_comp == RADV_FMASK_COMPRESSION_NONE) {
      cb_color_info &= C_028C70_COMPRESSION;
   }

   if (pdev->info.gfx_level >= GFX8 && pdev->info.gfx_level < GFX11 && iview->disable_tc_compat_cmask_mrt)
      cb_color_info &= C_028C70_FMASK_COMPRESS_1FRAG_ONLY;

   radeon_begin(cmd_buffer->cs);

   if (pdev->info.gfx_level >= GFX11) {
      radeon_set_context_reg_seq(R_028C6C_CB_COLOR0_VIEW + index * 0x3c, 4);
      radeon_emit(cb->ac.cb_color_view);   /* CB_COLOR0_VIEW */
      radeon_emit(cb->ac.cb_color_info);   /* CB_COLOR0_INFO */
      radeon_emit(cb->ac.cb_color_attrib); /* CB_COLOR0_ATTRIB */
      radeon_emit(cb_fdcc_control);        /* CB_COLOR0_FDCC_CONTROL */

      radeon_set_context_reg(R_028C60_CB_COLOR0_BASE + index * 0x3c, cb->ac.cb_color_base);
      radeon_set_context_reg(R_028E40_CB_COLOR0_BASE_EXT + index * 4, S_028E40_BASE_256B(cb->ac.cb_color_base >> 32));
      radeon_set_context_reg(R_028C94_CB_COLOR0_DCC_BASE + index * 0x3c, cb->ac.cb_dcc_base);
      radeon_set_context_reg(R_028EA0_CB_COLOR0_DCC_BASE_EXT + index * 4, S_028EA0_BASE_256B(cb->ac.cb_dcc_base >> 32));
      radeon_set_context_reg(R_028EC0_CB_COLOR0_ATTRIB2 + index * 4, cb->ac.cb_color_attrib2);
      radeon_set_context_reg(R_028EE0_CB_COLOR0_ATTRIB3 + index * 4, cb->ac.cb_color_attrib3);
   } else if (pdev->info.gfx_level >= GFX10) {
      radeon_set_context_reg_seq(R_028C60_CB_COLOR0_BASE + index * 0x3c, 11);
      radeon_emit(cb->ac.cb_color_base);
      radeon_emit(0);
      radeon_emit(0);
      radeon_emit(cb->ac.cb_color_view);
      radeon_emit(cb_color_info);
      radeon_emit(cb->ac.cb_color_attrib);
      radeon_emit(cb->ac.cb_dcc_control);
      radeon_emit(cb->ac.cb_color_cmask);
      radeon_emit(0);
      radeon_emit(cb->ac.cb_color_fmask);
      radeon_emit(0);

      radeon_set_context_reg(R_028C94_CB_COLOR0_DCC_BASE + index * 0x3c, cb->ac.cb_dcc_base);

      radeon_set_context_reg(R_028E40_CB_COLOR0_BASE_EXT + index * 4, S_028E40_BASE_256B(cb->ac.cb_color_base >> 32));
      radeon_set_context_reg(R_028E60_CB_COLOR0_CMASK_BASE_EXT + index * 4,
                             S_028E60_BASE_256B(cb->ac.cb_color_cmask >> 32));
      radeon_set_context_reg(R_028E80_CB_COLOR0_FMASK_BASE_EXT + index * 4,
                             S_028E80_BASE_256B(cb->ac.cb_color_fmask >> 32));
      radeon_set_context_reg(R_028EA0_CB_COLOR0_DCC_BASE_EXT + index * 4, S_028EA0_BASE_256B(cb->ac.cb_dcc_base >> 32));
      radeon_set_context_reg(R_028EC0_CB_COLOR0_ATTRIB2 + index * 4, cb->ac.cb_color_attrib2);
      radeon_set_context_reg(R_028EE0_CB_COLOR0_ATTRIB3 + index * 4, cb->ac.cb_color_attrib3);
   } else if (pdev->info.gfx_level == GFX9) {
      radeon_set_context_reg_seq(R_028C60_CB_COLOR0_BASE + index * 0x3c, 11);
      radeon_emit(cb->ac.cb_color_base);
      radeon_emit(S_028C64_BASE_256B(cb->ac.cb_color_base >> 32));
      radeon_emit(cb->ac.cb_color_attrib2);
      radeon_emit(cb->ac.cb_color_view);
      radeon_emit(cb_color_info);
      radeon_emit(cb->ac.cb_color_attrib);
      radeon_emit(cb->ac.cb_dcc_control);
      radeon_emit(cb->ac.cb_color_cmask);
      radeon_emit(S_028C80_BASE_256B(cb->ac.cb_color_cmask >> 32));
      radeon_emit(cb->ac.cb_color_fmask);
      radeon_emit(S_028C88_BASE_256B(cb->ac.cb_color_fmask >> 32));

      radeon_set_context_reg_seq(R_028C94_CB_COLOR0_DCC_BASE + index * 0x3c, 2);
      radeon_emit(cb->ac.cb_dcc_base);
      radeon_emit(S_028C98_BASE_256B(cb->ac.cb_dcc_base >> 32));

      radeon_set_context_reg(R_0287A0_CB_MRT0_EPITCH + index * 4, cb->ac.cb_mrt_epitch);
   } else {
      radeon_set_context_reg_seq(R_028C60_CB_COLOR0_BASE + index * 0x3c, 6);
      radeon_emit(cb->ac.cb_color_base);
      radeon_emit(cb->ac.cb_color_pitch);
      radeon_emit(cb->ac.cb_color_slice);
      radeon_emit(cb->ac.cb_color_view);
      radeon_emit(cb_color_info);
      radeon_emit(cb->ac.cb_color_attrib);

      if (pdev->info.gfx_level == GFX8)
         radeon_set_context_reg(R_028C78_CB_COLOR0_DCC_CONTROL + index * 0x3c, cb->ac.cb_dcc_control);

      radeon_set_context_reg_seq(R_028C7C_CB_COLOR0_CMASK + index * 0x3c, 4);
      radeon_emit(cb->ac.cb_color_cmask);
      radeon_emit(cb->ac.cb_color_cmask_slice);
      radeon_emit(cb->ac.cb_color_fmask);
      radeon_emit(cb->ac.cb_color_fmask_slice);

      if (is_vi) { /* DCC BASE */
         radeon_set_context_reg(R_028C94_CB_COLOR0_DCC_BASE + index * 0x3c, cb->ac.cb_dcc_base);
      }
   }

   radeon_end();

   if (pdev->info.gfx_level >= GFX11 ? G_028C78_FDCC_ENABLE(cb_fdcc_control) : G_028C70_DCC_ENABLE(cb_color_info)) {
      /* Drawing with DCC enabled also compresses colorbuffers. */
      VkImageSubresourceRange range = vk_image_view_subresource_range(&iview->vk);

      radv_update_dcc_metadata(cmd_buffer, image, &range, true);
   }
}

static void
radv_update_zrange_precision(struct radv_cmd_buffer *cmd_buffer, struct radv_ds_buffer_info *ds,
                             const struct radv_image_view *iview, bool requires_cond_exec)
{
   struct radv_device *device = radv_cmd_buffer_device(cmd_buffer);
   const struct radv_physical_device *pdev = radv_device_physical(device);
   const struct radv_image *image = iview->image;
   uint32_t db_z_info = ds->ac.db_z_info;
   uint32_t db_z_info_reg;

   if (!radv_image_has_tc_compat_zrange_metadata(device, image) ||
       !radv_tc_compat_htile_enabled(image, iview->vk.base_mip_level))
      return;

   db_z_info &= C_028040_ZRANGE_PRECISION;

   if (pdev->info.gfx_level == GFX9) {
      db_z_info_reg = R_028038_DB_Z_INFO;
   } else {
      db_z_info_reg = R_028040_DB_Z_INFO;
   }

   /* When we don't know the last fast clear value we need to emit a
    * conditional packet that will eventually skip the following
    * SET_CONTEXT_REG packet.
    */
   if (requires_cond_exec) {
      uint64_t va = radv_get_tc_compat_zrange_va(image, iview->vk.base_mip_level);

      ac_emit_cond_exec(cmd_buffer->cs->b, pdev->info.gfx_level, va, 3 /* SET_CONTEXT_REG size */);
   }

   radeon_begin(cmd_buffer->cs);
   radeon_set_context_reg(db_z_info_reg, db_z_info);
   radeon_end();
}

static struct radv_image *
radv_cmd_buffer_get_vrs_image(struct radv_cmd_buffer *cmd_buffer)
{
   struct radv_device *device = radv_cmd_buffer_device(cmd_buffer);

   if (!device->vrs.image) {
      VkResult result;

      /* The global VRS state is initialized on-demand to avoid wasting VRAM. */
      result = radv_device_init_vrs_state(device);
      if (result != VK_SUCCESS) {
         vk_command_buffer_set_error(&cmd_buffer->vk, result);
         return NULL;
      }
   }

   return device->vrs.image;
}

static void
radv_gfx12_emit_fb_ds_state(struct radv_cmd_buffer *cmd_buffer, struct radv_ds_buffer_info *ds)
{
   struct radv_cmd_stream *cs = cmd_buffer->cs;

   radeon_begin(cs);
   gfx12_begin_context_regs();
   gfx12_set_context_reg(R_028004_DB_DEPTH_VIEW, ds->ac.db_depth_view);
   gfx12_set_context_reg(R_028008_DB_DEPTH_VIEW1, ds->ac.u.gfx12.db_depth_view1);
   gfx12_set_context_reg(R_028010_DB_RENDER_OVERRIDE2, ds->db_render_override2);
   gfx12_set_context_reg(R_028014_DB_DEPTH_SIZE_XY, ds->ac.db_depth_size);
   gfx12_set_context_reg(R_028018_DB_Z_INFO, ds->ac.db_z_info);
   gfx12_set_context_reg(R_02801C_DB_STENCIL_INFO, ds->ac.db_stencil_info);
   gfx12_set_context_reg(R_028020_DB_Z_READ_BASE, ds->ac.db_depth_base);
   gfx12_set_context_reg(R_028024_DB_Z_READ_BASE_HI, S_028024_BASE_HI(ds->ac.db_depth_base >> 32));
   gfx12_set_context_reg(R_028028_DB_Z_WRITE_BASE, ds->ac.db_depth_base);
   gfx12_set_context_reg(R_02802C_DB_Z_WRITE_BASE_HI, S_02802C_BASE_HI(ds->ac.db_depth_base >> 32));
   gfx12_set_context_reg(R_028030_DB_STENCIL_READ_BASE, ds->ac.db_stencil_base);
   gfx12_set_context_reg(R_028034_DB_STENCIL_READ_BASE_HI, S_028034_BASE_HI(ds->ac.db_stencil_base >> 32));
   gfx12_set_context_reg(R_028038_DB_STENCIL_WRITE_BASE, ds->ac.db_stencil_base);
   gfx12_set_context_reg(R_02803C_DB_STENCIL_WRITE_BASE_HI, S_02803C_BASE_HI(ds->ac.db_stencil_base >> 32));
   gfx12_set_context_reg(R_028B94_PA_SC_HIZ_INFO, ds->ac.u.gfx12.hiz_info);
   gfx12_set_context_reg(R_028B98_PA_SC_HIS_INFO, ds->ac.u.gfx12.his_info);

   if (ds->ac.u.gfx12.hiz_info) {
      gfx12_set_context_reg(R_028B9C_PA_SC_HIZ_BASE, ds->ac.u.gfx12.hiz_base);
      gfx12_set_context_reg(R_028BA0_PA_SC_HIZ_BASE_EXT, S_028BA0_BASE_256B(ds->ac.u.gfx12.hiz_base >> 32));
      gfx12_set_context_reg(R_028BA4_PA_SC_HIZ_SIZE_XY, ds->ac.u.gfx12.hiz_size_xy);
   }

   if (ds->ac.u.gfx12.his_info) {
      gfx12_set_context_reg(R_028BA8_PA_SC_HIS_BASE, ds->ac.u.gfx12.his_base);
      gfx12_set_context_reg(R_028BAC_PA_SC_HIS_BASE_EXT, S_028BAC_BASE_256B(ds->ac.u.gfx12.his_base >> 32));
      gfx12_set_context_reg(R_028BB0_PA_SC_HIS_SIZE_XY, ds->ac.u.gfx12.his_size_xy);
   }
   gfx12_end_context_regs();
   radeon_end();
}

static void
radv_gfx6_emit_fb_ds_state(struct radv_cmd_buffer *cmd_buffer, struct radv_ds_buffer_info *ds,
                           struct radv_image_view *iview, bool depth_compressed, bool stencil_compressed)
{
   struct radv_device *device = radv_cmd_buffer_device(cmd_buffer);
   const struct radv_physical_device *pdev = radv_device_physical(device);
   uint64_t db_htile_data_base = ds->ac.u.gfx6.db_htile_data_base;
   uint32_t db_htile_surface = ds->ac.u.gfx6.db_htile_surface;
   uint32_t db_render_control = ds->db_render_control | cmd_buffer->state.db_render_control;
   uint32_t db_z_info = ds->ac.db_z_info;

   if (!depth_compressed)
      db_render_control |= S_028000_DEPTH_COMPRESS_DISABLE(1);
   if (!stencil_compressed)
      db_render_control |= S_028000_STENCIL_COMPRESS_DISABLE(1);

   if (pdev->info.gfx_level == GFX10_3) {
      if (!cmd_buffer->state.render.vrs_att.iview) {
         db_htile_surface &= C_028ABC_VRS_HTILE_ENCODING;
      } else {
         /* On GFX10.3, when a subpass uses VRS attachment but HTILE can't be enabled, we fallback to
          * our internal HTILE buffer.
          */
         if (!radv_htile_enabled(iview->image, iview->vk.base_mip_level) && radv_cmd_buffer_get_vrs_image(cmd_buffer)) {
            struct radv_buffer *htile_buffer = device->vrs.buffer;

            assert(!G_028038_TILE_SURFACE_ENABLE(db_z_info) && !db_htile_data_base && !db_htile_surface);
            db_z_info |= S_028038_TILE_SURFACE_ENABLE(1);
            db_htile_data_base = radv_buffer_get_va(htile_buffer->bo) >> 8;
            db_htile_surface = S_028ABC_FULL_CACHE(1) | S_028ABC_PIPE_ALIGNED(1) |
                               S_028ABC_VRS_HTILE_ENCODING(V_028ABC_VRS_HTILE_4BIT_ENCODING);
         }
      }
   }

   radeon_begin(cmd_buffer->cs);
   radeon_set_context_reg(R_028000_DB_RENDER_CONTROL, db_render_control);
   radeon_set_context_reg(R_028008_DB_DEPTH_VIEW, ds->ac.db_depth_view);
   radeon_set_context_reg(R_028ABC_DB_HTILE_SURFACE, db_htile_surface);
   radeon_set_context_reg(R_028010_DB_RENDER_OVERRIDE2, ds->db_render_override2);

   if (pdev->info.gfx_level >= GFX10) {
      radeon_set_context_reg(R_028014_DB_HTILE_DATA_BASE, db_htile_data_base);
      radeon_set_context_reg(R_02801C_DB_DEPTH_SIZE_XY, ds->ac.db_depth_size);

      if (pdev->info.gfx_level >= GFX11) {
         radeon_set_context_reg_seq(R_028040_DB_Z_INFO, 6);
      } else {
         radeon_set_context_reg_seq(R_02803C_DB_DEPTH_INFO, 7);
         radeon_emit(S_02803C_RESOURCE_LEVEL(1));
      }
      radeon_emit(db_z_info);
      radeon_emit(ds->ac.db_stencil_info);
      radeon_emit(ds->ac.db_depth_base);
      radeon_emit(ds->ac.db_stencil_base);
      radeon_emit(ds->ac.db_depth_base);
      radeon_emit(ds->ac.db_stencil_base);

      radeon_set_context_reg_seq(R_028068_DB_Z_READ_BASE_HI, 5);
      radeon_emit(S_028068_BASE_HI(ds->ac.db_depth_base >> 32));
      radeon_emit(S_02806C_BASE_HI(ds->ac.db_stencil_base >> 32));
      radeon_emit(S_028070_BASE_HI(ds->ac.db_depth_base >> 32));
      radeon_emit(S_028074_BASE_HI(ds->ac.db_stencil_base >> 32));
      radeon_emit(S_028078_BASE_HI(db_htile_data_base >> 32));
   } else if (pdev->info.gfx_level == GFX9) {
      radeon_set_context_reg_seq(R_028014_DB_HTILE_DATA_BASE, 3);
      radeon_emit(db_htile_data_base);
      radeon_emit(S_028018_BASE_HI(db_htile_data_base >> 32));
      radeon_emit(ds->ac.db_depth_size);

      radeon_set_context_reg_seq(R_028038_DB_Z_INFO, 10);
      radeon_emit(db_z_info);                                      /* DB_Z_INFO */
      radeon_emit(ds->ac.db_stencil_info);                         /* DB_STENCIL_INFO */
      radeon_emit(ds->ac.db_depth_base);                           /* DB_Z_READ_BASE */
      radeon_emit(S_028044_BASE_HI(ds->ac.db_depth_base >> 32));   /* DB_Z_READ_BASE_HI */
      radeon_emit(ds->ac.db_stencil_base);                         /* DB_STENCIL_READ_BASE */
      radeon_emit(S_02804C_BASE_HI(ds->ac.db_stencil_base >> 32)); /* DB_STENCIL_READ_BASE_HI */
      radeon_emit(ds->ac.db_depth_base);                           /* DB_Z_WRITE_BASE */
      radeon_emit(S_028054_BASE_HI(ds->ac.db_depth_base >> 32));   /* DB_Z_WRITE_BASE_HI */
      radeon_emit(ds->ac.db_stencil_base);                         /* DB_STENCIL_WRITE_BASE */
      radeon_emit(S_02805C_BASE_HI(ds->ac.db_stencil_base >> 32)); /* DB_STENCIL_WRITE_BASE_HI */

      radeon_set_context_reg_seq(R_028068_DB_Z_INFO2, 2);
      radeon_emit(ds->ac.u.gfx6.db_z_info2);
      radeon_emit(ds->ac.u.gfx6.db_stencil_info2);
   } else {
      radeon_set_context_reg(R_028014_DB_HTILE_DATA_BASE, db_htile_data_base);

      radeon_set_context_reg_seq(R_02803C_DB_DEPTH_INFO, 9);
      radeon_emit(ds->ac.u.gfx6.db_depth_info);  /* R_02803C_DB_DEPTH_INFO */
      radeon_emit(db_z_info);                    /* R_028040_DB_Z_INFO */
      radeon_emit(ds->ac.db_stencil_info);       /* R_028044_DB_STENCIL_INFO */
      radeon_emit(ds->ac.db_depth_base);         /* R_028048_DB_Z_READ_BASE */
      radeon_emit(ds->ac.db_stencil_base);       /* R_02804C_DB_STENCIL_READ_BASE */
      radeon_emit(ds->ac.db_depth_base);         /* R_028050_DB_Z_WRITE_BASE */
      radeon_emit(ds->ac.db_stencil_base);       /* R_028054_DB_STENCIL_WRITE_BASE */
      radeon_emit(ds->ac.db_depth_size);         /* R_028058_DB_DEPTH_SIZE */
      radeon_emit(ds->ac.u.gfx6.db_depth_slice); /* R_02805C_DB_DEPTH_SLICE */
   }

   radeon_end();

   /* Update the ZRANGE_PRECISION value for the TC-compat bug. */
   radv_update_zrange_precision(cmd_buffer, ds, iview, true);
}

static void
radv_gfx12_emit_null_ds_state(struct radv_cmd_buffer *cmd_buffer)
{
   struct radv_cmd_stream *cs = cmd_buffer->cs;

   radeon_begin(cs);
   gfx12_begin_context_regs();
   gfx12_set_context_reg(R_028018_DB_Z_INFO, S_028018_FORMAT(V_028018_Z_INVALID) | S_028018_NUM_SAMPLES(3));
   gfx12_set_context_reg(R_02801C_DB_STENCIL_INFO,
                         S_02801C_FORMAT(V_02801C_STENCIL_INVALID) | S_02801C_TILE_STENCIL_DISABLE(1));
   gfx12_set_context_reg(R_028B94_PA_SC_HIZ_INFO, S_028B94_SURFACE_ENABLE(0));
   gfx12_set_context_reg(R_028B98_PA_SC_HIS_INFO, S_028B98_SURFACE_ENABLE(0));
   gfx12_set_context_reg(R_028010_DB_RENDER_OVERRIDE2, S_028010_CENTROID_COMPUTATION_MODE(1));
   gfx12_end_context_regs();
   radeon_end();
}

static void
radv_gfx6_emit_null_ds_state(struct radv_cmd_buffer *cmd_buffer)
{
   struct radv_device *device = radv_cmd_buffer_device(cmd_buffer);
   const struct radv_physical_device *pdev = radv_device_physical(device);
   const enum amd_gfx_level gfx_level = pdev->info.gfx_level;

   radeon_begin(cmd_buffer->cs);

   if (gfx_level == GFX9) {
      radeon_set_context_reg_seq(R_028038_DB_Z_INFO, 2);
   } else {
      radeon_set_context_reg_seq(R_028040_DB_Z_INFO, 2);
   }

   /* On GFX11+, the hw intentionally looks at DB_Z_INFO.NUM_SAMPLES when there is no bound
    * depth/stencil buffer and it clamps the number of samples like MIN2(DB_Z_INFO.NUM_SAMPLES,
    * PA_SC_AA_CONFIG.MSAA_EXPOSED_SAMPLES). Use 8x for DB_Z_INFO.NUM_SAMPLES to make sure it's not
    * the constraining factor. This affects VRS, occlusion queries and POPS.
    */
   radeon_emit(S_028040_FORMAT(V_028040_Z_INVALID) | S_028040_NUM_SAMPLES(pdev->info.gfx_level >= GFX11 ? 3 : 0));
   radeon_emit(S_028044_FORMAT(V_028044_STENCIL_INVALID));
   uint32_t db_render_control = 0;

   if (gfx_level == GFX11 || gfx_level == GFX11_5)
      radv_gfx11_set_db_render_control(device, 1, &db_render_control);

   radeon_set_context_reg(R_028000_DB_RENDER_CONTROL, db_render_control);

   radeon_set_context_reg(R_028010_DB_RENDER_OVERRIDE2, S_028010_CENTROID_COMPUTATION_MODE(gfx_level >= GFX10_3));
   radeon_end();
}

/**
 * Update the fast clear depth/stencil values if the image is bound as a
 * depth/stencil buffer.
 */
static void
radv_update_bound_fast_clear_ds(struct radv_cmd_buffer *cmd_buffer, const struct radv_image_view *iview,
                                VkClearDepthStencilValue ds_clear_value, VkImageAspectFlags aspects)
{
   const struct radv_image *image = iview->image;
   struct radv_cmd_stream *cs = cmd_buffer->cs;

   if (cmd_buffer->state.render.ds_att.iview == NULL || cmd_buffer->state.render.ds_att.iview->image != image)
      return;

   radeon_begin(cs);

   if (aspects == (VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT)) {
      radeon_set_context_reg_seq(R_028028_DB_STENCIL_CLEAR, 2);
      radeon_emit(ds_clear_value.stencil);
      radeon_emit(fui(ds_clear_value.depth));
   } else if (aspects == VK_IMAGE_ASPECT_DEPTH_BIT) {
      radeon_set_context_reg(R_02802C_DB_DEPTH_CLEAR, fui(ds_clear_value.depth));
   } else {
      assert(aspects == VK_IMAGE_ASPECT_STENCIL_BIT);
      radeon_set_context_reg(R_028028_DB_STENCIL_CLEAR, ds_clear_value.stencil);
   }

   radeon_end();

   /* Update the ZRANGE_PRECISION value for the TC-compat bug. This is
    * only needed when clearing Z to 0.0.
    */
   if ((aspects & VK_IMAGE_ASPECT_DEPTH_BIT) && ds_clear_value.depth == 0.0) {
      radv_update_zrange_precision(cmd_buffer, &cmd_buffer->state.render.ds_att.ds, iview, false);
   }

   cmd_buffer->cs->context_roll_without_scissor_emitted = true;
}

/**
 * Set the clear depth/stencil values to the image's metadata.
 */
static void
radv_set_ds_clear_metadata(struct radv_cmd_buffer *cmd_buffer, struct radv_image *image,
                           const VkImageSubresourceRange *range, VkClearDepthStencilValue ds_clear_value,
                           VkImageAspectFlags aspects)
{
   struct radv_device *device = radv_cmd_buffer_device(cmd_buffer);
   struct radv_cmd_stream *cs = cmd_buffer->cs;
   uint32_t level_count = vk_image_subresource_level_count(&image->vk, range);

   if (aspects == (VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT)) {
      uint64_t va = radv_get_ds_clear_value_va(image, range->baseMipLevel);

      /* Use the fastest way when both aspects are used. */
      ASSERTED unsigned cdw_end =
         radv_cs_write_data_head(device, cs, V_370_PFP, va, 2 * level_count, cmd_buffer->state.predicating);

      radeon_begin(cs);

      for (uint32_t l = 0; l < level_count; l++) {
         radeon_emit(ds_clear_value.stencil);
         radeon_emit(fui(ds_clear_value.depth));
      }

      radeon_end();
      assert(cs->b->cdw == cdw_end);
   } else {
      /* Otherwise we need one WRITE_DATA packet per level. */
      for (uint32_t l = 0; l < level_count; l++) {
         uint64_t va = radv_get_ds_clear_value_va(image, range->baseMipLevel + l);
         unsigned value;

         if (aspects == VK_IMAGE_ASPECT_DEPTH_BIT) {
            value = fui(ds_clear_value.depth);
            va += 4;
         } else {
            assert(aspects == VK_IMAGE_ASPECT_STENCIL_BIT);
            value = ds_clear_value.stencil;
         }

         radv_write_data(cmd_buffer, V_370_PFP, va, 1, &value, cmd_buffer->state.predicating);
      }
   }
}

void
radv_update_hiz_metadata(struct radv_cmd_buffer *cmd_buffer, struct radv_image *image,
                         const VkImageSubresourceRange *range, bool enable)
{
   const struct radv_device *device = radv_cmd_buffer_device(cmd_buffer);
   struct radv_cmd_stream *cs = cmd_buffer->cs;

   if (!image->hiz_valid_offset)
      return;

   const uint64_t va = radv_get_hiz_valid_va(image, range->baseMipLevel);
   const uint32_t level_count = vk_image_subresource_level_count(&image->vk, range);

   ASSERTED unsigned cdw_end =
      radv_cs_write_data_head(device, cs, V_370_PFP, va, level_count, cmd_buffer->state.predicating);

   radeon_begin(cs);
   for (uint32_t l = 0; l < level_count; l++)
      radeon_emit(enable);
   radeon_end();

   assert(cs->b->cdw == cdw_end);
}

/**
 * Update the TC-compat metadata value for this image.
 */
static void
radv_set_tc_compat_zrange_metadata(struct radv_cmd_buffer *cmd_buffer, struct radv_image *image,
                                   const VkImageSubresourceRange *range, uint32_t value)
{
   struct radv_device *device = radv_cmd_buffer_device(cmd_buffer);
   struct radv_cmd_stream *cs = cmd_buffer->cs;

   if (!radv_image_has_tc_compat_zrange_metadata(device, image))
      return;

   uint64_t va = radv_get_tc_compat_zrange_va(image, range->baseMipLevel);
   uint32_t level_count = vk_image_subresource_level_count(&image->vk, range);

   ASSERTED unsigned cdw_end =
      radv_cs_write_data_head(device, cs, V_370_PFP, va, level_count, cmd_buffer->state.predicating);

   radeon_begin(cs);

   for (uint32_t l = 0; l < level_count; l++)
      radeon_emit(value);

   radeon_end();
   assert(cs->b->cdw == cdw_end);
}

static void
radv_update_tc_compat_zrange_metadata(struct radv_cmd_buffer *cmd_buffer, const struct radv_image_view *iview,
                                      VkClearDepthStencilValue ds_clear_value)
{
   VkImageSubresourceRange range = vk_image_view_subresource_range(&iview->vk);
   uint32_t cond_val;

   /* Conditionally set DB_Z_INFO.ZRANGE_PRECISION to 0 when the last
    * depth clear value is 0.0f.
    */
   cond_val = ds_clear_value.depth == 0.0f ? UINT_MAX : 0;

   radv_set_tc_compat_zrange_metadata(cmd_buffer, iview->image, &range, cond_val);
}

/**
 * Update the clear depth/stencil values for this image.
 */
void
radv_update_ds_clear_metadata(struct radv_cmd_buffer *cmd_buffer, const struct radv_image_view *iview,
                              VkClearDepthStencilValue ds_clear_value, VkImageAspectFlags aspects)
{
   VkImageSubresourceRange range = vk_image_view_subresource_range(&iview->vk);
   struct radv_image *image = iview->image;

   assert(radv_htile_enabled(image, range.baseMipLevel));

   radv_set_ds_clear_metadata(cmd_buffer, iview->image, &range, ds_clear_value, aspects);

   if (radv_tc_compat_htile_enabled(image, iview->vk.base_mip_level) && (aspects & VK_IMAGE_ASPECT_DEPTH_BIT)) {
      radv_update_tc_compat_zrange_metadata(cmd_buffer, iview, ds_clear_value);
   }

   radv_update_bound_fast_clear_ds(cmd_buffer, iview, ds_clear_value, aspects);
}

/**
 * Load the clear depth/stencil values from the image's metadata.
 */
static void
radv_load_ds_clear_metadata(struct radv_cmd_buffer *cmd_buffer, const struct radv_image_view *iview)
{
   struct radv_device *device = radv_cmd_buffer_device(cmd_buffer);
   const struct radv_physical_device *pdev = radv_device_physical(device);
   struct radv_cmd_stream *cs = cmd_buffer->cs;
   const struct radv_image *image = iview->image;
   VkImageAspectFlags aspects = vk_format_aspects(image->vk.format);
   uint64_t va = radv_get_ds_clear_value_va(image, iview->vk.base_mip_level);
   unsigned reg_offset = 0, reg_count = 0;

   assert(radv_htile_enabled(image, iview->vk.base_mip_level));

   if (aspects & VK_IMAGE_ASPECT_STENCIL_BIT) {
      ++reg_count;
   } else {
      ++reg_offset;
      va += 4;
   }
   if (aspects & VK_IMAGE_ASPECT_DEPTH_BIT)
      ++reg_count;

   uint32_t reg = R_028028_DB_STENCIL_CLEAR + 4 * reg_offset;

   if (pdev->info.has_load_ctx_reg_pkt) {
      radeon_begin(cs);
      radeon_emit(PKT3(PKT3_LOAD_CONTEXT_REG_INDEX, 3, 0));
      radeon_emit(va);
      radeon_emit(va >> 32);
      radeon_emit((reg - SI_CONTEXT_REG_OFFSET) >> 2);
      radeon_emit(reg_count);
      radeon_end();
   } else {
      ac_emit_cp_copy_data(cs->b, COPY_DATA_SRC_MEM, COPY_DATA_REG, va, reg >> 2,
                           (reg_count == 2 ? AC_CP_COPY_DATA_COUNT_SEL : 0));

      ac_emit_cp_pfp_sync_me(cs->b);
   }
}

/*
 * With DCC some colors don't require CMASK elimination before being
 * used as a texture. This sets a predicate value to determine if the
 * cmask eliminate is required.
 */
void
radv_update_fce_metadata(struct radv_cmd_buffer *cmd_buffer, struct radv_image *image,
                         const VkImageSubresourceRange *range, bool value)
{
   struct radv_device *device = radv_cmd_buffer_device(cmd_buffer);
   struct radv_cmd_stream *cs = cmd_buffer->cs;

   if (!image->fce_pred_offset)
      return;

   uint64_t pred_val = value;
   uint64_t va = radv_image_get_fce_pred_va(image, range->baseMipLevel);
   uint32_t level_count = vk_image_subresource_level_count(&image->vk, range);

   ASSERTED unsigned cdw_end = radv_cs_write_data_head(device, cs, V_370_PFP, va, 2 * level_count, false);

   radeon_begin(cs);

   for (uint32_t l = 0; l < level_count; l++) {
      radeon_emit(pred_val);
      radeon_emit(pred_val >> 32);
   }

   radeon_end();
   assert(cs->b->cdw == cdw_end);
}

/**
 * Update the DCC predicate to reflect the compression state.
 */
void
radv_update_dcc_metadata(struct radv_cmd_buffer *cmd_buffer, struct radv_image *image,
                         const VkImageSubresourceRange *range, bool value)
{
   struct radv_device *device = radv_cmd_buffer_device(cmd_buffer);
   struct radv_cmd_stream *cs = cmd_buffer->cs;

   if (image->dcc_pred_offset == 0)
      return;

   uint64_t pred_val = value;
   uint64_t va = radv_image_get_dcc_pred_va(image, range->baseMipLevel);
   uint32_t level_count = vk_image_subresource_level_count(&image->vk, range);

   assert(radv_dcc_enabled(image, range->baseMipLevel));

   ASSERTED unsigned cdw_end = radv_cs_write_data_head(device, cs, V_370_PFP, va, 2 * level_count, false);

   radeon_begin(cs);

   for (uint32_t l = 0; l < level_count; l++) {
      radeon_emit(pred_val);
      radeon_emit(pred_val >> 32);
   }

   radeon_end();
   assert(cs->b->cdw == cdw_end);
}

/**
 * Update the fast clear color values if the image is bound as a color buffer.
 */
static void
radv_update_bound_fast_clear_color(struct radv_cmd_buffer *cmd_buffer, struct radv_image *image, int cb_idx,
                                   uint32_t color_values[2])
{
   struct radv_device *device = radv_cmd_buffer_device(cmd_buffer);
   struct radv_cmd_stream *cs = cmd_buffer->cs;

   if (cb_idx >= cmd_buffer->state.render.color_att_count || cmd_buffer->state.render.color_att[cb_idx].iview == NULL ||
       cmd_buffer->state.render.color_att[cb_idx].iview->image != image)
      return;

   ASSERTED unsigned cdw_max = radeon_check_space(device->ws, cs->b, 4);

   radeon_begin(cs);
   radeon_set_context_reg_seq(R_028C8C_CB_COLOR0_CLEAR_WORD0 + cb_idx * 0x3c, 2);
   radeon_emit(color_values[0]);
   radeon_emit(color_values[1]);
   radeon_end();

   assert(cs->b->cdw <= cdw_max);

   cmd_buffer->cs->context_roll_without_scissor_emitted = true;
}

/**
 * Set the clear color values to the image's metadata.
 */
static void
radv_set_color_clear_metadata(struct radv_cmd_buffer *cmd_buffer, struct radv_image *image,
                              const VkImageSubresourceRange *range, uint32_t color_values[2])
{
   struct radv_device *device = radv_cmd_buffer_device(cmd_buffer);
   struct radv_cmd_stream *cs = cmd_buffer->cs;
   uint32_t level_count = vk_image_subresource_level_count(&image->vk, range);

   assert(radv_image_has_cmask(image) || radv_dcc_enabled(image, range->baseMipLevel));

   if (radv_image_has_clear_value(image)) {
      uint64_t va = radv_image_get_fast_clear_va(image, range->baseMipLevel);

      ASSERTED unsigned cdw_end =
         radv_cs_write_data_head(device, cs, V_370_PFP, va, 2 * level_count, cmd_buffer->state.predicating);

      radeon_begin(cs);

      for (uint32_t l = 0; l < level_count; l++) {
         radeon_emit(color_values[0]);
         radeon_emit(color_values[1]);
      }

      radeon_end();
      assert(cs->b->cdw == cdw_end);
   } else {
      /* Some default value we can set in the update. */
      assert(color_values[0] == 0 && color_values[1] == 0);
   }
}

/**
 * Update the clear color values for this image.
 */
void
radv_update_color_clear_metadata(struct radv_cmd_buffer *cmd_buffer, const struct radv_image_view *iview, int cb_idx,
                                 uint32_t color_values[2])
{
   struct radv_image *image = iview->image;
   VkImageSubresourceRange range = vk_image_view_subresource_range(&iview->vk);

   assert(radv_image_has_cmask(image) || radv_dcc_enabled(image, iview->vk.base_mip_level));

   /* Do not need to update the clear value for images that are fast cleared with the comp-to-single
    * mode because the hardware gets the value from the image directly.
    */
   if (iview->image->support_comp_to_single)
      return;

   radv_set_color_clear_metadata(cmd_buffer, image, &range, color_values);

   radv_update_bound_fast_clear_color(cmd_buffer, image, cb_idx, color_values);
}

/**
 * Load the clear color values from the image's metadata.
 */
static void
radv_load_color_clear_metadata(struct radv_cmd_buffer *cmd_buffer, struct radv_image_view *iview, int cb_idx)
{
   struct radv_device *device = radv_cmd_buffer_device(cmd_buffer);
   const struct radv_physical_device *pdev = radv_device_physical(device);
   struct radv_cmd_stream *cs = cmd_buffer->cs;
   struct radv_image *image = iview->image;

   if (!radv_image_has_cmask(image) && !radv_dcc_enabled(image, iview->vk.base_mip_level))
      return;

   if (iview->image->support_comp_to_single)
      return;

   if (!radv_image_has_clear_value(image)) {
      uint32_t color_values[2] = {0, 0};
      radv_update_bound_fast_clear_color(cmd_buffer, image, cb_idx, color_values);
      return;
   }

   uint64_t va = radv_image_get_fast_clear_va(image, iview->vk.base_mip_level);
   uint32_t reg = R_028C8C_CB_COLOR0_CLEAR_WORD0 + cb_idx * 0x3c;

   radeon_begin(cs);

   if (pdev->info.has_load_ctx_reg_pkt) {
      radeon_emit(PKT3(PKT3_LOAD_CONTEXT_REG_INDEX, 3, cmd_buffer->state.predicating));
      radeon_emit(va);
      radeon_emit(va >> 32);
      radeon_emit((reg - SI_CONTEXT_REG_OFFSET) >> 2);
      radeon_emit(2);
   } else {
      radeon_emit(PKT3(PKT3_COPY_DATA, 4, cmd_buffer->state.predicating));
      radeon_emit(COPY_DATA_SRC_SEL(COPY_DATA_SRC_MEM) | COPY_DATA_DST_SEL(COPY_DATA_REG) | COPY_DATA_COUNT_SEL);
      radeon_emit(va);
      radeon_emit(va >> 32);
      radeon_emit(reg >> 2);
      radeon_emit(0);

      radeon_emit(PKT3(PKT3_PFP_SYNC_ME, 0, cmd_buffer->state.predicating));
      radeon_emit(0);
   }

   radeon_end();
}

/* GFX9+ metadata cache flushing workaround. metadata cache coherency is
 * broken if the CB caches data of multiple mips of the same image at the
 * same time.
 *
 * Insert some flushes to avoid this.
 */
static void
radv_emit_fb_mip_change_flush(struct radv_cmd_buffer *cmd_buffer)
{
   struct radv_device *device = radv_cmd_buffer_device(cmd_buffer);
   const struct radv_physical_device *pdev = radv_device_physical(device);
   struct radv_rendering_state *render = &cmd_buffer->state.render;
   bool color_mip_changed = false;

   /* Entire workaround is not applicable before GFX9 */
   if (pdev->info.gfx_level < GFX9)
      return;

   for (int i = 0; i < render->color_att_count; ++i) {
      struct radv_image_view *iview = render->color_att[i].iview;
      if (!iview)
         continue;

      if ((radv_image_has_cmask(iview->image) || radv_dcc_enabled(iview->image, iview->vk.base_mip_level) ||
           radv_dcc_enabled(iview->image, cmd_buffer->state.cb_mip[i])) &&
          cmd_buffer->state.cb_mip[i] != iview->vk.base_mip_level)
         color_mip_changed = true;

      cmd_buffer->state.cb_mip[i] = iview->vk.base_mip_level;
   }

   if (color_mip_changed) {
      cmd_buffer->state.flush_bits |= RADV_CMD_FLAG_FLUSH_AND_INV_CB | RADV_CMD_FLAG_FLUSH_AND_INV_CB_META;
   }

   const struct radv_image_view *iview = render->ds_att.iview;
   if (iview) {
      if ((radv_htile_enabled(iview->image, iview->vk.base_mip_level) ||
           radv_htile_enabled(iview->image, cmd_buffer->state.ds_mip)) &&
          cmd_buffer->state.ds_mip != iview->vk.base_mip_level) {
         cmd_buffer->state.flush_bits |= RADV_CMD_FLAG_FLUSH_AND_INV_DB | RADV_CMD_FLAG_FLUSH_AND_INV_DB_META;
      }

      cmd_buffer->state.ds_mip = iview->vk.base_mip_level;
   }
}

/* This function does the flushes for mip changes if the levels are not zero for
 * all render targets. This way we can assume at the start of the next cmd_buffer
 * that rendering to mip 0 doesn't need any flushes. As that is the most common
 * case that saves some flushes. */
static void
radv_emit_mip_change_flush_default(struct radv_cmd_buffer *cmd_buffer)
{
   struct radv_device *device = radv_cmd_buffer_device(cmd_buffer);
   const struct radv_physical_device *pdev = radv_device_physical(device);

   /* Entire workaround is not applicable before GFX9 */
   if (pdev->info.gfx_level < GFX9)
      return;

   bool need_color_mip_flush = false;
   for (unsigned i = 0; i < 8; ++i) {
      if (cmd_buffer->state.cb_mip[i]) {
         need_color_mip_flush = true;
         break;
      }
   }

   if (need_color_mip_flush) {
      cmd_buffer->state.flush_bits |= RADV_CMD_FLAG_FLUSH_AND_INV_CB | RADV_CMD_FLAG_FLUSH_AND_INV_CB_META;
   }

   if (cmd_buffer->state.ds_mip) {
      cmd_buffer->state.flush_bits |= RADV_CMD_FLAG_FLUSH_AND_INV_DB | RADV_CMD_FLAG_FLUSH_AND_INV_DB_META;
   }

   memset(cmd_buffer->state.cb_mip, 0, sizeof(cmd_buffer->state.cb_mip));
   cmd_buffer->state.ds_mip = 0;
}

static void
radv_gfx11_emit_vrs_surface(struct radv_cmd_buffer *cmd_buffer)
{
   const struct radv_device *device = radv_cmd_buffer_device(cmd_buffer);
   const struct radv_physical_device *pdev = radv_device_physical(device);
   const struct radv_rendering_state *render = &cmd_buffer->state.render;
   const bool vrs_surface_enable = render->vrs_att.iview != NULL;
   struct radv_cmd_stream *cs = cmd_buffer->cs;
   unsigned xmax = 0, ymax = 0;
   uint8_t swizzle_mode = 0;
   uint64_t va = 0;

   if (vrs_surface_enable) {
      const struct radv_image_view *vrs_iview = render->vrs_att.iview;
      struct radv_image *vrs_image = vrs_iview->image;

      radv_cs_add_buffer(device->ws, cs->b, vrs_image->bindings[0].bo);

      va = vrs_image->bindings[0].addr;
      va |= vrs_image->planes[0].surface.tile_swizzle << 8;

      xmax = vrs_iview->vk.extent.width - 1;
      ymax = vrs_iview->vk.extent.height - 1;

      swizzle_mode = vrs_image->planes[0].surface.u.gfx9.swizzle_mode;
   }

   if (pdev->info.gfx_level >= GFX12) {
      radeon_begin(cs);
      gfx12_begin_context_regs();
      if (vrs_surface_enable) {
         gfx12_set_context_reg(R_0283F0_PA_SC_VRS_RATE_BASE, va >> 8);
         gfx12_set_context_reg(R_0283F4_PA_SC_VRS_RATE_BASE_EXT, S_0283F4_BASE_256B(va >> 40));
         gfx12_set_context_reg(R_0283F8_PA_SC_VRS_RATE_SIZE_XY, S_0283F8_X_MAX(xmax) | S_0283F8_Y_MAX(ymax));
         gfx12_set_context_reg(R_0283E0_PA_SC_VRS_INFO, S_0283E0_RATE_SW_MODE(swizzle_mode));
      }
      gfx12_set_context_reg(R_0283D0_PA_SC_VRS_OVERRIDE_CNTL, S_0283D0_VRS_SURFACE_ENABLE(vrs_surface_enable));
      gfx12_end_context_regs();
      radeon_end();
   } else {
      radeon_begin(cs);
      if (vrs_surface_enable) {
         radeon_set_context_reg_seq(R_0283F0_PA_SC_VRS_RATE_BASE, 3);
         radeon_emit(va >> 8);
         radeon_emit(S_0283F4_BASE_256B(va >> 40));
         radeon_emit(S_0283F8_X_MAX(xmax) | S_0283F8_Y_MAX(ymax));
      }
      radeon_set_context_reg(R_0283D0_PA_SC_VRS_OVERRIDE_CNTL, S_0283D0_VRS_SURFACE_ENABLE(vrs_surface_enable));
      radeon_end();
   }
}

static void
radv_emit_framebuffer_state(struct radv_cmd_buffer *cmd_buffer)
{
   struct radv_device *device = radv_cmd_buffer_device(cmd_buffer);
   const struct radv_physical_device *pdev = radv_device_physical(device);
   struct radv_rendering_state *render = &cmd_buffer->state.render;
   struct radv_cmd_stream *cs = cmd_buffer->cs;
   int i;
   unsigned color_invalid = pdev->info.gfx_level >= GFX12   ? S_028EC0_FORMAT(V_028EC0_COLOR_INVALID)
                            : pdev->info.gfx_level >= GFX11 ? S_028C70_FORMAT_GFX11(V_028C70_COLOR_INVALID)
                                                            : S_028C70_FORMAT_GFX6(V_028C70_COLOR_INVALID);

   ASSERTED unsigned cdw_max = radeon_check_space(device->ws, cs->b, 51 + MAX_RTS * 70);

   for (i = 0; i < render->color_att_count; ++i) {
      struct radv_image_view *iview = render->color_att[i].iview;
      if (!iview) {
         radeon_begin(cs);
         if (pdev->info.gfx_level >= GFX12) {
            radeon_set_context_reg(R_028EC0_CB_COLOR0_INFO + i * 4, color_invalid);
         } else {
            radeon_set_context_reg(R_028C70_CB_COLOR0_INFO + i * 0x3C, color_invalid);
         }
         radeon_end();
         continue;
      }

      VkImageLayout layout = render->color_att[i].layout;

      radv_cs_add_buffer(device->ws, cs->b, iview->image->bindings[0].bo);

      assert(iview->vk.aspects & (VK_IMAGE_ASPECT_COLOR_BIT | VK_IMAGE_ASPECT_PLANE_0_BIT |
                                  VK_IMAGE_ASPECT_PLANE_1_BIT | VK_IMAGE_ASPECT_PLANE_2_BIT));

      if (iview->image->disjoint && iview->vk.aspects == VK_IMAGE_ASPECT_COLOR_BIT) {
         for (uint32_t plane_id = 0; plane_id < iview->image->plane_count; plane_id++) {
            radv_cs_add_buffer(device->ws, cs->b, iview->image->bindings[plane_id].bo);
         }
      } else {
         uint32_t plane_id = iview->image->disjoint ? iview->plane_id : 0;
         radv_cs_add_buffer(device->ws, cs->b, iview->image->bindings[plane_id].bo);
      }

      if (pdev->info.gfx_level >= GFX12) {
         radv_gfx12_emit_fb_color_state(cmd_buffer, i, &render->color_att[i].cb);
      } else {
         radv_gfx6_emit_fb_color_state(cmd_buffer, i, &render->color_att[i].cb, iview, layout);
      }

      radv_load_color_clear_metadata(cmd_buffer, iview, i);
   }

   /* When there are no color outputs, always set the first color output as 32_R for RB+ depth-only. */
   if (pdev->info.rbplus_allowed && render->color_att_count == 0) {
      radeon_begin(cmd_buffer->cs);
      if (pdev->info.gfx_level >= GFX12) {
         radeon_set_context_reg(R_028EC0_CB_COLOR0_INFO + i * 4,
                                S_028EC0_FORMAT(V_028EC0_COLOR_32) | S_028EC0_NUMBER_TYPE(V_028C70_NUMBER_FLOAT));
      } else {
         const uint32_t cb_color0_info = (pdev->info.gfx_level >= GFX11 ? S_028C70_FORMAT_GFX11(V_028C70_COLOR_32)
                                                                        : S_028C70_FORMAT_GFX6(V_028C70_COLOR_32)) |
                                         S_028C70_NUMBER_TYPE(V_028C70_NUMBER_FLOAT);
         radeon_set_context_reg(R_028C70_CB_COLOR0_INFO + i * 0x3C, cb_color0_info);
      }
      radeon_end();
      ++i;
   }

   for (; i < cmd_buffer->state.last_subpass_color_count; i++) {
      radeon_begin(cs);
      if (pdev->info.gfx_level >= GFX12) {
         radeon_set_context_reg(R_028EC0_CB_COLOR0_INFO + i * 4, color_invalid);
      } else {
         radeon_set_context_reg(R_028C70_CB_COLOR0_INFO + i * 0x3C, color_invalid);
      }
      radeon_end();
   }
   cmd_buffer->state.last_subpass_color_count = render->color_att_count;

   if (render->ds_att.iview) {
      struct radv_image_view *iview = render->ds_att.iview;
      const struct radv_image *image = iview->image;
      radv_cs_add_buffer(device->ws, cs->b, image->bindings[0].bo);

      uint32_t qf_mask = radv_image_queue_family_mask(image, cmd_buffer->qf, cmd_buffer->qf);
      bool depth_compressed =
         radv_layout_is_htile_compressed(device, image, iview->vk.base_mip_level, render->ds_att.layout, qf_mask);
      bool stencil_compressed = radv_layout_is_htile_compressed(device, image, iview->vk.base_mip_level,
                                                                render->ds_att.stencil_layout, qf_mask);

      if (pdev->info.gfx_level >= GFX12) {
         radv_gfx12_emit_fb_ds_state(cmd_buffer, &render->ds_att.ds);
      } else {
         radv_gfx6_emit_fb_ds_state(cmd_buffer, &render->ds_att.ds, iview, depth_compressed, stencil_compressed);
      }

      if (depth_compressed || stencil_compressed) {
         /* Only load the depth/stencil fast clear values when
          * compressed rendering is enabled.
          */
         radv_load_ds_clear_metadata(cmd_buffer, iview);
      }
   } else if (pdev->info.gfx_level == GFX10_3 && render->vrs_att.iview && radv_cmd_buffer_get_vrs_image(cmd_buffer)) {
      /* When a subpass uses a VRS attachment without binding a depth/stencil attachment, we have to
       * bind our internal depth buffer that contains the VRS data as part of HTILE.
       */
      VkImageLayout layout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
      struct radv_buffer *htile_buffer = device->vrs.buffer;
      struct radv_image *image = device->vrs.image;
      struct radv_ds_buffer_info ds;
      struct radv_image_view iview;

      radv_image_view_init(&iview, device,
                           &(VkImageViewCreateInfo){
                              .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
                              .flags = VK_IMAGE_VIEW_CREATE_DRIVER_INTERNAL_BIT_MESA,
                              .image = radv_image_to_handle(image),
                              .viewType = radv_meta_get_view_type(image),
                              .format = image->vk.format,
                              .subresourceRange =
                                 {
                                    .aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT,
                                    .baseMipLevel = 0,
                                    .levelCount = 1,
                                    .baseArrayLayer = 0,
                                    .layerCount = 1,
                                 },
                           },
                           NULL);

      radv_initialise_vrs_surface(image, htile_buffer, &ds);

      radv_cs_add_buffer(device->ws, cs->b, htile_buffer->bo);

      bool depth_compressed = radv_layout_is_htile_compressed(
         device, image, 0, layout, radv_image_queue_family_mask(image, cmd_buffer->qf, cmd_buffer->qf));
      radv_gfx6_emit_fb_ds_state(cmd_buffer, &ds, &iview, depth_compressed, false);

      radv_image_view_finish(&iview);
   } else if (pdev->info.gfx_level >= GFX12) {
      radv_gfx12_emit_null_ds_state(cmd_buffer);
   } else {
      radv_gfx6_emit_null_ds_state(cmd_buffer);
   }

   if (pdev->info.gfx_level >= GFX11)
      radv_gfx11_emit_vrs_surface(cmd_buffer);

   assert(cs->b->cdw <= cdw_max);
}

static uint32_t
radv_gfx12_override_hiz_enable(struct radv_cmd_buffer *cmd_buffer, bool enable)
{
   const struct radv_rendering_state *render = &cmd_buffer->state.render;
   const struct radv_ds_buffer_info *ds = &render->ds_att.ds;
   struct radv_cmd_stream *cs = cmd_buffer->cs;
   uint32_t hiz_info = ds->ac.u.gfx12.hiz_info;
   const uint32_t cdw = cs->b->cdw;

   if (!enable)
      hiz_info &= C_028B94_SURFACE_ENABLE;

   radeon_begin(cs);
   gfx12_begin_context_regs();
   gfx12_set_context_reg(R_028B94_PA_SC_HIZ_INFO, hiz_info);
   gfx12_end_context_regs();
   radeon_end();

   return cs->b->cdw - cdw;
}

static void
radv_gfx12_emit_hiz_wa_full(struct radv_cmd_buffer *cmd_buffer)
{
   const struct radv_device *device = radv_cmd_buffer_device(cmd_buffer);
   const struct radv_physical_device *pdev = radv_device_physical(device);
   const struct radv_rendering_state *render = &cmd_buffer->state.render;
   const struct radv_image_view *iview = render->ds_att.iview;
   const struct radv_dynamic_state *d = &cmd_buffer->state.dynamic;

   if (!iview || !iview->image->hiz_valid_offset)
      return;

   struct vk_depth_stencil_state ds = d->vk.ds;
   vk_optimize_depth_stencil_state(&ds, render->ds_att_aspects, true);

   const bool depth_and_stencil_enable =
      (ds.depth.test_enable || ds.depth.write_enable) && (ds.stencil.test_enable || ds.stencil.write_enable);
   const bool depth_write_enable = ds.depth.write_enable;

   const uint32_t num_dwords = radv_gfx12_override_hiz_enable(cmd_buffer, false);

   if (depth_and_stencil_enable) {
      if (depth_write_enable) {
         VkImageSubresourceRange range = {
            .aspectMask = render->ds_att_aspects,
            .baseMipLevel = iview->vk.base_mip_level,
            .levelCount = iview->vk.level_count,
            .baseArrayLayer = iview->vk.base_array_layer,
            .layerCount = iview->vk.layer_count,
         };

         /* Mark HiZ metadata as invalid because HiZ will be disabled and metadata will be
          * out-of-sync with main image data.
          */
         radv_update_hiz_metadata(cmd_buffer, iview->image, &range, false);
      }
   } else {
      const uint64_t va = radv_get_hiz_valid_va(iview->image, iview->vk.base_mip_level);

      ac_emit_cond_exec(cmd_buffer->cs->b, pdev->info.gfx_level, va, num_dwords);

      radv_gfx12_override_hiz_enable(cmd_buffer, true);
   }
}

static void
radv_emit_guardband_state(struct radv_cmd_buffer *cmd_buffer)
{
   struct radv_device *device = radv_cmd_buffer_device(cmd_buffer);
   const struct radv_physical_device *pdev = radv_device_physical(device);
   const struct radv_dynamic_state *d = &cmd_buffer->state.dynamic;
   unsigned vgt_outprim_type = cmd_buffer->state.vgt_outprim_type;
   const bool draw_points =
      radv_vgt_outprim_is_point(vgt_outprim_type) || radv_polygon_mode_is_point(d->vk.rs.polygon_mode);
   const bool draw_lines =
      radv_vgt_outprim_is_line(vgt_outprim_type) || radv_polygon_mode_is_line(d->vk.rs.polygon_mode);
   struct radv_cmd_stream *cs = cmd_buffer->cs;
   int i;
   float guardband_x = INFINITY, guardband_y = INFINITY;
   float discard_x = 1.0f, discard_y = 1.0f;
   const float max_range = 32767.0f;

   if (!d->vk.vp.viewport_count)
      return;

   for (i = 0; i < d->vk.vp.viewport_count; i++) {
      float scale_x = fabsf(d->vp_xform[i].scale[0]);
      float scale_y = fabsf(d->vp_xform[i].scale[1]);
      const float translate_x = fabsf(d->vp_xform[i].translate[0]);
      const float translate_y = fabsf(d->vp_xform[i].translate[1]);

      if (scale_x < 0.5)
         scale_x = 0.5;
      if (scale_y < 0.5)
         scale_y = 0.5;

      guardband_x = MIN2(guardband_x, (max_range - translate_x) / scale_x);
      guardband_y = MIN2(guardband_y, (max_range - translate_y) / scale_y);

      if (draw_points || draw_lines) {
         /* When rendering wide points or lines, we need to be more conservative about when to
          * discard them entirely. */
         float pixels;

         if (draw_points) {
            pixels = 8191.875f;
         } else {
            pixels = d->vk.rs.line.width;
         }

         /* Add half the point size / line width. */
         discard_x += pixels / (2.0 * scale_x);
         discard_y += pixels / (2.0 * scale_y);

         /* Discard primitives that would lie entirely outside the clip region. */
         discard_x = MIN2(discard_x, guardband_x);
         discard_y = MIN2(discard_y, guardband_y);
      }
   }

   radeon_begin(cs);
   if (pdev->info.gfx_level >= GFX12) {
      radeon_set_context_reg_seq(R_02842C_PA_CL_GB_VERT_CLIP_ADJ, 4);
   } else {
      radeon_set_context_reg_seq(R_028BE8_PA_CL_GB_VERT_CLIP_ADJ, 4);
   }
   radeon_emit(fui(guardband_y));
   radeon_emit(fui(discard_y));
   radeon_emit(fui(guardband_x));
   radeon_emit(fui(discard_x));
   radeon_end();
}

/* Bind an internal index buffer for GPUs that hang with 0-sized index buffers to handle robustness2
 * which requires 0 for out-of-bounds access.
 */
static void
radv_handle_zero_index_buffer_bug(struct radv_cmd_buffer *cmd_buffer, uint64_t *index_va, uint32_t *remaining_indexes)
{
   const uint32_t zero = 0;
   uint32_t offset;

   if (!radv_cmd_buffer_upload_data(cmd_buffer, sizeof(uint32_t), &zero, &offset)) {
      vk_command_buffer_set_error(&cmd_buffer->vk, VK_ERROR_OUT_OF_HOST_MEMORY);
      return;
   }

   *index_va = radv_buffer_get_va(cmd_buffer->upload.upload_bo) + offset;
   *remaining_indexes = 1;
}

static void
radv_emit_index_buffer(struct radv_cmd_buffer *cmd_buffer)
{
   struct radv_device *device = radv_cmd_buffer_device(cmd_buffer);
   const struct radv_physical_device *pdev = radv_device_physical(device);
   struct radv_cmd_stream *cs = cmd_buffer->cs;
   struct radv_cmd_state *state = &cmd_buffer->state;
   uint32_t max_index_count = state->max_index_count;
   uint64_t index_va = state->index_va;

   /* With indirect generated commands the index buffer bind may be part of the
    * indirect command buffer, in which case the app may not have bound any yet. */
   if (state->index_type < 0)
      return;

   /* Handle indirect draw calls with NULL index buffer if the GPU doesn't support them. */
   if (!max_index_count && pdev->info.has_zero_index_buffer_bug) {
      radv_handle_zero_index_buffer_bug(cmd_buffer, &index_va, &max_index_count);
   }

   radeon_begin(cs);
   radeon_emit(PKT3(PKT3_INDEX_BASE, 1, 0));
   radeon_emit(index_va);
   radeon_emit(index_va >> 32);

   radeon_emit(PKT3(PKT3_INDEX_BUFFER_SIZE, 0, 0));
   radeon_emit(max_index_count);
   radeon_end();
}

static void
radv_emit_occlusion_query_state(struct radv_cmd_buffer *cmd_buffer)
{
   struct radv_device *device = radv_cmd_buffer_device(cmd_buffer);
   const struct radv_physical_device *pdev = radv_device_physical(device);
   const enum amd_gfx_level gfx_level = pdev->info.gfx_level;
   const bool enable_occlusion_queries =
      cmd_buffer->state.active_occlusion_queries || cmd_buffer->state.inherited_occlusion_queries;
   uint32_t db_count_control;

   if (!enable_occlusion_queries) {
      db_count_control = S_028004_ZPASS_INCREMENT_DISABLE(gfx_level < GFX11);
   } else {
      bool gfx10_perfect =
         gfx_level >= GFX10 && (cmd_buffer->state.perfect_occlusion_queries_enabled ||
                                cmd_buffer->state.inherited_query_control_flags & VK_QUERY_CONTROL_PRECISE_BIT);

      if (gfx_level >= GFX7) {
         /* Always enable PERFECT_ZPASS_COUNTS due to issues with partially
          * covered tiles, discards, and early depth testing. For more details,
          * see https://gitlab.freedesktop.org/mesa/mesa/-/issues/3218 */
         db_count_control = S_028004_PERFECT_ZPASS_COUNTS(1) |
                            S_028004_DISABLE_CONSERVATIVE_ZPASS_COUNTS(gfx10_perfect) | S_028004_ZPASS_ENABLE(1) |
                            S_028004_SLICE_EVEN_ENABLE(1) | S_028004_SLICE_ODD_ENABLE(1);
      } else {
         db_count_control = S_028004_PERFECT_ZPASS_COUNTS(1);
      }

      if (gfx_level < GFX12) {
         const uint32_t rasterization_samples = cmd_buffer->state.num_rast_samples;
         const uint32_t sample_rate = util_logbase2(rasterization_samples);

         db_count_control |= S_028004_SAMPLE_RATE(sample_rate);
      }
   }

   radeon_begin(cmd_buffer->cs);

   if (pdev->info.gfx_level >= GFX12) {
      radeon_opt_set_context_reg(R_028060_DB_COUNT_CONTROL, RADV_TRACKED_DB_COUNT_CONTROL, db_count_control);
   } else {
      radeon_opt_set_context_reg(R_028004_DB_COUNT_CONTROL, RADV_TRACKED_DB_COUNT_CONTROL, db_count_control);
   }

   radeon_end();
}

unsigned
radv_instance_rate_prolog_index(unsigned num_attributes, uint32_t instance_rate_inputs)
{
   /* instance_rate_vs_prologs is a flattened array of array of arrays of different sizes, or a
    * single array sorted in ascending order using:
    * - total number of attributes
    * - number of instanced attributes
    * - index of first instanced attribute
    */

   /* From total number of attributes to offset. */
   static const uint16_t total_to_offset[16] = {0, 1, 4, 10, 20, 35, 56, 84, 120, 165, 220, 286, 364, 455, 560, 680};
   unsigned start_index = total_to_offset[num_attributes - 1];

   /* From number of instanced attributes to offset. This would require a different LUT depending on
    * the total number of attributes, but we can exploit a pattern to use just the LUT for 16 total
    * attributes.
    */
   static const uint8_t count_to_offset_total16[16] = {0,   16,  31,  45,  58,  70,  81,  91,
                                                       100, 108, 115, 121, 126, 130, 133, 135};
   unsigned count = util_bitcount(instance_rate_inputs);
   unsigned offset_from_start_index = count_to_offset_total16[count - 1] - ((16 - num_attributes) * (count - 1));

   unsigned first = ffs(instance_rate_inputs) - 1;
   return start_index + offset_from_start_index + first;
}

static struct radv_shader_part *
lookup_vs_prolog(struct radv_cmd_buffer *cmd_buffer, const struct radv_shader *vs_shader, uint32_t *nontrivial_divisors)
{
   assert(vs_shader->info.vs.dynamic_inputs);

   struct radv_device *device = radv_cmd_buffer_device(cmd_buffer);
   const struct radv_physical_device *pdev = radv_device_physical(device);
   struct radv_dynamic_state *d = &cmd_buffer->state.dynamic;

   unsigned num_attributes = vs_shader->info.vs.num_attributes;
   uint32_t attribute_mask = vs_shader->info.vs.vb_desc_usage_mask;

   uint32_t instance_rate_inputs = d->vertex_input.instance_rate_inputs & attribute_mask;
   uint32_t zero_divisors = d->vertex_input.zero_divisors & attribute_mask;
   *nontrivial_divisors = d->vertex_input.nontrivial_divisors & attribute_mask;
   uint32_t misaligned_mask = d->vertex_input.vbo_misaligned_mask;
   uint32_t unaligned_mask = d->vertex_input.vbo_unaligned_mask;
   if (d->vertex_input.vbo_misaligned_mask_invalid) {
      bool misalignment_possible = pdev->info.gfx_level == GFX6 || pdev->info.gfx_level >= GFX10;
      u_foreach_bit (index, d->vertex_input.vbo_misaligned_mask_invalid & attribute_mask) {
         uint8_t binding = d->vertex_input.bindings[index];
         if (!(cmd_buffer->state.vbo_bound_mask & BITFIELD_BIT(binding)))
            continue;

         uint8_t format_req = d->vertex_input.format_align_req_minus_1[index];
         uint8_t component_req = d->vertex_input.component_align_req_minus_1[index];
         uint64_t vb_addr = cmd_buffer->vertex_bindings[binding].addr;
         uint64_t vb_stride = d->vk.vi_binding_strides[binding];

         VkDeviceSize addr = vb_addr + d->vertex_input.offsets[index];

         if (misalignment_possible && ((addr | vb_stride) & format_req))
            misaligned_mask |= BITFIELD_BIT(index);
         if ((addr | vb_stride) & component_req)
            unaligned_mask |= BITFIELD_BIT(index);
      }
      d->vertex_input.vbo_misaligned_mask = misaligned_mask;
      d->vertex_input.vbo_unaligned_mask = unaligned_mask;
      d->vertex_input.vbo_misaligned_mask_invalid &= ~attribute_mask;
   }
   misaligned_mask |= d->vertex_input.nontrivial_formats | unaligned_mask;
   misaligned_mask &= attribute_mask;
   unaligned_mask &= attribute_mask;

   /* The instance ID input VGPR is placed differently when as_ls=true. as_ls is also needed to
    * workaround the LS VGPR initialization bug.
    */
   bool as_ls = vs_shader->info.vs.as_ls && (instance_rate_inputs || pdev->info.has_ls_vgpr_init_bug);

   /* try to use a pre-compiled prolog first */
   struct radv_shader_part *prolog = NULL;
   if (cmd_buffer->state.can_use_simple_vertex_input && !as_ls && !misaligned_mask &&
       !d->vertex_input.alpha_adjust_lo && !d->vertex_input.alpha_adjust_hi) {
      if (!instance_rate_inputs) {
         prolog = device->simple_vs_prologs[num_attributes - 1];
      } else if (num_attributes <= 16 && !*nontrivial_divisors && !zero_divisors &&
                 util_bitcount(instance_rate_inputs) ==
                    (util_last_bit(instance_rate_inputs) - ffs(instance_rate_inputs) + 1)) {
         unsigned index = radv_instance_rate_prolog_index(num_attributes, instance_rate_inputs);
         prolog = device->instance_rate_vs_prologs[index];
      }
   }
   if (prolog)
      return prolog;

   struct radv_vs_prolog_key key;
   memset(&key, 0, sizeof(key));
   key.instance_rate_inputs = instance_rate_inputs;
   key.nontrivial_divisors = *nontrivial_divisors;
   key.zero_divisors = zero_divisors;
   /* If the attribute is aligned, post shuffle is implemented using DST_SEL instead. */
   key.post_shuffle = d->vertex_input.post_shuffle & misaligned_mask;
   key.alpha_adjust_hi = d->vertex_input.alpha_adjust_hi & attribute_mask & ~unaligned_mask;
   key.alpha_adjust_lo = d->vertex_input.alpha_adjust_lo & attribute_mask & ~unaligned_mask;
   u_foreach_bit (index, misaligned_mask)
      key.formats[index] = d->vertex_input.formats[index];
   key.num_attributes = num_attributes;
   key.misaligned_mask = misaligned_mask;
   key.unaligned_mask = unaligned_mask;
   key.as_ls = as_ls;
   key.is_ngg = vs_shader->info.is_ngg;
   key.wave32 = vs_shader->info.wave_size == 32;

   if (vs_shader->info.merged_shader_compiled_separately) {
      assert(vs_shader->info.next_stage == MESA_SHADER_TESS_CTRL || vs_shader->info.next_stage == MESA_SHADER_GEOMETRY);
      key.next_stage = vs_shader->info.next_stage;
   } else {
      key.next_stage = vs_shader->info.stage;
   }

   return radv_shader_part_cache_get(device, &device->vs_prologs, &cmd_buffer->vs_prologs, &key);
}

static void
emit_prolog_regs(struct radv_cmd_buffer *cmd_buffer, const struct radv_shader *vs_shader,
                 const struct radv_shader_part *prolog)
{
   struct radv_device *device = radv_cmd_buffer_device(cmd_buffer);
   const struct radv_physical_device *pdev = radv_device_physical(device);
   struct radv_cmd_stream *cs = cmd_buffer->cs;
   uint32_t rsrc1, rsrc2;

   /* no need to re-emit anything in this case */
   if (cmd_buffer->state.emitted_vs_prolog == prolog)
      return;

   enum amd_gfx_level chip = pdev->info.gfx_level;

   assert(cmd_buffer->state.emitted_graphics_pipeline == cmd_buffer->state.graphics_pipeline);

   if (vs_shader->info.merged_shader_compiled_separately) {
      if (vs_shader->info.next_stage == MESA_SHADER_GEOMETRY) {
         radv_shader_combine_cfg_vs_gs(device, vs_shader, cmd_buffer->state.shaders[MESA_SHADER_GEOMETRY], &rsrc1,
                                       &rsrc2);
      } else {
         assert(vs_shader->info.next_stage == MESA_SHADER_TESS_CTRL);

         radv_shader_combine_cfg_vs_tcs(vs_shader, cmd_buffer->state.shaders[MESA_SHADER_TESS_CTRL], &rsrc1, &rsrc2);
      }
   } else {
      rsrc1 = vs_shader->config.rsrc1;
   }

   if (chip < GFX10 && G_00B228_SGPRS(prolog->rsrc1) > G_00B228_SGPRS(rsrc1))
      rsrc1 = (rsrc1 & C_00B228_SGPRS) | (prolog->rsrc1 & ~C_00B228_SGPRS);

   if (G_00B848_VGPRS(prolog->rsrc1) > G_00B848_VGPRS(rsrc1))
      rsrc1 = (rsrc1 & C_00B848_VGPRS) | (prolog->rsrc1 & ~C_00B848_VGPRS);

   radeon_begin(cs);
   if (pdev->info.gfx_level >= GFX12) {
      gfx12_push_sh_reg(vs_shader->info.regs.pgm_lo, prolog->va >> 8);
      gfx12_push_sh_reg(vs_shader->info.regs.pgm_rsrc1, rsrc1);
      if (vs_shader->info.merged_shader_compiled_separately)
         gfx12_push_sh_reg(vs_shader->info.regs.pgm_rsrc2, rsrc2);
   } else {
      radeon_set_sh_reg(vs_shader->info.regs.pgm_lo, prolog->va >> 8);
      radeon_set_sh_reg(vs_shader->info.regs.pgm_rsrc1, rsrc1);
      if (vs_shader->info.merged_shader_compiled_separately)
         radeon_set_sh_reg(vs_shader->info.regs.pgm_rsrc2, rsrc2);
   }
   radeon_end();

   radv_cs_add_buffer(device->ws, cs->b, prolog->bo);
}

static void
emit_prolog_inputs(struct radv_cmd_buffer *cmd_buffer, const struct radv_shader *vs_shader,
                   uint32_t nontrivial_divisors)
{
   const struct radv_device *device = radv_cmd_buffer_device(cmd_buffer);
   const struct radv_physical_device *pdev = radv_device_physical(device);

   /* no need to re-emit anything in this case */
   if (!nontrivial_divisors && cmd_buffer->state.emitted_vs_prolog &&
       !cmd_buffer->state.emitted_vs_prolog->nontrivial_divisors)
      return;

   const struct radv_dynamic_state *d = &cmd_buffer->state.dynamic;
   uint64_t input_va = radv_shader_get_va(vs_shader);

   if (nontrivial_divisors) {
      unsigned inputs_offset;
      uint32_t *inputs;
      unsigned size = 8 + util_bitcount(nontrivial_divisors) * 8;
      if (!radv_cmd_buffer_upload_alloc(cmd_buffer, size, &inputs_offset, (void **)&inputs))
         return;

      *(inputs++) = input_va;
      *(inputs++) = input_va >> 32;

      u_foreach_bit (index, nontrivial_divisors) {
         uint32_t div = d->vertex_input.divisors[index];
         if (div == 0) {
            *(inputs++) = 0;
            *(inputs++) = 1;
         } else if (util_is_power_of_two_or_zero(div)) {
            *(inputs++) = util_logbase2(div) | (1 << 8);
            *(inputs++) = 0xffffffffu;
         } else {
            struct util_fast_udiv_info info = util_compute_fast_udiv_info(div, 32, 32);
            *(inputs++) = info.pre_shift | (info.increment << 8) | (info.post_shift << 16);
            *(inputs++) = info.multiplier;
         }
      }

      input_va = radv_buffer_get_va(cmd_buffer->upload.upload_bo) + inputs_offset;
   }

   const uint32_t vs_prolog_inputs_offset = radv_get_user_sgpr_loc(vs_shader, AC_UD_VS_PROLOG_INPUTS);
   radeon_begin(cmd_buffer->cs);
   if (pdev->info.gfx_level >= GFX12) {
      gfx12_push_64bit_pointer(vs_prolog_inputs_offset, input_va);
   } else {
      radeon_emit_64bit_pointer(vs_prolog_inputs_offset, input_va);
   }
   radeon_end();
}

static void
radv_emit_vs_prolog_state(struct radv_cmd_buffer *cmd_buffer)
{
   const struct radv_shader *vs_shader = radv_get_shader(cmd_buffer->state.shaders, MESA_SHADER_VERTEX);
   struct radv_device *device = radv_cmd_buffer_device(cmd_buffer);

   if (!vs_shader || !vs_shader->info.vs.has_prolog)
      return;

   uint32_t nontrivial_divisors;
   struct radv_shader_part *prolog = lookup_vs_prolog(cmd_buffer, vs_shader, &nontrivial_divisors);
   if (!prolog) {
      vk_command_buffer_set_error(&cmd_buffer->vk, VK_ERROR_OUT_OF_HOST_MEMORY);
      return;
   }
   emit_prolog_regs(cmd_buffer, vs_shader, prolog);
   emit_prolog_inputs(cmd_buffer, vs_shader, nontrivial_divisors);

   cmd_buffer->shader_upload_seq = MAX2(cmd_buffer->shader_upload_seq, prolog->upload_seq);

   cmd_buffer->state.emitted_vs_prolog = prolog;

   if (radv_device_fault_detection_enabled(device))
      radv_save_vs_prolog(cmd_buffer, prolog);
}

static void
radv_emit_tess_domain_origin_state(struct radv_cmd_buffer *cmd_buffer)
{
   struct radv_device *device = radv_cmd_buffer_device(cmd_buffer);
   const struct radv_physical_device *pdev = radv_device_physical(device);
   const struct radv_shader *tes = radv_get_shader(cmd_buffer->state.shaders, MESA_SHADER_TESS_EVAL);
   const struct radv_dynamic_state *d = &cmd_buffer->state.dynamic;
   unsigned type = 0, partitioning = 0;
   unsigned topology;

   if (!tes)
      return;

   switch (tes->info.tes._primitive_mode) {
   case TESS_PRIMITIVE_TRIANGLES:
      type = V_028B6C_TESS_TRIANGLE;
      break;
   case TESS_PRIMITIVE_QUADS:
      type = V_028B6C_TESS_QUAD;
      break;
   case TESS_PRIMITIVE_ISOLINES:
      type = V_028B6C_TESS_ISOLINE;
      break;
   default:
      UNREACHABLE("Invalid tess primitive type");
   }

   switch (tes->info.tes.spacing) {
   case TESS_SPACING_EQUAL:
      partitioning = V_028B6C_PART_INTEGER;
      break;
   case TESS_SPACING_FRACTIONAL_ODD:
      partitioning = V_028B6C_PART_FRAC_ODD;
      break;
   case TESS_SPACING_FRACTIONAL_EVEN:
      partitioning = V_028B6C_PART_FRAC_EVEN;
      break;
   default:
      UNREACHABLE("Invalid tess spacing type");
   }

   if (tes->info.tes.point_mode) {
      topology = V_028B6C_OUTPUT_POINT;
   } else if (tes->info.tes._primitive_mode == TESS_PRIMITIVE_ISOLINES) {
      topology = V_028B6C_OUTPUT_LINE;
   } else {
      bool ccw = tes->info.tes.ccw;

      if (d->vk.ts.domain_origin != VK_TESSELLATION_DOMAIN_ORIGIN_UPPER_LEFT) {
         ccw = !ccw;
      }

      topology = ccw ? V_028B6C_OUTPUT_TRIANGLE_CCW : V_028B6C_OUTPUT_TRIANGLE_CW;
   }

   uint32_t vgt_tf_param = S_028B6C_TYPE(type) | S_028B6C_PARTITIONING(partitioning) | S_028B6C_TOPOLOGY(topology) |
                           S_028B6C_DISTRIBUTION_MODE(pdev->tess_distribution_mode);

   radeon_begin(cmd_buffer->cs);
   if (pdev->info.gfx_level >= GFX12) {
      vgt_tf_param |= S_028AA4_TEMPORAL(gfx12_load_last_use_discard);

      radeon_set_context_reg(R_028AA4_VGT_TF_PARAM, vgt_tf_param);
   } else {
      radeon_set_context_reg(R_028B6C_VGT_TF_PARAM, vgt_tf_param);
   }
   radeon_end();
}

static bool
radv_is_dual_src_enabled(const struct radv_dynamic_state *dynamic_state)
{
   /* Dual-source blending must be ignored if blending isn't enabled for MRT0. */
   return dynamic_state->blend_eq.mrt0_is_dual_src && !!(dynamic_state->color_blend_enable & 1u);
}

static struct radv_shader_part *
lookup_ps_epilog(struct radv_cmd_buffer *cmd_buffer)
{
   struct radv_device *device = radv_cmd_buffer_device(cmd_buffer);
   const struct radv_physical_device *pdev = radv_device_physical(device);
   const struct radv_shader *ps = cmd_buffer->state.shaders[MESA_SHADER_FRAGMENT];
   const struct radv_rendering_state *render = &cmd_buffer->state.render;
   const struct radv_dynamic_state *d = &cmd_buffer->state.dynamic;
   struct radv_ps_epilog_state state = {0};
   uint8_t color_remap[MAX_RTS];

   memset(color_remap, MESA_VK_ATTACHMENT_UNUSED, sizeof(color_remap));

   state.color_attachment_count = render->color_att_count;
   for (unsigned i = 0; i < render->color_att_count; ++i) {
      const uint32_t cb_blend_control = d->blend_eq.att[i].cb_blend_control;
      const uint32_t src_blend = G_028780_COLOR_SRCBLEND(cb_blend_control);
      const uint32_t dst_blend = G_028780_COLOR_DESTBLEND(cb_blend_control);

      state.color_attachment_formats[i] = render->color_att[i].format;

      if (src_blend == V_028780_BLEND_SRC_ALPHA || src_blend == V_028780_BLEND_ONE_MINUS_SRC_ALPHA ||
          src_blend == V_028780_BLEND_SRC_ALPHA_SATURATE || dst_blend == V_028780_BLEND_SRC_ALPHA ||
          dst_blend == V_028780_BLEND_ONE_MINUS_SRC_ALPHA || dst_blend == V_028780_BLEND_SRC_ALPHA_SATURATE)
         state.need_src_alpha |= 1 << i;

      state.color_attachment_mappings[i] = d->vk.cal.color_map[i];
      if (state.color_attachment_mappings[i] != MESA_VK_ATTACHMENT_UNUSED)
         color_remap[state.color_attachment_mappings[i]] = i;
   }

   state.color_write_mask = d->color_write_mask;
   state.color_blend_enable = d->color_blend_enable;
   state.mrt0_is_dual_src = radv_is_dual_src_enabled(&cmd_buffer->state.dynamic);

   if (d->vk.ms.alpha_to_coverage_enable) {
      /* Select a color export format with alpha when alpha to coverage is enabled. */
      state.need_src_alpha |= 0x1;
   }

   state.alpha_to_one = d->vk.ms.alpha_to_one_enable;
   state.colors_written = ps->info.ps.colors_written;

   if (ps->info.ps.exports_mrtz_via_epilog) {
      const bool export_z_stencil_samplemask =
         ps->info.ps.writes_z || ps->info.ps.writes_stencil || ps->info.ps.writes_sample_mask;

      state.export_depth = ps->info.ps.writes_z;
      state.export_stencil = ps->info.ps.writes_stencil;
      state.export_sample_mask = ps->info.ps.writes_sample_mask;

      if (d->vk.ms.alpha_to_coverage_enable) {
         /* We need coverage-to-mask when alpha-to-one is also enabled. On GFX11, it's always
          * enabled if there's a mrtz export.
          */
         const bool coverage_to_mask =
            d->vk.ms.alpha_to_one_enable || (pdev->info.gfx_level >= GFX11 && export_z_stencil_samplemask);
         state.alpha_to_coverage_via_mrtz = coverage_to_mask;
      }
   }

   struct radv_ps_epilog_key key = radv_generate_ps_epilog_key(device, &state);

   /* Adjust the remapping for alpha-to-coverage without any color attachment and dual-source
    * blending to make sure colors written aren't cleared.
    */
   if (!state.color_attachment_count && state.need_src_alpha)
      color_remap[0] = 0;
   if (state.mrt0_is_dual_src)
      color_remap[1] = 1;

   /* Determine the actual colors written if outputs are remapped. */
   uint32_t colors_written = 0;
   for (uint32_t i = 0; i < MAX_RTS; i++) {
      if (!((ps->info.ps.colors_written >> (i * 4)) & 0xf))
         continue;

      if (color_remap[i] == MESA_VK_ATTACHMENT_UNUSED)
         continue;

      colors_written |= 0xfu << (4 * color_remap[i]);
   }

   /* Clear color attachments that aren't exported by the FS to match IO shader arguments. */
   key.spi_shader_col_format &= colors_written;

   return radv_shader_part_cache_get(device, &device->ps_epilogs, &cmd_buffer->ps_epilogs, &key);
}

static void
radv_flush_push_descriptors(struct radv_cmd_buffer *cmd_buffer, struct radv_descriptor_state *descriptors_state)
{
   struct radv_descriptor_set *set = (struct radv_descriptor_set *)&descriptors_state->push_set.set;
   unsigned bo_offset;

   if (!radv_cmd_buffer_upload_data(cmd_buffer, set->header.size, set->header.mapped_ptr, &bo_offset))
      return;

   set->header.va = radv_buffer_get_va(cmd_buffer->upload.upload_bo);
   set->header.va += bo_offset;
}

void
radv_upload_indirect_descriptor_sets(struct radv_cmd_buffer *cmd_buffer,
                                     struct radv_descriptor_state *descriptors_state)
{
   const uint32_t last_valid_desc = util_last_bit(descriptors_state->valid);
   const uint32_t size = last_valid_desc * 4;
   uint32_t offset;
   void *ptr;

   if (!radv_cmd_buffer_upload_alloc(cmd_buffer, size, &offset, &ptr))
      return;

   descriptors_state->indirect_descriptor_sets_va = radv_buffer_get_va(cmd_buffer->upload.upload_bo) + offset;

   for (unsigned i = 0; i < last_valid_desc; i++) {
      uint32_t *uptr = ((uint32_t *)ptr) + i;
      uint64_t set_va = 0;
      if (descriptors_state->valid & (1u << i))
         set_va = radv_descriptor_get_va(descriptors_state, i);

      uptr[0] = set_va & 0xffffffff;
   }
}

ALWAYS_INLINE static void
radv_flush_descriptors(struct radv_cmd_buffer *cmd_buffer, VkShaderStageFlags stages, VkPipelineBindPoint bind_point)
{
   struct radv_descriptor_state *descriptors_state = radv_get_descriptors_state(cmd_buffer, bind_point);
   struct radv_device *device = radv_cmd_buffer_device(cmd_buffer);
   struct radv_cmd_stream *cs = cmd_buffer->cs;

   if (descriptors_state->need_indirect_descriptors)
      radv_upload_indirect_descriptor_sets(cmd_buffer, descriptors_state);

   ASSERTED unsigned cdw_max = radeon_check_space(device->ws, cs->b, MAX_SETS * MESA_VULKAN_SHADER_STAGES * 4);

   if (stages & VK_SHADER_STAGE_COMPUTE_BIT) {
      const struct radv_shader *compute_shader = bind_point == VK_PIPELINE_BIND_POINT_COMPUTE
                                                    ? cmd_buffer->state.shaders[MESA_SHADER_COMPUTE]
                                                    : cmd_buffer->state.rt_prolog;

      radv_emit_descriptors_per_stage(device, cs, compute_shader, descriptors_state);
   } else {
      radv_foreach_stage (stage, stages & ~VK_SHADER_STAGE_TASK_BIT_EXT) {
         if (!cmd_buffer->state.shaders[stage])
            continue;

         radv_emit_descriptors_per_stage(device, cs, cmd_buffer->state.shaders[stage], descriptors_state);
      }

      if (stages & VK_SHADER_STAGE_TASK_BIT_EXT) {
         radv_emit_descriptors_per_stage(device, cmd_buffer->gang.cs, cmd_buffer->state.shaders[MESA_SHADER_TASK],
                                         descriptors_state);
      }
   }

   assert(cs->b->cdw <= cdw_max);

   if (radv_device_fault_detection_enabled(device))
      radv_save_descriptors(cmd_buffer, bind_point);
}

ALWAYS_INLINE static VkShaderStageFlags
radv_must_flush_constants(const struct radv_cmd_buffer *cmd_buffer, VkShaderStageFlags stages,
                          VkPipelineBindPoint bind_point)
{
   const struct radv_push_constant_state *push_constants = radv_get_push_constants_state(cmd_buffer, bind_point);

   if (push_constants->size)
      return stages & cmd_buffer->push_constant_stages;

   return 0;
}

static void
radv_emit_push_constants_per_stage(const struct radv_device *device, struct radv_cmd_stream *cs,
                                   const struct radv_shader *shader, uint32_t *values, uint64_t push_constants_va)
{
   const struct radv_physical_device *pdev = radv_device_physical(device);
   const uint32_t push_constants_offset = radv_get_user_sgpr_loc(shader, AC_UD_PUSH_CONSTANTS);
   const uint64_t inline_push_const_mask = shader->info.inline_push_constant_mask;

   /* Emit inlined push constants. */
   if (inline_push_const_mask) {
      const uint8_t base = ffs(inline_push_const_mask) - 1;

      if (inline_push_const_mask == u_bit_consecutive64(base, util_last_bit64(inline_push_const_mask) - base)) {
         /* consecutive inline push constants */
         radv_emit_inline_push_consts(device, cs, shader, AC_UD_INLINE_PUSH_CONSTANTS, values + base);
      } else {
         /* sparse inline push constants */
         uint32_t consts[AC_MAX_INLINE_PUSH_CONSTS];
         unsigned num_consts = 0;
         u_foreach_bit64 (idx, inline_push_const_mask)
            consts[num_consts++] = values[idx];
         radv_emit_inline_push_consts(device, cs, shader, AC_UD_INLINE_PUSH_CONSTANTS, consts);
      }
   }

   /* Emit the push constants upload pointer. */
   if (push_constants_offset) {
      radeon_check_space(device->ws, cs->b, 3);
      radeon_begin(cs);
      if (pdev->info.gfx_level >= GFX12) {
         gfx12_push_32bit_pointer(push_constants_offset, push_constants_va, &pdev->info);
      } else {
         radeon_emit_32bit_pointer(push_constants_offset, push_constants_va, &pdev->info);
      }
      radeon_end();
   }
}

static void
radv_upload_push_constants(struct radv_cmd_buffer *cmd_buffer, const struct radv_push_constant_state *pc_state,
                           uint64_t *va)
{
   unsigned offset;
   void *ptr;

   if (!radv_cmd_buffer_upload_alloc(cmd_buffer, pc_state->size, &offset, &ptr))
      return;

   memcpy(ptr, cmd_buffer->push_constants, pc_state->size);

   *va = radv_buffer_get_va(cmd_buffer->upload.upload_bo) + offset;
}

static void
radv_flush_constants(struct radv_cmd_buffer *cmd_buffer, VkShaderStageFlags stages, VkPipelineBindPoint bind_point)
{
   struct radv_device *device = radv_cmd_buffer_device(cmd_buffer);
   struct radv_cmd_stream *cs = cmd_buffer->cs;
   const struct radv_push_constant_state *push_constants = radv_get_push_constants_state(cmd_buffer, bind_point);
   uint64_t va = 0;
   uint32_t internal_stages = stages;

   switch (bind_point) {
   case VK_PIPELINE_BIND_POINT_GRAPHICS:
      break;
   case VK_PIPELINE_BIND_POINT_COMPUTE:
      break;
   case VK_PIPELINE_BIND_POINT_RAY_TRACING_KHR:
      internal_stages = VK_SHADER_STAGE_COMPUTE_BIT;
      break;
   default:
      UNREACHABLE("Unhandled bind point");
   }

   if (push_constants->need_upload) {
      radv_upload_push_constants(cmd_buffer, push_constants, &va);
   }

   if (internal_stages & VK_SHADER_STAGE_COMPUTE_BIT) {
      const struct radv_shader *compute_shader = bind_point == VK_PIPELINE_BIND_POINT_COMPUTE
                                                    ? cmd_buffer->state.shaders[MESA_SHADER_COMPUTE]
                                                    : cmd_buffer->state.rt_prolog;

      radv_emit_push_constants_per_stage(device, cs, compute_shader, (uint32_t *)cmd_buffer->push_constants, va);
   } else {
      struct radv_shader *prev_shader = NULL;

      radv_foreach_stage (stage, internal_stages & ~VK_SHADER_STAGE_TASK_BIT_EXT) {
         struct radv_shader *shader = radv_get_shader(cmd_buffer->state.shaders, stage);

         /* Avoid redundantly emitting the same values for merged stages. */
         if (shader && shader != prev_shader) {
            radv_emit_push_constants_per_stage(device, cs, shader, (uint32_t *)cmd_buffer->push_constants, va);

            prev_shader = shader;
         }
      }

      if (internal_stages & VK_SHADER_STAGE_TASK_BIT_EXT) {
         radv_emit_push_constants_per_stage(device, cmd_buffer->gang.cs, cmd_buffer->state.shaders[MESA_SHADER_TASK],
                                            (uint32_t *)cmd_buffer->push_constants, va);
      }
   }

   cmd_buffer->push_constant_stages &= ~stages;
}

static void
radv_upload_dynamic_descriptors(struct radv_cmd_buffer *cmd_buffer,
                                const struct radv_descriptor_state *descriptors_state, uint64_t *va)
{
   const uint32_t size = descriptors_state->dynamic_offset_count * 16;
   unsigned offset;
   void *ptr;

   if (!radv_cmd_buffer_upload_alloc(cmd_buffer, size, &offset, &ptr))
      return;

   memcpy(ptr, descriptors_state->dynamic_buffers, size);

   *va = radv_buffer_get_va(cmd_buffer->upload.upload_bo) + offset;
}

static void
radv_flush_dynamic_descriptors(struct radv_cmd_buffer *cmd_buffer, VkShaderStageFlags stages,
                               VkPipelineBindPoint bind_point)
{
   const struct radv_device *device = radv_cmd_buffer_device(cmd_buffer);
   struct radv_descriptor_state *descriptors_state = radv_get_descriptors_state(cmd_buffer, bind_point);
   struct radv_cmd_stream *cs = cmd_buffer->cs;
   uint64_t va = 0;

   radv_upload_dynamic_descriptors(cmd_buffer, descriptors_state, &va);

   ASSERTED unsigned cdw_max = radeon_check_space(device->ws, cs->b, MESA_VULKAN_SHADER_STAGES * 4);

   if (stages & VK_SHADER_STAGE_COMPUTE_BIT) {
      const struct radv_shader *compute_shader = bind_point == VK_PIPELINE_BIND_POINT_COMPUTE
                                                    ? cmd_buffer->state.shaders[MESA_SHADER_COMPUTE]
                                                    : cmd_buffer->state.rt_prolog;

      radv_emit_userdata_address(device, cs, compute_shader, AC_UD_DYNAMIC_DESCRIPTORS, va);
   } else {
      radv_foreach_stage (stage, stages & ~VK_SHADER_STAGE_TASK_BIT_EXT) {
         if (!cmd_buffer->state.shaders[stage])
            continue;

         radv_emit_userdata_address(device, cs, cmd_buffer->state.shaders[stage], AC_UD_DYNAMIC_DESCRIPTORS, va);
      }

      if (stages & VK_SHADER_STAGE_TASK_BIT_EXT) {
         radv_emit_userdata_address(device, cmd_buffer->gang.cs, cmd_buffer->state.shaders[MESA_SHADER_TASK],
                                    AC_UD_DYNAMIC_DESCRIPTORS, va);
      }
   }

   assert(cs->b->cdw <= cdw_max);
}

ALWAYS_INLINE void
radv_get_vbo_info(const struct radv_cmd_buffer *cmd_buffer, uint32_t idx, struct radv_vbo_info *vbo_info)
{
   const struct radv_dynamic_state *d = &cmd_buffer->state.dynamic;
   const uint32_t binding = d->vertex_input.bindings[idx];

   vbo_info->binding = binding;
   vbo_info->va = cmd_buffer->vertex_bindings[binding].addr;
   vbo_info->size = cmd_buffer->vertex_bindings[binding].size;

   vbo_info->stride = d->vk.vi_binding_strides[binding];

   vbo_info->attrib_offset = d->vertex_input.offsets[idx];
   vbo_info->attrib_index_offset = d->vertex_input.attrib_index_offset[idx];
   vbo_info->attrib_format_size = d->vertex_input.format_sizes[idx];
   vbo_info->non_trivial_format = d->vertex_input.non_trivial_format[idx];
}

ALWAYS_INLINE static void
radv_write_vertex_descriptor(const struct radv_cmd_buffer *cmd_buffer, const struct radv_shader *vs, const unsigned i,
                             const bool uses_dynamic_inputs, uint32_t *desc)
{
   struct radv_device *device = radv_cmd_buffer_device(cmd_buffer);
   const struct radv_physical_device *pdev = radv_device_physical(device);
   const struct radv_dynamic_state *d = &cmd_buffer->state.dynamic;
   enum amd_gfx_level chip = pdev->info.gfx_level;

   if (uses_dynamic_inputs && !(d->vertex_input.attribute_mask & BITFIELD_BIT(i))) {
      /* No vertex attribute description given: assume that the shader doesn't use this
       * location (vb_desc_usage_mask can be larger than attribute usage) and use a null
       * descriptor to avoid hangs (prologs load all attributes, even if there are holes).
       */
      memset(desc, 0, 4 * 4);
      return;
   }

   struct radv_vbo_info vbo_info;
   radv_get_vbo_info(cmd_buffer, i, &vbo_info);

   uint32_t rsrc_word3;

   if (uses_dynamic_inputs && vbo_info.non_trivial_format) {
      rsrc_word3 = vbo_info.non_trivial_format;
   } else {
      rsrc_word3 = S_008F0C_DST_SEL_X(V_008F0C_SQ_SEL_X) | S_008F0C_DST_SEL_Y(V_008F0C_SQ_SEL_Y) |
                   S_008F0C_DST_SEL_Z(V_008F0C_SQ_SEL_Z) | S_008F0C_DST_SEL_W(V_008F0C_SQ_SEL_W);

      if (pdev->info.gfx_level >= GFX10) {
         rsrc_word3 |= S_008F0C_FORMAT_GFX10(V_008F0C_GFX10_FORMAT_32_UINT);
      } else {
         rsrc_word3 |=
            S_008F0C_NUM_FORMAT(V_008F0C_BUF_NUM_FORMAT_UINT) | S_008F0C_DATA_FORMAT(V_008F0C_BUF_DATA_FORMAT_32);
      }
   }

   if (!vbo_info.va) {
      if (uses_dynamic_inputs) {
         /* Stride needs to be non-zero on GFX9, or else bounds checking is disabled. We need
          * to include the format/word3 so that the alpha channel is 1 for formats without an
          * alpha channel.
          */
         desc[0] = 0;
         desc[1] = S_008F04_STRIDE(16);
         desc[2] = 0;
         desc[3] = rsrc_word3;
      } else {
         memset(desc, 0, 4 * 4);
      }

      return;
   }

   const unsigned stride = vbo_info.stride;
   uint32_t num_records = vbo_info.size;

   if (vs->info.vs.use_per_attribute_vb_descs) {
      const uint32_t attrib_end = vbo_info.attrib_offset + vbo_info.attrib_format_size;

      if (num_records < attrib_end) {
         num_records = 0; /* not enough space for one vertex */
      } else if (stride == 0) {
         num_records = 1; /* only one vertex */
      } else {
         num_records = (num_records - attrib_end) / stride + 1;
         /* If attrib_offset>stride, then the compiler will increase the vertex index by
          * attrib_offset/stride and decrease the offset by attrib_offset%stride. This is
          * only allowed with static strides.
          */
         num_records += vbo_info.attrib_index_offset;
      }

      /* GFX10 uses OOB_SELECT_RAW if stride==0, so convert num_records from elements into
       * into bytes in that case. GFX8 always uses bytes.
       */
      if (num_records && (chip == GFX8 || (chip != GFX9 && !stride))) {
         num_records = (num_records - 1) * stride + attrib_end;
      } else if (!num_records) {
         /* On GFX9, it seems bounds checking is disabled if both
          * num_records and stride are zero. This doesn't seem necessary on GFX8, GFX10 and
          * GFX10.3 but it doesn't hurt.
          */
         if (uses_dynamic_inputs) {
            desc[0] = 0;
            desc[1] = S_008F04_STRIDE(16);
            desc[2] = 0;
            desc[3] = rsrc_word3;
         } else {
            memset(desc, 0, 16);
         }

         return;
      }
   } else {
      if (chip != GFX8 && stride)
         num_records = DIV_ROUND_UP(num_records, stride);
   }

   if (chip >= GFX10) {
      /* OOB_SELECT chooses the out-of-bounds check:
       * - 1: index >= NUM_RECORDS (Structured)
       * - 3: offset >= NUM_RECORDS (Raw)
       */
      int oob_select = stride ? V_008F0C_OOB_SELECT_STRUCTURED : V_008F0C_OOB_SELECT_RAW;
      rsrc_word3 |= S_008F0C_OOB_SELECT(oob_select) | S_008F0C_RESOURCE_LEVEL(chip < GFX11);
   }

   uint64_t va = vbo_info.va;
   if (uses_dynamic_inputs)
      va += vbo_info.attrib_offset;

   desc[0] = va;
   desc[1] = S_008F04_BASE_ADDRESS_HI(va >> 32) | S_008F04_STRIDE(stride);
   desc[2] = num_records;
   desc[3] = rsrc_word3;
}

ALWAYS_INLINE static void
radv_write_vertex_descriptors_dynamic(const struct radv_cmd_buffer *cmd_buffer, const struct radv_shader *vs,
                                      void *vb_ptr)
{
   unsigned desc_index = 0;
   for (unsigned i = 0; i < vs->info.vs.num_attributes; i++) {
      uint32_t *desc = &((uint32_t *)vb_ptr)[desc_index++ * 4];
      radv_write_vertex_descriptor(cmd_buffer, vs, i, true, desc);
   }
}

ALWAYS_INLINE static void
radv_write_vertex_descriptors(const struct radv_cmd_buffer *cmd_buffer, const struct radv_shader *vs, void *vb_ptr)
{
   unsigned desc_index = 0;
   u_foreach_bit (i, vs->info.vs.vb_desc_usage_mask) {
      uint32_t *desc = &((uint32_t *)vb_ptr)[desc_index++ * 4];
      radv_write_vertex_descriptor(cmd_buffer, vs, i, false, desc);
   }
}

ALWAYS_INLINE static void
radv_flush_vertex_descriptors(struct radv_cmd_buffer *cmd_buffer)
{
   struct radv_shader *vs = radv_get_shader(cmd_buffer->state.shaders, MESA_SHADER_VERTEX);
   struct radv_device *device = radv_cmd_buffer_device(cmd_buffer);
   bool uses_dynamic_inputs = vs->info.vs.dynamic_inputs;
   struct radv_cmd_stream *cs = cmd_buffer->cs;

   if (!vs)
      return;

   if (!vs->info.vs.vb_desc_usage_mask)
      return;

   unsigned vb_desc_alloc_size =
      (uses_dynamic_inputs ? vs->info.vs.num_attributes : util_bitcount(vs->info.vs.vb_desc_usage_mask)) * 16;
   unsigned vb_offset;
   void *vb_ptr;
   uint64_t va;

   /* allocate some descriptor state for vertex buffers */
   if (!radv_cmd_buffer_upload_alloc(cmd_buffer, vb_desc_alloc_size, &vb_offset, &vb_ptr))
      return;

   if (uses_dynamic_inputs)
      radv_write_vertex_descriptors_dynamic(cmd_buffer, vs, vb_ptr);
   else
      radv_write_vertex_descriptors(cmd_buffer, vs, vb_ptr);

   va = radv_buffer_get_va(cmd_buffer->upload.upload_bo);
   va += vb_offset;

   radv_emit_userdata_address(device, cs, vs, AC_UD_VS_VERTEX_BUFFERS, va);

   cmd_buffer->state.vb_va = va;
   cmd_buffer->state.vb_size = vb_desc_alloc_size;
   cmd_buffer->state.prefetch_L2_mask |= RADV_PREFETCH_VBO_DESCRIPTORS;

   if (radv_device_fault_detection_enabled(device))
      radv_save_vertex_descriptors(cmd_buffer, (uintptr_t)vb_ptr);
}

static void
radv_emit_streamout_buffers(struct radv_cmd_buffer *cmd_buffer, uint64_t va)
{
   const struct radv_shader *last_vgt_shader = cmd_buffer->state.last_vgt_shader;
   uint32_t streamout_buffers_offset = radv_get_user_sgpr_loc(last_vgt_shader, AC_UD_STREAMOUT_BUFFERS);
   struct radv_device *device = radv_cmd_buffer_device(cmd_buffer);
   const struct radv_physical_device *pdev = radv_device_physical(device);

   if (!streamout_buffers_offset)
      return;

   radeon_begin(cmd_buffer->cs);
   if (pdev->info.gfx_level >= GFX12) {
      gfx12_push_32bit_pointer(streamout_buffers_offset, va, &pdev->info);
   } else {
      radeon_emit_32bit_pointer(streamout_buffers_offset, va, &pdev->info);

      if (cmd_buffer->state.gs_copy_shader) {
         streamout_buffers_offset = radv_get_user_sgpr_loc(cmd_buffer->state.gs_copy_shader, AC_UD_STREAMOUT_BUFFERS);
         if (streamout_buffers_offset)
            radeon_emit_32bit_pointer(streamout_buffers_offset, va, &pdev->info);
      }
   }
   radeon_end();
}

static void
radv_emit_streamout_state(struct radv_cmd_buffer *cmd_buffer)
{
   const struct radv_device *device = radv_cmd_buffer_device(cmd_buffer);
   const struct radv_physical_device *pdev = radv_device_physical(device);
   const struct radv_shader *last_vgt_shader = cmd_buffer->state.last_vgt_shader;
   const uint32_t streamout_state_offset = radv_get_user_sgpr_loc(last_vgt_shader, AC_UD_STREAMOUT_STATE);
   struct radv_streamout_state *so = &cmd_buffer->state.streamout;

   assert(pdev->info.gfx_level >= GFX12);

   if (!streamout_state_offset)
      return;

   radeon_begin(cmd_buffer->cs);
   gfx12_push_32bit_pointer(streamout_state_offset, so->state_va, &pdev->info);
   radeon_end();
}

static void
radv_flush_streamout_descriptors(struct radv_cmd_buffer *cmd_buffer)
{
   struct radv_device *device = radv_cmd_buffer_device(cmd_buffer);
   const struct radv_physical_device *pdev = radv_device_physical(device);
   struct radv_streamout_binding *sb = cmd_buffer->streamout_bindings;
   struct radv_streamout_state *so = &cmd_buffer->state.streamout;
   unsigned so_offset;
   uint64_t desc_va;
   void *so_ptr;

   /* Allocate some descriptor state for streamout buffers. */
   if (!radv_cmd_buffer_upload_alloc(cmd_buffer, MAX_SO_BUFFERS * 16, &so_offset, &so_ptr))
      return;

   for (uint32_t i = 0; i < MAX_SO_BUFFERS; i++) {
      uint32_t *desc = &((uint32_t *)so_ptr)[i * 4];
      uint32_t size = 0;
      uint64_t va = 0;

      if (so->enabled_mask & (1 << i)) {
         va = sb[i].va;

         /* Set the descriptor.
          *
          * On GFX8, the format must be non-INVALID, otherwise
          * the buffer will be considered not bound and store
          * instructions will be no-ops.
          */
         size = 0xffffffff;

         if (pdev->use_ngg_streamout) {
            /* With NGG streamout, the buffer size is used to determine the max emit per buffer
             * and also acts as a disable bit when it's 0.
             */
            size = radv_is_streamout_enabled(cmd_buffer) ? sb[i].size : 0;
         }
      }

      ac_build_raw_buffer_descriptor(pdev->info.gfx_level, va, size, desc);
   }

   desc_va = radv_buffer_get_va(cmd_buffer->upload.upload_bo);
   desc_va += so_offset;

   radv_emit_streamout_buffers(cmd_buffer, desc_va);

   if (pdev->info.gfx_level >= GFX12)
      radv_emit_streamout_state(cmd_buffer);
}

ALWAYS_INLINE static void
radv_upload_graphics_shader_descriptors(struct radv_cmd_buffer *cmd_buffer)
{
   struct radv_descriptor_state *descriptors_state =
      radv_get_descriptors_state(cmd_buffer, VK_PIPELINE_BIND_POINT_GRAPHICS);
   const VkShaderStageFlags stages = VK_SHADER_STAGE_ALL_GRAPHICS;

   if (cmd_buffer->state.dirty & RADV_CMD_DIRTY_VERTEX_BUFFER) {
      radv_flush_vertex_descriptors(cmd_buffer);
      cmd_buffer->state.dirty &= ~RADV_CMD_DIRTY_VERTEX_BUFFER;
   }

   if (cmd_buffer->state.dirty & RADV_CMD_DIRTY_STREAMOUT_BUFFER) {
      radv_flush_streamout_descriptors(cmd_buffer);
      cmd_buffer->state.dirty &= ~RADV_CMD_DIRTY_STREAMOUT_BUFFER;
   }

   if (descriptors_state->dirty) {
      radv_flush_descriptors(cmd_buffer, stages, VK_PIPELINE_BIND_POINT_GRAPHICS);
      descriptors_state->dirty = 0;
   }

   if (descriptors_state->dirty_dynamic && descriptors_state->dynamic_offset_count) {
      radv_flush_dynamic_descriptors(cmd_buffer, stages, VK_PIPELINE_BIND_POINT_GRAPHICS);
      descriptors_state->dirty_dynamic = false;
   }

   const VkShaderStageFlags pc_stages = radv_must_flush_constants(cmd_buffer, stages, VK_PIPELINE_BIND_POINT_GRAPHICS);
   if (pc_stages)
      radv_flush_constants(cmd_buffer, pc_stages, VK_PIPELINE_BIND_POINT_GRAPHICS);
}

struct radv_prim_vertex_count {
   uint8_t min;
   uint8_t incr;
};

static inline unsigned
radv_prims_for_vertices(struct radv_prim_vertex_count *info, unsigned num)
{
   if (num == 0)
      return 0;

   if (info->incr == 0)
      return 0;

   if (num < info->min)
      return 0;

   return 1 + ((num - info->min) / info->incr);
}

static const struct radv_prim_vertex_count prim_size_table[] = {
   [V_008958_DI_PT_NONE] = {0, 0},          [V_008958_DI_PT_POINTLIST] = {1, 1},
   [V_008958_DI_PT_LINELIST] = {2, 2},      [V_008958_DI_PT_LINESTRIP] = {2, 1},
   [V_008958_DI_PT_TRILIST] = {3, 3},       [V_008958_DI_PT_TRIFAN] = {3, 1},
   [V_008958_DI_PT_TRISTRIP] = {3, 1},      [V_008958_DI_PT_LINELIST_ADJ] = {4, 4},
   [V_008958_DI_PT_LINESTRIP_ADJ] = {4, 1}, [V_008958_DI_PT_TRILIST_ADJ] = {6, 6},
   [V_008958_DI_PT_TRISTRIP_ADJ] = {6, 2},  [V_008958_DI_PT_RECTLIST] = {3, 3},
   [V_008958_DI_PT_LINELOOP] = {2, 1},      [V_008958_DI_PT_POLYGON] = {3, 1},
   [V_008958_DI_PT_2D_TRI_STRIP] = {0, 0},
};

static uint32_t
radv_get_ia_multi_vgt_param(struct radv_cmd_buffer *cmd_buffer, bool instanced_draw, bool indirect_draw,
                            bool count_from_stream_output, uint32_t draw_vertex_count, unsigned topology,
                            bool prim_restart_enable, unsigned patch_control_points, unsigned num_tess_patches)
{
   struct radv_device *device = radv_cmd_buffer_device(cmd_buffer);
   const struct radv_physical_device *pdev = radv_device_physical(device);
   const struct radeon_info *gpu_info = &pdev->info;
   const unsigned max_primgroup_in_wave = 2;

   /* WD (work distributor) can launch work on IA0 and IA1 (input assemblers).
    * By default, it distributes each primitive group to a different IA.
    * When set, it only switches IA at the end of a draw packet,
    * reducing primitive throughput by 50%. WD_SWITCH_ON_EOP(0) is always preferred.
    */
   bool wd_switch_on_eop = false;

   /* Each IA (input assembler) can launch work two SE (shader engine).
    * By default, they distribute each primitive group to a different SE.
    * When set, they only switch SE at the end of a draw packet,
    * reducing primitive throughput by another 50%. SWITCH_ON_EOP(0) is always preferred.
    */
   bool ia_switch_on_eop = false;

   bool ia_switch_on_eoi = false;
   bool partial_vs_wave = false;
   bool partial_es_wave = cmd_buffer->state.ia_multi_vgt_param.partial_es_wave;
   bool multi_instances_smaller_than_primgroup;
   struct radv_prim_vertex_count prim_vertex_count = prim_size_table[topology];
   unsigned primgroup_size;

   if (radv_cmdbuf_has_stage(cmd_buffer, MESA_SHADER_TESS_CTRL)) {
      primgroup_size = num_tess_patches;
   } else if (radv_cmdbuf_has_stage(cmd_buffer, MESA_SHADER_GEOMETRY)) {
      primgroup_size = 64;
   } else {
      primgroup_size = 128; /* recommended without a GS */
   }

   /* GS requirement. */
   if (radv_cmdbuf_has_stage(cmd_buffer, MESA_SHADER_GEOMETRY) && gpu_info->gfx_level <= GFX8) {
      unsigned gs_table_depth = pdev->gs_table_depth;
      if (SI_GS_PER_ES / primgroup_size >= gs_table_depth - 3)
         partial_es_wave = true;
   }

   if (radv_cmdbuf_has_stage(cmd_buffer, MESA_SHADER_TESS_CTRL)) {
      if (topology == V_008958_DI_PT_PATCH) {
         prim_vertex_count.min = patch_control_points;
         prim_vertex_count.incr = 1;
      }
   }

   multi_instances_smaller_than_primgroup = indirect_draw;
   if (!multi_instances_smaller_than_primgroup && instanced_draw) {
      uint32_t num_prims = radv_prims_for_vertices(&prim_vertex_count, draw_vertex_count);
      if (num_prims < primgroup_size)
         multi_instances_smaller_than_primgroup = true;
   }

   ia_switch_on_eoi = cmd_buffer->state.ia_multi_vgt_param.ia_switch_on_eoi;
   partial_vs_wave = cmd_buffer->state.ia_multi_vgt_param.partial_vs_wave;

   if (gpu_info->gfx_level >= GFX7) {
      /* WD_SWITCH_ON_EOP has no effect on GPUs with less than
       * 4 shader engines. Set 1 to pass the assertion below.
       * The other cases are hardware requirements. */
      if (gpu_info->max_se < 4 || topology == V_008958_DI_PT_POLYGON || topology == V_008958_DI_PT_LINELOOP ||
          topology == V_008958_DI_PT_TRIFAN || topology == V_008958_DI_PT_TRISTRIP_ADJ ||
          (prim_restart_enable && (gpu_info->family < CHIP_POLARIS10 ||
                                   (topology != V_008958_DI_PT_POINTLIST && topology != V_008958_DI_PT_LINESTRIP))))
         wd_switch_on_eop = true;

      /* Hawaii hangs if instancing is enabled and WD_SWITCH_ON_EOP is 0.
       * We don't know that for indirect drawing, so treat it as
       * always problematic. */
      if (gpu_info->family == CHIP_HAWAII) {
         if (instanced_draw || indirect_draw)
            wd_switch_on_eop = true;

         /* Mitigate a GPU hang in Dota 2 and Rise of the Tomb Raider.
          * This workaround is not documented by AMD and may not be correct.
          * Further investigation is necessary to understand it better.
          */
         if (topology == V_008958_DI_PT_TRILIST) {
            ia_switch_on_eop = true;
            wd_switch_on_eop = true;
         }
      }

      /* Performance recommendation for 4 SE Gfx7-8 parts if
       * instances are smaller than a primgroup.
       * Assume indirect draws always use small instances.
       * This is needed for good VS wave utilization.
       */
      if (gpu_info->gfx_level <= GFX8 && gpu_info->max_se == 4 && multi_instances_smaller_than_primgroup)
         wd_switch_on_eop = true;

      /* Hardware requirement when drawing primitives from a stream
       * output buffer.
       */
      if (count_from_stream_output)
         wd_switch_on_eop = true;

      /* Required on GFX7 and later. */
      if (gpu_info->max_se > 2 && !wd_switch_on_eop)
         ia_switch_on_eoi = true;

      /* Required by Hawaii and, for some special cases, by GFX8. */
      if (ia_switch_on_eoi &&
          (gpu_info->family == CHIP_HAWAII ||
           (gpu_info->gfx_level == GFX8 &&
            /* max primgroup in wave is always 2 - leave this for documentation */
            (radv_cmdbuf_has_stage(cmd_buffer, MESA_SHADER_GEOMETRY) || max_primgroup_in_wave != 2))))
         partial_vs_wave = true;

      /* Instancing bug on Bonaire. */
      if (gpu_info->family == CHIP_BONAIRE && ia_switch_on_eoi && (instanced_draw || indirect_draw))
         partial_vs_wave = true;

      /* If the WD switch is false, the IA switch must be false too. */
      assert(wd_switch_on_eop || !ia_switch_on_eop);
   }
   /* If SWITCH_ON_EOI is set, PARTIAL_ES_WAVE must be set too. */
   if (gpu_info->gfx_level <= GFX8 && ia_switch_on_eoi)
      partial_es_wave = true;

   if (radv_cmdbuf_has_stage(cmd_buffer, MESA_SHADER_GEOMETRY)) {
      /* GS hw bug with single-primitive instances and SWITCH_ON_EOI.
       * The hw doc says all multi-SE chips are affected, but amdgpu-pro Vulkan
       * only applies it to Hawaii. Do what amdgpu-pro Vulkan does.
       */
      if (gpu_info->family == CHIP_HAWAII && ia_switch_on_eoi) {
         bool set_vgt_flush = indirect_draw;
         if (!set_vgt_flush && instanced_draw) {
            uint32_t num_prims = radv_prims_for_vertices(&prim_vertex_count, draw_vertex_count);
            if (num_prims <= 1)
               set_vgt_flush = true;
         }
         if (set_vgt_flush)
            cmd_buffer->state.flush_bits |= RADV_CMD_FLAG_VGT_FLUSH;
      }
   }

   /* Workaround for a VGT hang when strip primitive types are used with
    * primitive restart.
    */
   if (prim_restart_enable && (topology == V_008958_DI_PT_LINESTRIP || topology == V_008958_DI_PT_TRISTRIP ||
                               topology == V_008958_DI_PT_LINESTRIP_ADJ || topology == V_008958_DI_PT_TRISTRIP_ADJ)) {
      partial_vs_wave = true;
   }

   return cmd_buffer->state.ia_multi_vgt_param.base | S_028AA8_PRIMGROUP_SIZE(primgroup_size - 1) |
          S_028AA8_SWITCH_ON_EOP(ia_switch_on_eop) | S_028AA8_SWITCH_ON_EOI(ia_switch_on_eoi) |
          S_028AA8_PARTIAL_VS_WAVE_ON(partial_vs_wave) | S_028AA8_PARTIAL_ES_WAVE_ON(partial_es_wave) |
          S_028AA8_WD_SWITCH_ON_EOP(gpu_info->gfx_level >= GFX7 ? wd_switch_on_eop : 0);
}

static void
radv_emit_ia_multi_vgt_param(struct radv_cmd_buffer *cmd_buffer, bool instanced_draw, bool indirect_draw,
                             bool count_from_stream_output, uint32_t draw_vertex_count)
{
   struct radv_device *device = radv_cmd_buffer_device(cmd_buffer);
   const struct radv_physical_device *pdev = radv_device_physical(device);
   const struct radeon_info *gpu_info = &pdev->info;
   struct radv_cmd_state *state = &cmd_buffer->state;
   const unsigned patch_control_points = state->dynamic.vk.ts.patch_control_points;
   const unsigned topology = state->dynamic.vk.ia.primitive_topology;
   const bool prim_restart_enable = state->dynamic.vk.ia.primitive_restart_enable;
   struct radv_cmd_stream *cs = cmd_buffer->cs;
   unsigned ia_multi_vgt_param;

   ia_multi_vgt_param = radv_get_ia_multi_vgt_param(cmd_buffer, instanced_draw, indirect_draw, count_from_stream_output,
                                                    draw_vertex_count, topology, prim_restart_enable,
                                                    patch_control_points, state->tess_num_patches);

   if (state->last_ia_multi_vgt_param != ia_multi_vgt_param) {
      radeon_begin(cs);

      if (gpu_info->gfx_level == GFX9) {
         radeon_set_uconfig_reg_idx(&pdev->info, R_030960_IA_MULTI_VGT_PARAM, 4, ia_multi_vgt_param);
      } else if (gpu_info->gfx_level >= GFX7) {
         radeon_set_context_reg_idx(R_028AA8_IA_MULTI_VGT_PARAM, 1, ia_multi_vgt_param);
      } else {
         radeon_set_context_reg(R_028AA8_IA_MULTI_VGT_PARAM, ia_multi_vgt_param);
      }

      radeon_end();

      state->last_ia_multi_vgt_param = ia_multi_vgt_param;
   }
}

static void
gfx10_emit_ge_cntl(struct radv_cmd_buffer *cmd_buffer)
{
   const struct radv_shader *last_vgt_shader = cmd_buffer->state.last_vgt_shader;
   struct radv_cmd_state *state = &cmd_buffer->state;
   bool break_wave_at_eoi = false;
   unsigned primgroup_size;
   unsigned ge_cntl;

   if (last_vgt_shader->info.is_ngg)
      return;

   if (radv_cmdbuf_has_stage(cmd_buffer, MESA_SHADER_TESS_CTRL)) {
      const struct radv_shader *tes = radv_get_shader(cmd_buffer->state.shaders, MESA_SHADER_TESS_EVAL);

      primgroup_size = state->tess_num_patches;

      if (cmd_buffer->state.shaders[MESA_SHADER_TESS_CTRL]->info.uses_prim_id || tes->info.uses_prim_id ||
          (tes->info.merged_shader_compiled_separately &&
           cmd_buffer->state.shaders[MESA_SHADER_GEOMETRY]->info.uses_prim_id)) {
         break_wave_at_eoi = true;
      }
   } else if (radv_cmdbuf_has_stage(cmd_buffer, MESA_SHADER_GEOMETRY)) {
      const struct radv_legacy_gs_info *gs_state = &cmd_buffer->state.shaders[MESA_SHADER_GEOMETRY]->info.gs_ring_info;
      primgroup_size = gs_state->gs_prims_per_subgroup;
   } else {
      primgroup_size = 128; /* recommended without a GS and tess */
   }

   ge_cntl = S_03096C_PRIM_GRP_SIZE_GFX10(primgroup_size) | S_03096C_VERT_GRP_SIZE(256) | /* disable vertex grouping */
             S_03096C_PACKET_TO_ONE_PA(0) /* this should only be set if LINE_STIPPLE_TEX_ENA == 1 */ |
             S_03096C_BREAK_WAVE_AT_EOI(break_wave_at_eoi);

   if (state->last_ge_cntl != ge_cntl) {
      radeon_begin(cmd_buffer->cs);
      radeon_set_uconfig_reg(R_03096C_GE_CNTL, ge_cntl);
      radeon_end();

      state->last_ge_cntl = ge_cntl;
   }
}

static void
radv_emit_primitive_restart(struct radv_cmd_buffer *cmd_buffer, bool enable)
{
   struct radv_device *device = radv_cmd_buffer_device(cmd_buffer);
   const struct radv_physical_device *pdev = radv_device_physical(device);
   const enum amd_gfx_level gfx_level = pdev->info.gfx_level;
   struct radv_cmd_stream *cs = cmd_buffer->cs;

   radeon_begin(cs);

   if (pdev->info.has_prim_restart_sync_bug) {
      radeon_event_write(V_028A90_SQ_NON_EVENT);
   }

   if (gfx_level >= GFX11) {
      radeon_set_uconfig_reg(R_03092C_GE_MULTI_PRIM_IB_RESET_EN, S_03092C_RESET_EN(enable) |
                                                                    /* This disables primitive restart for non-indexed
                                                                     * draws. By keeping this set, we don't have to
                                                                     * unset RESET_EN for non-indexed draws. */
                                                                    S_03092C_DISABLE_FOR_AUTO_INDEX(1));
   } else if (gfx_level >= GFX9) {
      radeon_set_uconfig_reg(R_03092C_VGT_MULTI_PRIM_IB_RESET_EN, enable);
   } else {
      radeon_set_context_reg(R_028A94_VGT_MULTI_PRIM_IB_RESET_EN, enable);

      /* GFX6-7: All 32 bits are compared.
       * GFX8: Only index type bits are compared.
       * GFX9+: Default is same as GFX8, MATCH_ALL_BITS=1 selects GFX6-7 behavior
       */
      if (enable && gfx_level <= GFX7) {
         const uint32_t primitive_reset_index = radv_get_primitive_reset_index(cmd_buffer);

         radeon_opt_set_context_reg(R_02840C_VGT_MULTI_PRIM_IB_RESET_INDX, RADV_TRACKED_VGT_MULTI_PRIM_IB_RESET_INDX,
                                    primitive_reset_index);
      }
   }

   radeon_end();
}

static void
radv_emit_draw_registers(struct radv_cmd_buffer *cmd_buffer, const struct radv_draw_info *draw_info)
{
   struct radv_device *device = radv_cmd_buffer_device(cmd_buffer);
   const struct radv_physical_device *pdev = radv_device_physical(device);
   const struct radv_dynamic_state *d = &cmd_buffer->state.dynamic;
   const bool primitive_restart_en =
      (draw_info->indexed || pdev->info.gfx_level >= GFX11) && d->vk.ia.primitive_restart_enable;
   const uint32_t primitive_reset_index = radv_get_primitive_reset_index(cmd_buffer);
   const struct radeon_info *gpu_info = &pdev->info;
   struct radv_cmd_state *state = &cmd_buffer->state;
   struct radv_cmd_stream *cs = cmd_buffer->cs;
   uint32_t topology = state->dynamic.vk.ia.primitive_topology;
   bool disable_instance_packing = false;

   /* Draw state. */
   if (gpu_info->gfx_level >= GFX10) {
      gfx10_emit_ge_cntl(cmd_buffer);
   } else {
      radv_emit_ia_multi_vgt_param(cmd_buffer, draw_info->instance_count > 1, !!draw_info->indirect_va,
                                   !!draw_info->strmout_va, draw_info->indirect_va ? 0 : draw_info->count);
   }

   /* RDNA2 is affected by a hardware bug when instance packing is enabled for adjacent primitive
    * topologies and instance_count > 1, pipeline stats generated by GE are incorrect. It needs to
    * be applied for indexed and non-indexed draws.
    */
   if (gpu_info->gfx_level == GFX10_3 && state->active_pipeline_queries > 0 &&
       (draw_info->instance_count > 1 || draw_info->indirect_va) &&
       (topology == V_008958_DI_PT_LINELIST_ADJ || topology == V_008958_DI_PT_LINESTRIP_ADJ ||
        topology == V_008958_DI_PT_TRILIST_ADJ || topology == V_008958_DI_PT_TRISTRIP_ADJ)) {
      disable_instance_packing = true;
   }

   if ((draw_info->indexed && state->index_type != state->last_index_type) ||
       (gpu_info->gfx_level == GFX10_3 &&
        (state->last_index_type == -1 ||
         disable_instance_packing != G_028A7C_DISABLE_INSTANCE_PACKING(state->last_index_type)))) {
      uint32_t index_type = state->index_type | S_028A7C_DISABLE_INSTANCE_PACKING(disable_instance_packing);

      radeon_begin(cs);

      if (pdev->info.gfx_level >= GFX9) {
         radeon_set_uconfig_reg_idx(&pdev->info, R_03090C_VGT_INDEX_TYPE, 2, index_type);
      } else {
         radeon_emit(PKT3(PKT3_INDEX_TYPE, 0, 0));
         radeon_emit(index_type);
      }

      radeon_end();

      state->last_index_type = index_type;
   }

   if (primitive_restart_en != state->last_primitive_restart_en ||
       (pdev->info.gfx_level <= GFX7 && primitive_reset_index != state->last_primitive_reset_index)) {
      radv_emit_primitive_restart(cmd_buffer, primitive_restart_en);
      state->last_primitive_restart_en = primitive_restart_en;
      state->last_primitive_reset_index = primitive_reset_index;
   }
}

static void
radv_stage_flush(struct radv_cmd_buffer *cmd_buffer, VkPipelineStageFlags2 src_stage_mask)
{
   /* For simplicity, if the barrier wants to wait for the task shader,
    * just make it wait for the mesh shader too.
    */
   if (src_stage_mask & VK_PIPELINE_STAGE_2_TASK_SHADER_BIT_EXT)
      src_stage_mask |= VK_PIPELINE_STAGE_2_MESH_SHADER_BIT_EXT;

   if (src_stage_mask & (VK_PIPELINE_STAGE_2_COPY_BIT | VK_PIPELINE_STAGE_2_RESOLVE_BIT | VK_PIPELINE_STAGE_2_BLIT_BIT |
                         VK_PIPELINE_STAGE_2_CLEAR_BIT)) {
      /* Be conservative for now. */
      src_stage_mask |= VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT;
   }

   if (src_stage_mask &
       (VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT |
        VK_PIPELINE_STAGE_2_ACCELERATION_STRUCTURE_BUILD_BIT_KHR |
        VK_PIPELINE_STAGE_2_ACCELERATION_STRUCTURE_COPY_BIT_KHR | VK_PIPELINE_STAGE_2_RAY_TRACING_SHADER_BIT_KHR |
        VK_PIPELINE_STAGE_2_COMMAND_PREPROCESS_BIT_EXT | VK_PIPELINE_STAGE_2_BOTTOM_OF_PIPE_BIT |
        VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT)) {
      cmd_buffer->state.flush_bits |= RADV_CMD_FLAG_CS_PARTIAL_FLUSH;
   }

   if (src_stage_mask & (VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT |
                         VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT |
                         VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT | VK_PIPELINE_STAGE_2_BOTTOM_OF_PIPE_BIT |
                         VK_PIPELINE_STAGE_2_ALL_GRAPHICS_BIT | VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT)) {
      cmd_buffer->state.flush_bits |= RADV_CMD_FLAG_PS_PARTIAL_FLUSH;
   } else if (src_stage_mask &
              (VK_PIPELINE_STAGE_2_DRAW_INDIRECT_BIT | VK_PIPELINE_STAGE_2_VERTEX_INPUT_BIT |
               VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_2_TESSELLATION_CONTROL_SHADER_BIT |
               VK_PIPELINE_STAGE_2_TESSELLATION_EVALUATION_SHADER_BIT | VK_PIPELINE_STAGE_2_GEOMETRY_SHADER_BIT |
               VK_PIPELINE_STAGE_2_MESH_SHADER_BIT_EXT | VK_PIPELINE_STAGE_2_TRANSFORM_FEEDBACK_BIT_EXT |
               VK_PIPELINE_STAGE_2_PRE_RASTERIZATION_SHADERS_BIT)) {
      cmd_buffer->state.flush_bits |= RADV_CMD_FLAG_VS_PARTIAL_FLUSH;
   }
}

static bool
can_skip_buffer_l2_flushes(struct radv_device *device)
{
   const struct radv_physical_device *pdev = radv_device_physical(device);
   return pdev->info.gfx_level == GFX9 || (pdev->info.gfx_level >= GFX10 && !pdev->info.tcc_rb_non_coherent);
}

/*
 * In vulkan barriers have two kinds of operations:
 *
 * - visibility (implemented with radv_src_access_flush)
 * - availability (implemented with radv_dst_access_flush)
 *
 * for a memory operation to observe the result of a previous memory operation
 * one needs to do a visibility operation from the source memory and then an
 * availability operation to the target memory.
 *
 * The complication is the availability and visibility operations do not need to
 * be in the same barrier.
 *
 * The cleanest way to implement this is to define the visibility operation to
 * bring the caches to a "state of rest", which none of the caches below that
 * level dirty.
 *
 * For GFX8 and earlier this would be VRAM/GTT with none of the caches dirty.
 *
 * For GFX9+ we can define the state at rest to be L2 instead of VRAM for all
 * buffers and for images marked as coherent, and VRAM/GTT for non-coherent
 * images. However, given the existence of memory barriers which do not specify
 * the image/buffer it often devolves to just VRAM/GTT anyway.
 *
 * To help reducing the invalidations for GPUs that have L2 coherency between the
 * RB and the shader caches, we always invalidate L2 on the src side, as we can
 * use our knowledge of past usage to optimize flushes away.
 */

enum radv_cmd_flush_bits
radv_src_access_flush(struct radv_cmd_buffer *cmd_buffer, VkPipelineStageFlags2 src_stages, VkAccessFlags2 src_flags,
                      VkAccessFlags3KHR src3_flags, const struct radv_image *image,
                      const VkImageSubresourceRange *range)
{
   const struct radv_device *device = radv_cmd_buffer_device(cmd_buffer);

   src_flags = vk_expand_src_access_flags2(src_stages, src_flags);

   bool has_CB_meta = true, has_DB_meta = true;
   bool image_is_coherent = image ? radv_image_is_l2_coherent(device, image, range) : false;
   enum radv_cmd_flush_bits flush_bits = 0;

   if (image) {
      if (!radv_image_has_CB_metadata(image))
         has_CB_meta = false;
      if (!radv_htile_enabled(image, range ? range->baseMipLevel : 0))
         has_DB_meta = false;
   }

   if (src_flags & VK_ACCESS_2_COMMAND_PREPROCESS_WRITE_BIT_EXT)
      flush_bits |= RADV_CMD_FLAG_INV_L2;

   if (src_flags & (VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT | VK_ACCESS_2_ACCELERATION_STRUCTURE_WRITE_BIT_KHR)) {
      /* since the STORAGE bit isn't set we know that this is a meta operation.
       * on the dst flush side we skip CB/DB flushes without the STORAGE bit, so
       * set it here. */
      if (image && !(image->vk.usage & VK_IMAGE_USAGE_STORAGE_BIT)) {
         if (vk_format_is_depth_or_stencil(image->vk.format)) {
            flush_bits |= RADV_CMD_FLAG_FLUSH_AND_INV_DB;
         } else {
            flush_bits |= RADV_CMD_FLAG_FLUSH_AND_INV_CB;
         }
      }

      if (!image_is_coherent)
         flush_bits |= RADV_CMD_FLAG_INV_L2;
   }

   if (src_flags &
       (VK_ACCESS_2_TRANSFORM_FEEDBACK_WRITE_BIT_EXT | VK_ACCESS_2_TRANSFORM_FEEDBACK_COUNTER_WRITE_BIT_EXT)) {
      if (!image_is_coherent)
         flush_bits |= RADV_CMD_FLAG_WB_L2;
   }

   if (src_flags & VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT) {
      flush_bits |= RADV_CMD_FLAG_FLUSH_AND_INV_CB;
      if (!image_is_coherent)
         flush_bits |= RADV_CMD_FLAG_INV_L2;
      if (has_CB_meta)
         flush_bits |= RADV_CMD_FLAG_FLUSH_AND_INV_CB_META;
   }

   if (src_flags & VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT) {
      flush_bits |= RADV_CMD_FLAG_FLUSH_AND_INV_DB;
      if (!image_is_coherent)
         flush_bits |= RADV_CMD_FLAG_INV_L2;
      if (has_DB_meta)
         flush_bits |= RADV_CMD_FLAG_FLUSH_AND_INV_DB_META;
   }

   if (src_flags & VK_ACCESS_2_TRANSFER_WRITE_BIT) {
      flush_bits |= RADV_CMD_FLAG_FLUSH_AND_INV_CB | RADV_CMD_FLAG_FLUSH_AND_INV_DB;

      if (!image_is_coherent)
         flush_bits |= RADV_CMD_FLAG_INV_L2;
      if (has_CB_meta)
         flush_bits |= RADV_CMD_FLAG_FLUSH_AND_INV_CB_META;
      if (has_DB_meta)
         flush_bits |= RADV_CMD_FLAG_FLUSH_AND_INV_DB_META;
   }

   return flush_bits;
}

enum radv_cmd_flush_bits
radv_dst_access_flush(struct radv_cmd_buffer *cmd_buffer, VkPipelineStageFlags2 dst_stages, VkAccessFlags2 dst_flags,
                      VkAccessFlags3KHR dst3_flags, const struct radv_image *image,
                      const VkImageSubresourceRange *range)
{
   struct radv_device *device = radv_cmd_buffer_device(cmd_buffer);
   const struct radv_physical_device *pdev = radv_device_physical(device);
   bool has_CB_meta = true, has_DB_meta = true;
   enum radv_cmd_flush_bits flush_bits = 0;
   bool flush_CB = true, flush_DB = true;
   bool image_is_coherent = image ? radv_image_is_l2_coherent(device, image, range) : false;
   bool flush_L2_metadata = false;

   dst_flags = vk_expand_dst_access_flags2(dst_stages, dst_flags);

   if (image) {
      if (!(image->vk.usage & VK_IMAGE_USAGE_STORAGE_BIT)) {
         flush_CB = false;
         flush_DB = false;
      }

      if (!radv_image_has_CB_metadata(image))
         has_CB_meta = false;
      if (!radv_htile_enabled(image, range ? range->baseMipLevel : 0))
         has_DB_meta = false;
   }

   flush_L2_metadata = (has_CB_meta || has_DB_meta) && pdev->info.gfx_level < GFX12;

   /* All the L2 invalidations below are not the CB/DB. So if there are no incoherent images
    * in the L2 cache in CB/DB mode then they are already usable from all the other L2 clients. */
   image_is_coherent |= can_skip_buffer_l2_flushes(device) && !cmd_buffer->state.rb_noncoherent_dirty;

   if (dst_flags & (VK_ACCESS_2_INDIRECT_COMMAND_READ_BIT | VK_ACCESS_2_CONDITIONAL_RENDERING_READ_BIT_EXT)) {
      /* SMEM loads are used to read compute dispatch size in shaders */
      if ((dst_flags & VK_ACCESS_2_INDIRECT_COMMAND_READ_BIT) && !device->load_grid_size_from_user_sgpr) {
         flush_bits |= RADV_CMD_FLAG_INV_SCACHE;
      }

      /* Ensure the DGC meta shader can read the commands. */
      if (device->vk.enabled_features.deviceGeneratedCommands) {
         flush_bits |= RADV_CMD_FLAG_INV_SCACHE | RADV_CMD_FLAG_INV_VCACHE;
         if (pdev->info.gfx_level < GFX9)
            flush_bits |= RADV_CMD_FLAG_INV_L2;
      }
   }

   if (dst_flags & VK_ACCESS_2_UNIFORM_READ_BIT)
      flush_bits |= RADV_CMD_FLAG_INV_VCACHE | RADV_CMD_FLAG_INV_SCACHE;

   if (dst_flags & (VK_ACCESS_2_VERTEX_ATTRIBUTE_READ_BIT | VK_ACCESS_2_INPUT_ATTACHMENT_READ_BIT |
                    VK_ACCESS_2_TRANSFER_READ_BIT)) {
      flush_bits |= RADV_CMD_FLAG_INV_VCACHE;

      if (flush_L2_metadata)
         flush_bits |= RADV_CMD_FLAG_INV_L2_METADATA;
      if (!image_is_coherent)
         flush_bits |= RADV_CMD_FLAG_INV_L2;
   }

   if (dst_flags & VK_ACCESS_2_DESCRIPTOR_BUFFER_READ_BIT_EXT)
      flush_bits |= RADV_CMD_FLAG_INV_SCACHE;

   if (dst_flags & (VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_BINDING_TABLE_READ_BIT_KHR |
                    VK_ACCESS_2_ACCELERATION_STRUCTURE_READ_BIT_KHR | VK_ACCESS_2_SHADER_SAMPLED_READ_BIT)) {
      if (dst_flags & (VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_BINDING_TABLE_READ_BIT_KHR |
                       VK_ACCESS_2_ACCELERATION_STRUCTURE_READ_BIT_KHR)) {
         /* Unlike LLVM, ACO uses SMEM for SSBOs and we have to
          * invalidate the scalar cache. */
         if (!pdev->use_llvm && !image)
            flush_bits |= RADV_CMD_FLAG_INV_SCACHE;
      }

      flush_bits |= RADV_CMD_FLAG_INV_VCACHE;
      if (flush_L2_metadata)
         flush_bits |= RADV_CMD_FLAG_INV_L2_METADATA;
      if (!image_is_coherent)
         flush_bits |= RADV_CMD_FLAG_INV_L2;
   }

   if (dst_flags & VK_ACCESS_2_COMMAND_PREPROCESS_READ_BIT_EXT) {
      flush_bits |= RADV_CMD_FLAG_INV_VCACHE;
      if (pdev->info.gfx_level < GFX9)
         flush_bits |= RADV_CMD_FLAG_INV_L2;
   }

   if (dst_flags & VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT) {
      if (flush_CB)
         flush_bits |= RADV_CMD_FLAG_FLUSH_AND_INV_CB;
      if (has_CB_meta)
         flush_bits |= RADV_CMD_FLAG_FLUSH_AND_INV_CB_META;
   }

   if (dst_flags & VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT) {
      if (flush_DB)
         flush_bits |= RADV_CMD_FLAG_FLUSH_AND_INV_DB;
      if (has_DB_meta)
         flush_bits |= RADV_CMD_FLAG_FLUSH_AND_INV_DB_META;
   }

   return flush_bits;
}

void
radv_emit_resolve_barrier(struct radv_cmd_buffer *cmd_buffer, const struct radv_resolve_barrier *barrier)
{
   struct radv_rendering_state *render = &cmd_buffer->state.render;

   for (uint32_t i = 0; i < render->color_att_count; i++) {
      struct radv_image_view *iview = render->color_att[i].iview;
      if (!iview)
         continue;

      const VkImageSubresourceRange range = vk_image_view_subresource_range(&iview->vk);

      cmd_buffer->state.flush_bits |=
         radv_src_access_flush(cmd_buffer, barrier->src_stage_mask, barrier->src_access_mask, 0, iview->image, &range);
   }
   if (render->ds_att.iview) {
      struct radv_image_view *iview = render->ds_att.iview;

      const VkImageSubresourceRange range = vk_image_view_subresource_range(&iview->vk);

      cmd_buffer->state.flush_bits |= radv_src_access_flush(
         cmd_buffer, barrier->src_stage_mask, barrier->src_access_mask, 0, render->ds_att.iview->image, &range);
   }

   radv_stage_flush(cmd_buffer, barrier->src_stage_mask);

   for (uint32_t i = 0; i < render->color_att_count; i++) {
      struct radv_image_view *iview = render->color_att[i].iview;
      if (!iview)
         continue;

      const VkImageSubresourceRange range = vk_image_view_subresource_range(&iview->vk);

      cmd_buffer->state.flush_bits |=
         radv_dst_access_flush(cmd_buffer, barrier->dst_stage_mask, barrier->dst_access_mask, 0, iview->image, &range);
   }
   if (render->ds_att.iview) {
      struct radv_image_view *iview = render->ds_att.iview;

      const VkImageSubresourceRange range = vk_image_view_subresource_range(&iview->vk);

      cmd_buffer->state.flush_bits |=
         radv_dst_access_flush(cmd_buffer, barrier->dst_stage_mask, barrier->dst_access_mask, 0, iview->image, &range);
   }

   radv_gang_barrier(cmd_buffer, barrier->src_stage_mask, barrier->dst_stage_mask);
}

static void
radv_handle_image_transition_separate(struct radv_cmd_buffer *cmd_buffer, struct radv_image *image,
                                      VkImageLayout src_layout, VkImageLayout dst_layout,
                                      VkImageLayout src_stencil_layout, VkImageLayout dst_stencil_layout,
                                      uint32_t src_family_index, uint32_t dst_family_index,
                                      const VkImageSubresourceRange *range,
                                      struct radv_sample_locations_state *sample_locs)
{
   /* If we have a stencil layout that's different from depth, we need to
    * perform the stencil transition separately.
    */
   if ((range->aspectMask & VK_IMAGE_ASPECT_STENCIL_BIT) &&
       (src_layout != src_stencil_layout || dst_layout != dst_stencil_layout)) {
      VkImageSubresourceRange aspect_range = *range;
      /* Depth-only transitions. */
      if (range->aspectMask & VK_IMAGE_ASPECT_DEPTH_BIT) {
         aspect_range.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
         radv_handle_image_transition(cmd_buffer, image, src_layout, dst_layout, src_family_index, dst_family_index,
                                      &aspect_range, sample_locs);
      }

      /* Stencil-only transitions. */
      aspect_range.aspectMask = VK_IMAGE_ASPECT_STENCIL_BIT;
      radv_handle_image_transition(cmd_buffer, image, src_stencil_layout, dst_stencil_layout, src_family_index,
                                   dst_family_index, &aspect_range, sample_locs);
   } else {
      radv_handle_image_transition(cmd_buffer, image, src_layout, dst_layout, src_family_index, dst_family_index, range,
                                   sample_locs);
   }
}

static void
radv_handle_rendering_image_transition(struct radv_cmd_buffer *cmd_buffer, struct radv_image_view *view,
                                       uint32_t layer_count, uint32_t view_mask, VkImageLayout initial_layout,
                                       VkImageLayout initial_stencil_layout, VkImageLayout final_layout,
                                       VkImageLayout final_stencil_layout,
                                       struct radv_sample_locations_state *sample_locs)
{
   VkImageSubresourceRange range;
   range.aspectMask = view->image->vk.aspects;
   range.baseMipLevel = view->vk.base_mip_level;
   range.levelCount = 1;

   if (view_mask) {
      while (view_mask) {
         int start, count;
         u_bit_scan_consecutive_range(&view_mask, &start, &count);

         range.baseArrayLayer = view->vk.base_array_layer + start;
         range.layerCount = count;

         radv_handle_image_transition_separate(cmd_buffer, view->image, initial_layout, final_layout,
                                               initial_stencil_layout, final_stencil_layout, 0, 0, &range, sample_locs);
      }
   } else {
      range.baseArrayLayer = view->vk.base_array_layer;
      range.layerCount = layer_count;
      radv_handle_image_transition_separate(cmd_buffer, view->image, initial_layout, final_layout,
                                            initial_stencil_layout, final_stencil_layout, 0, 0, &range, sample_locs);
   }
}

static void
radv_init_default_dynamic_graphics_state(struct radv_cmd_buffer *cmd_buffer)
{
   vk_dynamic_graphics_state_init(&cmd_buffer->state.dynamic.vk);

   cmd_buffer->state.dynamic.color_write_enable = 0xffffffffu;
}

VKAPI_ATTR VkResult VKAPI_CALL
radv_BeginCommandBuffer(VkCommandBuffer commandBuffer, const VkCommandBufferBeginInfo *pBeginInfo)
{
   VK_FROM_HANDLE(radv_cmd_buffer, cmd_buffer, commandBuffer);
   struct radv_device *device = radv_cmd_buffer_device(cmd_buffer);
   const struct radv_physical_device *pdev = radv_device_physical(device);
   VkResult result = VK_SUCCESS;

   vk_command_buffer_begin(&cmd_buffer->vk, pBeginInfo);

   if (cmd_buffer->qf == RADV_QUEUE_SPARSE)
      return result;

   memset(&cmd_buffer->state, 0, sizeof(cmd_buffer->state));
   cmd_buffer->state.last_index_type = -1;
   cmd_buffer->state.last_primitive_restart_en = pdev->info.gfx_level >= GFX11 ? false : -1;
   cmd_buffer->state.last_num_instances = -1;
   cmd_buffer->state.last_vertex_offset_valid = false;
   cmd_buffer->state.last_first_instance = -1;
   cmd_buffer->state.last_drawid = -1;
   cmd_buffer->state.last_subpass_color_count = MAX_RTS;
   cmd_buffer->state.predication_type = -1;
   cmd_buffer->state.mesh_shading = false;

   cmd_buffer->usage_flags = pBeginInfo->flags;

   cmd_buffer->state.dirty |= RADV_CMD_DIRTY_GUARDBAND | RADV_CMD_DIRTY_OCCLUSION_QUERY |
                              RADV_CMD_DIRTY_DB_SHADER_CONTROL | RADV_CMD_DIRTY_FRAGMENT_OUTPUT;
   if (pdev->info.rbplus_allowed)
      cmd_buffer->state.dirty |= RADV_CMD_DIRTY_RBPLUS;

   cmd_buffer->state.dirty_dynamic |= RADV_DYNAMIC_ALL;

   if (cmd_buffer->qf == RADV_QUEUE_GENERAL)
      radv_init_default_dynamic_graphics_state(cmd_buffer);

   if (cmd_buffer->qf == RADV_QUEUE_COMPUTE || device->vk.enabled_features.taskShader) {
      uint32_t pred_value = 0;
      uint32_t pred_offset;
      if (!radv_cmd_buffer_upload_data(cmd_buffer, 4, &pred_value, &pred_offset)) {
         vk_command_buffer_set_error(&cmd_buffer->vk, VK_ERROR_OUT_OF_HOST_MEMORY);
         return VK_ERROR_OUT_OF_HOST_MEMORY;
      }

      cmd_buffer->state.mec_inv_pred_emitted = false;
      cmd_buffer->state.mec_inv_pred_va = radv_buffer_get_va(cmd_buffer->upload.upload_bo) + pred_offset;
   }

   if (pdev->info.gfx_level >= GFX9 && cmd_buffer->qf == RADV_QUEUE_GENERAL) {
      unsigned num_db = pdev->info.max_render_backends;
      unsigned fence_offset, eop_bug_offset;
      void *fence_ptr;

      if (!radv_cmd_buffer_upload_alloc(cmd_buffer, 8, &fence_offset, &fence_ptr)) {
         vk_command_buffer_set_error(&cmd_buffer->vk, VK_ERROR_OUT_OF_HOST_MEMORY);
         return VK_ERROR_OUT_OF_HOST_MEMORY;
      }
      memset(fence_ptr, 0, 8);

      cmd_buffer->gfx9_fence_va = radv_buffer_get_va(cmd_buffer->upload.upload_bo);
      cmd_buffer->gfx9_fence_va += fence_offset;

      radv_emit_clear_data(cmd_buffer, V_370_PFP, cmd_buffer->gfx9_fence_va, 8);

      if (pdev->info.gfx_level == GFX9) {
         /* Allocate a buffer for the EOP bug on GFX9. */
         if (!radv_cmd_buffer_upload_alloc(cmd_buffer, 16 * num_db, &eop_bug_offset, &fence_ptr)) {
            vk_command_buffer_set_error(&cmd_buffer->vk, VK_ERROR_OUT_OF_HOST_MEMORY);
            return VK_ERROR_OUT_OF_HOST_MEMORY;
         }

         memset(fence_ptr, 0, 16 * num_db);
         cmd_buffer->gfx9_eop_bug_va = radv_buffer_get_va(cmd_buffer->upload.upload_bo);
         cmd_buffer->gfx9_eop_bug_va += eop_bug_offset;

         radv_emit_clear_data(cmd_buffer, V_370_PFP, cmd_buffer->gfx9_eop_bug_va, 16 * num_db);
      }
   }

   if (cmd_buffer->vk.level == VK_COMMAND_BUFFER_LEVEL_SECONDARY &&
       (pBeginInfo->flags & VK_COMMAND_BUFFER_USAGE_RENDER_PASS_CONTINUE_BIT)) {

      char gcbiar_data[VK_GCBIARR_DATA_SIZE(MAX_RTS)];
      const VkRenderingInfo *resume_info =
         vk_get_command_buffer_inheritance_as_rendering_resume(cmd_buffer->vk.level, pBeginInfo, gcbiar_data);
      if (resume_info) {
         radv_CmdBeginRendering(commandBuffer, resume_info);
      } else {
         const VkCommandBufferInheritanceRenderingInfo *inheritance_info =
            vk_get_command_buffer_inheritance_rendering_info(cmd_buffer->vk.level, pBeginInfo);

         radv_cmd_buffer_reset_rendering(cmd_buffer);
         struct radv_rendering_state *render = &cmd_buffer->state.render;
         render->active = true;
         render->view_mask = inheritance_info->viewMask;
         render->max_samples = inheritance_info->rasterizationSamples;
         render->color_att_count = inheritance_info->colorAttachmentCount;
         for (uint32_t i = 0; i < render->color_att_count; i++) {
            render->color_att[i] = (struct radv_attachment){
               .format = inheritance_info->pColorAttachmentFormats[i],
            };
         }
         assert(inheritance_info->depthAttachmentFormat == VK_FORMAT_UNDEFINED ||
                inheritance_info->stencilAttachmentFormat == VK_FORMAT_UNDEFINED ||
                inheritance_info->depthAttachmentFormat == inheritance_info->stencilAttachmentFormat);
         render->ds_att = (struct radv_attachment){.iview = NULL};
         if (inheritance_info->depthAttachmentFormat != VK_FORMAT_UNDEFINED)
            render->ds_att.format = inheritance_info->depthAttachmentFormat;
         if (inheritance_info->stencilAttachmentFormat != VK_FORMAT_UNDEFINED)
            render->ds_att.format = inheritance_info->stencilAttachmentFormat;

         if (vk_format_has_depth(render->ds_att.format))
            render->ds_att_aspects |= VK_IMAGE_ASPECT_DEPTH_BIT;
         if (vk_format_has_stencil(render->ds_att.format))
            render->ds_att_aspects |= VK_IMAGE_ASPECT_STENCIL_BIT;

         if (pdev->info.gfx_level >= GFX12 && pdev->use_hiz && render->ds_att.format) {
            /* For inherited rendering with secondary commands buffers, assume HiZ/HiS is enabled if
             * there is a depth/stencil attachment. This is required to apply hardware workarounds
             * on GFX12.
             */
            render->has_hiz_his = true;
         }

         const VkRenderingAttachmentLocationInfo *ral_info =
            vk_find_struct_const(pBeginInfo->pInheritanceInfo->pNext, RENDERING_ATTACHMENT_LOCATION_INFO);
         if (ral_info) {
            radv_CmdSetRenderingAttachmentLocations(commandBuffer, ral_info);
         }

         const VkRenderingInputAttachmentIndexInfo *ria_info =
            vk_find_struct_const(pBeginInfo->pInheritanceInfo->pNext, RENDERING_INPUT_ATTACHMENT_INDEX_INFO);
         if (ria_info) {
            radv_CmdSetRenderingInputAttachmentIndices(commandBuffer, ria_info);
         }
      }

      cmd_buffer->state.inherited_pipeline_statistics = pBeginInfo->pInheritanceInfo->pipelineStatistics;

      if (cmd_buffer->state.inherited_pipeline_statistics & VK_QUERY_PIPELINE_STATISTIC_GEOMETRY_SHADER_PRIMITIVES_BIT)
         cmd_buffer->state.dirty |= RADV_CMD_DIRTY_SHADER_QUERY;

      cmd_buffer->state.inherited_occlusion_queries = pBeginInfo->pInheritanceInfo->occlusionQueryEnable;
      cmd_buffer->state.inherited_query_control_flags = pBeginInfo->pInheritanceInfo->queryFlags;
      if (cmd_buffer->state.inherited_occlusion_queries)
         cmd_buffer->state.dirty |= RADV_CMD_DIRTY_OCCLUSION_QUERY;
   }

   if (radv_device_fault_detection_enabled(device))
      radv_cmd_buffer_trace_emit(cmd_buffer);

   radv_describe_begin_cmd_buffer(cmd_buffer);

   return result;
}

VKAPI_ATTR void VKAPI_CALL
radv_CmdBindVertexBuffers2(VkCommandBuffer commandBuffer, uint32_t firstBinding, uint32_t bindingCount,
                           const VkBuffer *pBuffers, const VkDeviceSize *pOffsets, const VkDeviceSize *pSizes,
                           const VkDeviceSize *pStrides)
{
   VK_FROM_HANDLE(radv_cmd_buffer, cmd_buffer, commandBuffer);
   struct radv_device *device = radv_cmd_buffer_device(cmd_buffer);
   struct radv_vertex_binding *vb = cmd_buffer->vertex_bindings;
   struct radv_dynamic_state *d = &cmd_buffer->state.dynamic;
   struct radv_cmd_stream *cs = cmd_buffer->cs;

   /* We have to defer setting up vertex buffer since we need the buffer
    * stride from the pipeline. */

   assert(firstBinding + bindingCount <= MAX_VBS);

   uint32_t misaligned_mask_invalid = 0;

   for (uint32_t i = 0; i < bindingCount; i++) {
      VK_FROM_HANDLE(radv_buffer, buffer, pBuffers[i]);
      uint32_t idx = firstBinding + i;
      VkDeviceSize size = pSizes ? pSizes[i] : VK_WHOLE_SIZE;
      VkDeviceSize stride = pStrides ? pStrides[i] : d->vk.vi_binding_strides[idx];
      uint64_t addr = buffer ? vk_buffer_address(&buffer->vk, pOffsets[i]) : 0;

      if (!!vb[idx].addr != !!addr || (addr && (((vb[idx].addr & 0x3) != (addr & 0x3) ||
                                                 (d->vk.vi_binding_strides[idx] & 0x3) != (stride & 0x3))))) {
         misaligned_mask_invalid |= d->vertex_input.bindings_match_attrib ? BITFIELD_BIT(idx) : 0xffffffff;
      }

      vb[idx].addr = addr;
      vb[idx].size = buffer ? vk_buffer_range(&buffer->vk, pOffsets[i], size) : 0;
      /* if pStrides=NULL, it shouldn't overwrite the strides specified by CmdSetVertexInputEXT */
      if (pStrides)
         radv_cmd_set_vertex_binding_strides(cmd_buffer, idx, 1, (uint16_t *)&pStrides[i]);

      uint32_t bit = BITFIELD_BIT(idx);
      if (buffer) {
         radv_cs_add_buffer(device->ws, cs->b, buffer->bo);
         cmd_buffer->state.vbo_bound_mask |= bit;
      } else {
         cmd_buffer->state.vbo_bound_mask &= ~bit;
      }
   }

   if (misaligned_mask_invalid != d->vertex_input.vbo_misaligned_mask_invalid) {
      d->vertex_input.vbo_misaligned_mask_invalid = misaligned_mask_invalid;
      d->vertex_input.vbo_misaligned_mask &= ~misaligned_mask_invalid;
      d->vertex_input.vbo_unaligned_mask &= ~misaligned_mask_invalid;
      cmd_buffer->state.dirty |= RADV_CMD_DIRTY_VS_PROLOG_STATE;
   }

   cmd_buffer->state.dirty |= RADV_CMD_DIRTY_VERTEX_BUFFER;
}

static uint32_t
vk_to_index_type(VkIndexType type)
{
   switch (type) {
   case VK_INDEX_TYPE_UINT8:
      return V_028A7C_VGT_INDEX_8;
   case VK_INDEX_TYPE_UINT16:
      return V_028A7C_VGT_INDEX_16;
   case VK_INDEX_TYPE_UINT32:
      return V_028A7C_VGT_INDEX_32;
   default:
      UNREACHABLE("invalid index type");
   }
}

static uint32_t
radv_get_vgt_index_size(uint32_t type)
{
   uint32_t index_type = G_028A7C_INDEX_TYPE(type);
   switch (index_type) {
   case V_028A7C_VGT_INDEX_8:
      return 1;
   case V_028A7C_VGT_INDEX_16:
      return 2;
   case V_028A7C_VGT_INDEX_32:
      return 4;
   default:
      UNREACHABLE("invalid index type");
   }
}

VKAPI_ATTR void VKAPI_CALL
radv_CmdBindIndexBuffer2(VkCommandBuffer commandBuffer, VkBuffer buffer, VkDeviceSize offset, VkDeviceSize size,
                         VkIndexType indexType)
{
   VK_FROM_HANDLE(radv_cmd_buffer, cmd_buffer, commandBuffer);
   VK_FROM_HANDLE(radv_buffer, index_buffer, buffer);
   struct radv_device *device = radv_cmd_buffer_device(cmd_buffer);
   const struct radv_physical_device *pdev = radv_device_physical(device);
   struct radv_cmd_stream *cs = cmd_buffer->cs;

   cmd_buffer->state.index_type = vk_to_index_type(indexType);

   if (index_buffer) {
      cmd_buffer->state.index_va = vk_buffer_address(&index_buffer->vk, offset);

      int index_size = radv_get_vgt_index_size(vk_to_index_type(indexType));
      cmd_buffer->state.max_index_count = (vk_buffer_range(&index_buffer->vk, offset, size)) / index_size;
      radv_cs_add_buffer(device->ws, cs->b, index_buffer->bo);
   } else {
      cmd_buffer->state.index_va = 0;
      cmd_buffer->state.max_index_count = 0;

      if (pdev->info.has_null_index_buffer_clamping_bug)
         cmd_buffer->state.index_va = 0x2;
   }

   cmd_buffer->state.dirty |= RADV_CMD_DIRTY_INDEX_BUFFER;
}

static void
radv_bind_descriptor_set(struct radv_cmd_buffer *cmd_buffer, VkPipelineBindPoint bind_point,
                         struct radv_descriptor_set *set, unsigned idx)
{
   struct radv_device *device = radv_cmd_buffer_device(cmd_buffer);
   struct radv_cmd_stream *cs = cmd_buffer->cs;
   struct radeon_winsys *ws = device->ws;

   radv_set_descriptor_set(cmd_buffer, bind_point, set, idx);

   assert(set);
   assert(!(set->header.layout->flags & VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT));

   if (!device->use_global_bo_list) {
      for (unsigned j = 0; j < set->header.buffer_count; ++j)
         if (set->descriptors[j])
            radv_cs_add_buffer(ws, cs->b, set->descriptors[j]);
   }

   if (set->header.bo)
      radv_cs_add_buffer(ws, cs->b, set->header.bo);
}

static void
radv_bind_descriptor_sets(struct radv_cmd_buffer *cmd_buffer, const VkBindDescriptorSetsInfo *pBindDescriptorSetsInfo,
                          VkPipelineBindPoint bind_point)
{
   VK_FROM_HANDLE(radv_pipeline_layout, layout, pBindDescriptorSetsInfo->layout);
   struct radv_device *device = radv_cmd_buffer_device(cmd_buffer);
   const struct radv_physical_device *pdev = radv_device_physical(device);
   const struct radv_instance *instance = radv_physical_device_instance(pdev);
   const bool no_dynamic_bounds = instance->drirc.debug.no_dynamic_bounds;
   struct radv_descriptor_state *descriptors_state = radv_get_descriptors_state(cmd_buffer, bind_point);
   unsigned dyn_idx = 0;

   for (unsigned i = 0; i < pBindDescriptorSetsInfo->descriptorSetCount; ++i) {
      unsigned set_idx = i + pBindDescriptorSetsInfo->firstSet;
      VK_FROM_HANDLE(radv_descriptor_set, set, pBindDescriptorSetsInfo->pDescriptorSets[i]);

      if (!set)
         continue;

      /* If the set is already bound we only need to update the
       * (potentially changed) dynamic offsets. */
      if (descriptors_state->sets[set_idx] != set || !(descriptors_state->valid & (1u << set_idx))) {
         radv_bind_descriptor_set(cmd_buffer, bind_point, set, set_idx);
      }

      for (unsigned j = 0; j < set->header.layout->dynamic_offset_count; ++j, ++dyn_idx) {
         unsigned idx = j + layout->set[i + pBindDescriptorSetsInfo->firstSet].dynamic_offset_start;
         uint32_t *dst = descriptors_state->dynamic_buffers + idx * 4;
         assert(dyn_idx < pBindDescriptorSetsInfo->dynamicOffsetCount);

         struct radv_descriptor_range *range = set->header.dynamic_descriptors + j;

         if (!range->va) {
            memset(dst, 0, 4 * 4);
         } else {
            uint64_t va = range->va + pBindDescriptorSetsInfo->pDynamicOffsets[dyn_idx];
            const uint32_t size = no_dynamic_bounds ? 0xffffffffu : range->size;

            ac_build_raw_buffer_descriptor(pdev->info.gfx_level, va, size, dst);
         }

         descriptors_state->dirty_dynamic = true;
      }
   }
}

VKAPI_ATTR void VKAPI_CALL
radv_CmdBindDescriptorSets2(VkCommandBuffer commandBuffer, const VkBindDescriptorSetsInfo *pBindDescriptorSetsInfo)
{
   VK_FROM_HANDLE(radv_cmd_buffer, cmd_buffer, commandBuffer);

   if (pBindDescriptorSetsInfo->stageFlags & VK_SHADER_STAGE_COMPUTE_BIT) {
      radv_bind_descriptor_sets(cmd_buffer, pBindDescriptorSetsInfo, VK_PIPELINE_BIND_POINT_COMPUTE);
   }

   if (pBindDescriptorSetsInfo->stageFlags & RADV_GRAPHICS_STAGE_BITS) {
      radv_bind_descriptor_sets(cmd_buffer, pBindDescriptorSetsInfo, VK_PIPELINE_BIND_POINT_GRAPHICS);
   }

   if (pBindDescriptorSetsInfo->stageFlags & RADV_RT_STAGE_BITS) {
      radv_bind_descriptor_sets(cmd_buffer, pBindDescriptorSetsInfo, VK_PIPELINE_BIND_POINT_RAY_TRACING_KHR);
   }
}

static bool
radv_init_push_descriptor_set(struct radv_cmd_buffer *cmd_buffer, struct radv_descriptor_set *set,
                              struct radv_descriptor_set_layout *layout, VkPipelineBindPoint bind_point)
{
   struct radv_descriptor_state *descriptors_state = radv_get_descriptors_state(cmd_buffer, bind_point);
   struct radv_device *device = radv_cmd_buffer_device(cmd_buffer);
   set->header.size = layout->size;

   if (set->header.layout != layout) {
      if (set->header.layout)
         vk_descriptor_set_layout_unref(&device->vk, &set->header.layout->vk);
      vk_descriptor_set_layout_ref(&layout->vk);
      set->header.layout = layout;
   }

   if (descriptors_state->push_set.capacity < set->header.size) {
      size_t new_size = MAX2(set->header.size, 1024);
      new_size = MAX2(new_size, 2 * descriptors_state->push_set.capacity);
      new_size = MIN2(new_size, 96 * MAX_PUSH_DESCRIPTORS);

      free(set->header.mapped_ptr);
      set->header.mapped_ptr = malloc(new_size);

      if (!set->header.mapped_ptr) {
         descriptors_state->push_set.capacity = 0;
         vk_command_buffer_set_error(&cmd_buffer->vk, VK_ERROR_OUT_OF_HOST_MEMORY);
         return false;
      }

      descriptors_state->push_set.capacity = new_size;
   }

   return true;
}

static void
radv_push_descriptor_set(struct radv_cmd_buffer *cmd_buffer, const VkPushDescriptorSetInfoKHR *pPushDescriptorSetInfo,
                         VkPipelineBindPoint bind_point)
{
   VK_FROM_HANDLE(radv_pipeline_layout, layout, pPushDescriptorSetInfo->layout);
   struct radv_descriptor_state *descriptors_state = radv_get_descriptors_state(cmd_buffer, bind_point);
   struct radv_descriptor_set *push_set = (struct radv_descriptor_set *)&descriptors_state->push_set.set;
   struct radv_device *device = radv_cmd_buffer_device(cmd_buffer);

   assert(layout->set[pPushDescriptorSetInfo->set].layout->flags & VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT);

   if (!radv_init_push_descriptor_set(cmd_buffer, push_set, layout->set[pPushDescriptorSetInfo->set].layout,
                                      bind_point))
      return;

   /* Check that there are no inline uniform block updates when calling vkCmdPushDescriptorSet()
    * because it is invalid, according to Vulkan spec.
    */
   for (int i = 0; i < pPushDescriptorSetInfo->descriptorWriteCount; i++) {
      ASSERTED const VkWriteDescriptorSet *writeset = &pPushDescriptorSetInfo->pDescriptorWrites[i];
      assert(writeset->descriptorType != VK_DESCRIPTOR_TYPE_INLINE_UNIFORM_BLOCK);
   }

   radv_cmd_update_descriptor_sets(device, cmd_buffer, radv_descriptor_set_to_handle(push_set),
                                   pPushDescriptorSetInfo->descriptorWriteCount,
                                   pPushDescriptorSetInfo->pDescriptorWrites, 0, NULL);

   radv_set_descriptor_set(cmd_buffer, bind_point, push_set, pPushDescriptorSetInfo->set);

   radv_flush_push_descriptors(cmd_buffer, descriptors_state);
}

VKAPI_ATTR void VKAPI_CALL
radv_CmdPushDescriptorSet2KHR(VkCommandBuffer commandBuffer, const VkPushDescriptorSetInfoKHR *pPushDescriptorSetInfo)
{
   VK_FROM_HANDLE(radv_cmd_buffer, cmd_buffer, commandBuffer);

   if (pPushDescriptorSetInfo->stageFlags & VK_SHADER_STAGE_COMPUTE_BIT) {
      radv_push_descriptor_set(cmd_buffer, pPushDescriptorSetInfo, VK_PIPELINE_BIND_POINT_COMPUTE);
   }

   if (pPushDescriptorSetInfo->stageFlags & RADV_GRAPHICS_STAGE_BITS) {
      radv_push_descriptor_set(cmd_buffer, pPushDescriptorSetInfo, VK_PIPELINE_BIND_POINT_GRAPHICS);
   }

   if (pPushDescriptorSetInfo->stageFlags & RADV_RT_STAGE_BITS) {
      radv_push_descriptor_set(cmd_buffer, pPushDescriptorSetInfo, VK_PIPELINE_BIND_POINT_RAY_TRACING_KHR);
   }
}

VKAPI_ATTR void VKAPI_CALL
radv_CmdPushDescriptorSetWithTemplate2KHR(
   VkCommandBuffer commandBuffer, const VkPushDescriptorSetWithTemplateInfoKHR *pPushDescriptorSetWithTemplateInfo)
{
   VK_FROM_HANDLE(radv_cmd_buffer, cmd_buffer, commandBuffer);
   VK_FROM_HANDLE(radv_pipeline_layout, layout, pPushDescriptorSetWithTemplateInfo->layout);
   VK_FROM_HANDLE(radv_descriptor_update_template, templ, pPushDescriptorSetWithTemplateInfo->descriptorUpdateTemplate);
   struct radv_descriptor_state *descriptors_state = radv_get_descriptors_state(cmd_buffer, templ->bind_point);
   struct radv_descriptor_set *push_set = (struct radv_descriptor_set *)&descriptors_state->push_set.set;
   struct radv_device *device = radv_cmd_buffer_device(cmd_buffer);

   assert(layout->set[pPushDescriptorSetWithTemplateInfo->set].layout->flags &
          VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT);

   if (!radv_init_push_descriptor_set(cmd_buffer, push_set, layout->set[pPushDescriptorSetWithTemplateInfo->set].layout,
                                      templ->bind_point))
      return;

   radv_cmd_update_descriptor_set_with_template(device, cmd_buffer, push_set,
                                                pPushDescriptorSetWithTemplateInfo->descriptorUpdateTemplate,
                                                pPushDescriptorSetWithTemplateInfo->pData);

   radv_set_descriptor_set(cmd_buffer, templ->bind_point, push_set, pPushDescriptorSetWithTemplateInfo->set);

   radv_flush_push_descriptors(cmd_buffer, descriptors_state);
}

VKAPI_ATTR void VKAPI_CALL
radv_CmdPushConstants2(VkCommandBuffer commandBuffer, const VkPushConstantsInfo *pPushConstantsInfo)
{
   VK_FROM_HANDLE(radv_cmd_buffer, cmd_buffer, commandBuffer);
   memcpy(cmd_buffer->push_constants + pPushConstantsInfo->offset, pPushConstantsInfo->pValues,
          pPushConstantsInfo->size);
   cmd_buffer->push_constant_stages |= pPushConstantsInfo->stageFlags;
}

VKAPI_ATTR VkResult VKAPI_CALL
radv_EndCommandBuffer(VkCommandBuffer commandBuffer)
{
   VK_FROM_HANDLE(radv_cmd_buffer, cmd_buffer, commandBuffer);
   struct radv_device *device = radv_cmd_buffer_device(cmd_buffer);
   struct radv_cmd_stream *cs = cmd_buffer->cs;
   struct radv_cmd_stream *ace_cs = cmd_buffer->gang.cs;

   if (cmd_buffer->qf == RADV_QUEUE_SPARSE)
      return vk_command_buffer_end(&cmd_buffer->vk);

   radv_emit_mip_change_flush_default(cmd_buffer);

   const bool is_gfx_or_ace = cmd_buffer->qf == RADV_QUEUE_GENERAL || cmd_buffer->qf == RADV_QUEUE_COMPUTE;

   if (is_gfx_or_ace) {
      /* Make sure to sync all pending active queries at the end of
       * command buffer.
       */
      cmd_buffer->state.flush_bits |= cmd_buffer->active_query_flush_bits;

      /* Flush noncoherent images when needed so we can assume they're clean on the start of a
       * command buffer.
       */
      if (cmd_buffer->state.rb_noncoherent_dirty && !can_skip_buffer_l2_flushes(device))
         cmd_buffer->state.flush_bits |= radv_src_access_flush(cmd_buffer, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                                                               VK_ACCESS_2_TRANSFER_WRITE_BIT, 0, NULL, NULL);

      /* Since NGG streamout uses GDS, we need to make GDS idle when
       * we leave the IB, otherwise another process might overwrite
       * it while our shaders are busy.
       */
      if (cmd_buffer->gds_needed)
         cmd_buffer->state.flush_bits |= RADV_CMD_FLAG_PS_PARTIAL_FLUSH;
   }

   /* Finalize the internal compute command stream, if it exists. */
   if (ace_cs) {
      VkResult result = radv_gang_finalize(cmd_buffer);
      if (result != VK_SUCCESS)
         return vk_error(cmd_buffer, result);
   }

   if (is_gfx_or_ace) {
      radv_emit_cache_flush(cmd_buffer);

      /* Make sure CP DMA is idle at the end of IBs because the kernel
       * doesn't wait for it.
       */
      radv_cp_dma_wait_for_idle(cmd_buffer);
   }

   radv_describe_end_cmd_buffer(cmd_buffer);

   VkResult result = radv_finalize_cmd_stream(device, cs);
   if (result != VK_SUCCESS)
      return vk_error(cmd_buffer, result);

   return vk_command_buffer_end(&cmd_buffer->vk);
}

static void
radv_emit_compute_pipeline(struct radv_cmd_buffer *cmd_buffer, struct radv_compute_pipeline *pipeline)
{
   struct radv_device *device = radv_cmd_buffer_device(cmd_buffer);
   const struct radv_physical_device *pdev = radv_device_physical(device);
   struct radv_cmd_stream *cs = cmd_buffer->cs;

   if (pipeline == cmd_buffer->state.emitted_compute_pipeline)
      return;

   radeon_check_space(device->ws, cs->b, pdev->info.gfx_level >= GFX10 ? 25 : 22);

   if (pipeline->base.type == RADV_PIPELINE_COMPUTE) {
      radv_emit_compute_shader(pdev, cs, cmd_buffer->state.shaders[MESA_SHADER_COMPUTE]);
   } else {
      const struct radv_shader *rt_prolog = cmd_buffer->state.rt_prolog;

      radv_emit_compute_shader(pdev, cs, rt_prolog);

      const uint32_t ray_dynamic_callback_stack_base_offset =
         radv_get_user_sgpr_loc(rt_prolog, AC_UD_CS_RAY_DYNAMIC_CALLABLE_STACK_BASE);
      if (ray_dynamic_callback_stack_base_offset) {
         const struct radv_shader_info *cs_info = &rt_prolog->info;

         radeon_begin(cs);
         if (pdev->info.gfx_level >= GFX12) {
            gfx12_push_sh_reg(ray_dynamic_callback_stack_base_offset,
                              rt_prolog->config.scratch_bytes_per_wave / cs_info->wave_size);
         } else {
            radeon_set_sh_reg(ray_dynamic_callback_stack_base_offset,
                              rt_prolog->config.scratch_bytes_per_wave / cs_info->wave_size);
         }
         radeon_end();
      }

      const uint32_t traversal_shader_addr_offset = radv_get_user_sgpr_loc(rt_prolog, AC_UD_CS_TRAVERSAL_SHADER_ADDR);
      struct radv_shader *traversal_shader = cmd_buffer->state.shaders[MESA_SHADER_INTERSECTION];
      if (traversal_shader_addr_offset && traversal_shader) {
         uint64_t traversal_va = traversal_shader->va | radv_rt_priority_traversal;

         radeon_begin(cs);
         if (pdev->info.gfx_level >= GFX12) {
            gfx12_push_32bit_pointer(traversal_shader_addr_offset, traversal_va, &pdev->info);
         } else {
            radeon_emit_32bit_pointer(traversal_shader_addr_offset, traversal_va, &pdev->info);
         }
         radeon_end();
      }
   }

   cmd_buffer->state.emitted_compute_pipeline = pipeline;

   if (radv_device_fault_detection_enabled(device))
      radv_save_pipeline(cmd_buffer, &pipeline->base);
}

static void
radv_mark_descriptors_dirty(struct radv_cmd_buffer *cmd_buffer, VkPipelineBindPoint bind_point)
{
   struct radv_descriptor_state *descriptors_state = radv_get_descriptors_state(cmd_buffer, bind_point);

   descriptors_state->dirty |= descriptors_state->valid;
   if (descriptors_state->dynamic_offset_count)
      descriptors_state->dirty_dynamic = true;
}

static void
radv_bind_multisample_state(struct radv_cmd_buffer *cmd_buffer, const struct radv_multisample_state *ms)
{
   const struct radv_device *device = radv_cmd_buffer_device(cmd_buffer);
   const struct radv_physical_device *pdev = radv_device_physical(device);

   if (cmd_buffer->state.ms.sample_shading_enable != ms->sample_shading_enable) {
      cmd_buffer->state.ms.sample_shading_enable = ms->sample_shading_enable;
      cmd_buffer->state.dirty |= RADV_CMD_DIRTY_RAST_SAMPLES_STATE | RADV_CMD_DIRTY_MSAA_STATE;
      if (pdev->info.gfx_level >= GFX10_3)
         cmd_buffer->state.dirty |= RADV_CMD_DIRTY_FSR_STATE;
      if (pdev->info.gfx_level == GFX9)
         cmd_buffer->state.dirty |= RADV_CMD_DIRTY_BINNING_STATE;
   }

   if (ms->sample_shading_enable) {
      if (cmd_buffer->state.ms.min_sample_shading != ms->min_sample_shading) {
         cmd_buffer->state.ms.min_sample_shading = ms->min_sample_shading;
         cmd_buffer->state.dirty |= RADV_CMD_DIRTY_RAST_SAMPLES_STATE | RADV_CMD_DIRTY_MSAA_STATE;
         if (pdev->info.gfx_level == GFX9)
            cmd_buffer->state.dirty |= RADV_CMD_DIRTY_BINNING_STATE;
      }
   }
}

static void
radv_bind_custom_blend_mode(struct radv_cmd_buffer *cmd_buffer, unsigned custom_blend_mode)
{
   /* Re-emit CB_COLOR_CONTROL when the custom blending mode changes. */
   if (cmd_buffer->state.custom_blend_mode != custom_blend_mode)
      cmd_buffer->state.dirty |= RADV_CMD_DIRTY_CB_RENDER_STATE;

   cmd_buffer->state.custom_blend_mode = custom_blend_mode;
}

static bool
radv_can_enable_rbplus_depth_only(const struct radv_cmd_buffer *cmd_buffer, const struct radv_shader *ps,
                                  uint32_t col_format, uint32_t custom_blend_mode)
{
   const struct radv_device *device = radv_cmd_buffer_device(cmd_buffer);
   const struct radv_physical_device *pdev = radv_device_physical(device);

   if (!pdev->info.rbplus_allowed)
      return false;

   /* Enable RB+ for depth-only rendering. Registers must be programmed as follows:
    *    CB_COLOR_CONTROL.MODE = CB_DISABLE
    *    CB_COLOR0_INFO.FORMAT = COLOR_32
    *    CB_COLOR0_INFO.NUMBER_TYPE = NUMBER_FLOAT
    *    SPI_SHADER_COL_FORMAT.COL0_EXPORT_FORMAT = SPI_SHADER_32_R
    *    SX_PS_DOWNCONVERT.MRT0 = SX_RT_EXPORT_32_R
    *
    * col_format == 0 implies no color outputs written and no alpha to coverage.
    */

   /* Do not enable for secondaries because it depends on states that we might not know. */
   if (cmd_buffer->vk.level == VK_COMMAND_BUFFER_LEVEL_SECONDARY)
      return false;

   /* Do not enable for internal operations which program CB_MODE differently. */
   if (custom_blend_mode)
      return false;

   return !col_format && (!ps || !ps->info.ps.writes_memory);
}

static void
radv_bind_fragment_output_state(struct radv_cmd_buffer *cmd_buffer, const struct radv_shader *ps,
                                const struct radv_shader_part *ps_epilog, uint32_t custom_blend_mode)
{
   const struct radv_device *device = radv_cmd_buffer_device(cmd_buffer);
   const struct radv_physical_device *pdev = radv_device_physical(device);
   uint32_t col_format = 0, z_format = 0, cb_shader_mask = 0;

   if (ps) {
      col_format = ps_epilog ? ps_epilog->spi_shader_col_format : ps->info.ps.spi_shader_col_format;
      z_format = ps_epilog && ps->info.ps.exports_mrtz_via_epilog ? ps_epilog->spi_shader_z_format
                                                                  : ps->info.regs.ps.spi_shader_z_format;
      cb_shader_mask = ps_epilog ? ps_epilog->cb_shader_mask : ps->info.ps.cb_shader_mask;
   }

   if (custom_blend_mode) {
      /* According to the CB spec states, CB_SHADER_MASK should be set to enable writes to all four
       * channels of MRT0.
       */
      cb_shader_mask = 0xf;
   }

   const bool rbplus_depth_only_enabled =
      radv_can_enable_rbplus_depth_only(cmd_buffer, ps, col_format, custom_blend_mode);

   if ((radv_needs_null_export_workaround(device, ps, custom_blend_mode) && !col_format) || rbplus_depth_only_enabled)
      col_format = V_028714_SPI_SHADER_32_R;

   if (cmd_buffer->state.spi_shader_col_format != col_format) {
      cmd_buffer->state.spi_shader_col_format = col_format;
      cmd_buffer->state.dirty |= RADV_CMD_DIRTY_FRAGMENT_OUTPUT;
      if (pdev->info.rbplus_allowed)
         cmd_buffer->state.dirty |= RADV_CMD_DIRTY_RBPLUS;
   }

   if (cmd_buffer->state.cb_shader_mask != cb_shader_mask || cmd_buffer->state.spi_shader_z_format != z_format) {
      cmd_buffer->state.cb_shader_mask = cb_shader_mask;
      cmd_buffer->state.spi_shader_z_format = z_format;
      cmd_buffer->state.dirty |= RADV_CMD_DIRTY_FRAGMENT_OUTPUT;
   }
}

static void
radv_bind_pre_rast_shader(struct radv_cmd_buffer *cmd_buffer, const struct radv_shader *shader)
{
   struct radv_device *device = radv_cmd_buffer_device(cmd_buffer);
   const struct radv_physical_device *pdev = radv_device_physical(device);
   bool mesh_shading = shader->info.stage == MESA_SHADER_MESH;
   const struct radv_userdata_info *loc;

   assert(shader->info.stage == MESA_SHADER_VERTEX || shader->info.stage == MESA_SHADER_TESS_CTRL ||
          shader->info.stage == MESA_SHADER_TESS_EVAL || shader->info.stage == MESA_SHADER_GEOMETRY ||
          shader->info.stage == MESA_SHADER_MESH);

   if (radv_get_user_sgpr_info(shader, AC_UD_NGG_STATE)->sgpr_idx != -1 ||
       radv_get_user_sgpr_info(shader, AC_UD_NGG_QUERY_BUF_VA)->sgpr_idx != -1)
      cmd_buffer->state.dirty |= RADV_CMD_DIRTY_NGG_STATE;

   if (radv_get_user_sgpr_info(shader, AC_UD_NGGC_SETTINGS)->sgpr_idx != -1)
      cmd_buffer->state.dirty |= RADV_CMD_DIRTY_NGGC_SETTINGS;

   if (radv_get_user_sgpr_info(shader, AC_UD_NGGC_VIEWPORT)->sgpr_idx != -1)
      cmd_buffer->state.dirty |= RADV_CMD_DIRTY_NGGC_VIEWPORT;

   if (radv_get_user_sgpr_info(shader, AC_UD_STREAMOUT_BUFFERS)->sgpr_idx != -1 ||
       radv_get_user_sgpr_info(shader, AC_UD_STREAMOUT_STATE)->sgpr_idx != -1) {
      /* Re-emit the streamout buffers because the SGPR idx can be different and with NGG streamout
       * they always need to be emitted because a buffer size of 0 is used to disable streamout.
       */
      cmd_buffer->state.dirty |= RADV_CMD_DIRTY_STREAMOUT_BUFFER;

      if (pdev->use_ngg_streamout && pdev->info.gfx_level < GFX12) {
         /* GFX11 needs GDS OA for streamout. */
         cmd_buffer->gds_oa_needed = true;
      }
   }

   if (radv_get_user_sgpr_info(shader, AC_UD_FORCE_VRS_RATES)->sgpr_idx != -1)
      cmd_buffer->state.dirty |= RADV_CMD_DIRTY_FORCE_VRS_STATE;

   /* Re-emit the VS prolog when a new vertex shader is bound. */
   if (shader->info.vs.has_prolog) {
      cmd_buffer->state.emitted_vs_prolog = NULL;
      cmd_buffer->state.dirty |= RADV_CMD_DIRTY_VS_PROLOG_STATE;
   }

   /* Re-emit the vertex buffer descriptors because they are really tied to the pipeline. */
   if (shader->info.vs.vb_desc_usage_mask) {
      cmd_buffer->state.dirty |= RADV_CMD_DIRTY_VERTEX_BUFFER;
   }

   const bool needs_vtx_sgpr =
      shader->info.stage == MESA_SHADER_VERTEX || shader->info.stage == MESA_SHADER_MESH ||
      (shader->info.stage == MESA_SHADER_GEOMETRY && !shader->info.merged_shader_compiled_separately) ||
      (shader->info.stage == MESA_SHADER_TESS_CTRL && !shader->info.merged_shader_compiled_separately);

   loc = radv_get_user_sgpr_info(shader, AC_UD_VS_BASE_VERTEX_START_INSTANCE);
   if (needs_vtx_sgpr && loc->sgpr_idx != -1) {
      cmd_buffer->state.vtx_base_sgpr = shader->info.user_data_0 + loc->sgpr_idx * 4;
      cmd_buffer->state.vtx_emit_num = loc->num_sgprs;
      cmd_buffer->state.uses_drawid = shader->info.vs.needs_draw_id;
      cmd_buffer->state.uses_baseinstance = shader->info.vs.needs_base_instance;

      if (shader->info.merged_shader_compiled_separately) {
         /* Merged shaders compiled separately (eg. VS+TCS) always declare these user SGPRS
          * because the input arguments must match.
          */
         cmd_buffer->state.uses_drawid = true;
         cmd_buffer->state.uses_baseinstance = true;
      }

      /* Re-emit some vertex states because the SGPR idx can be different. */
      cmd_buffer->state.last_first_instance = -1;
      cmd_buffer->state.last_vertex_offset_valid = false;
      cmd_buffer->state.last_drawid = -1;
   }

   if (mesh_shading != cmd_buffer->state.mesh_shading) {
      /* Re-emit VRS state because the combiner is different (vertex vs primitive). Re-emit
       * primitive topology because the mesh shading pipeline clobbered it.
       */
      cmd_buffer->state.dirty |= RADV_CMD_DIRTY_FSR_STATE | RADV_CMD_DIRTY_VGT_PRIM_STATE;
   }

   /* Determine if this shader is the last VGT shader. */
   if (shader->info.next_stage == MESA_SHADER_NONE || shader->info.next_stage == MESA_SHADER_FRAGMENT) {
      if (pdev->info.has_vgt_flush_ngg_legacy_bug &&
          (!cmd_buffer->state.last_vgt_shader ||
           (cmd_buffer->state.last_vgt_shader->info.is_ngg && !shader->info.is_ngg))) {
         /* Transitioning from NGG to legacy GS requires VGT_FLUSH on GFX10 and Navi21. VGT_FLUSH is
          * also emitted at the beginning of IBs when legacy GS ring pointers are set.
          */
         cmd_buffer->state.flush_bits |= RADV_CMD_FLAG_VGT_FLUSH;
      }

      cmd_buffer->state.last_vgt_shader = (struct radv_shader *)shader;
   }

   cmd_buffer->state.mesh_shading = mesh_shading;
}

static void
radv_bind_vertex_shader(struct radv_cmd_buffer *cmd_buffer, const struct radv_shader *vs)
{
   struct radv_device *device = radv_cmd_buffer_device(cmd_buffer);
   const struct radv_physical_device *pdev = radv_device_physical(device);

   radv_bind_pre_rast_shader(cmd_buffer, vs);

   /* Re-emit states that need to be updated when the vertex shader is compiled separately
    * because shader configs are combined.
    */
   if (vs->info.merged_shader_compiled_separately && vs->info.next_stage == MESA_SHADER_TESS_CTRL) {
      cmd_buffer->state.dirty |= RADV_CMD_DIRTY_TCS_TES_STATE;
   }

   cmd_buffer->state.can_use_simple_vertex_input = !vs->info.merged_shader_compiled_separately &&
                                                   vs->info.is_ngg == pdev->use_ngg &&
                                                   vs->info.wave_size == pdev->ge_wave_size;
   /* Can't put anything else here due to merged shaders */
}

static void
radv_bind_tess_ctrl_shader(struct radv_cmd_buffer *cmd_buffer, const struct radv_shader *tcs)
{
   radv_bind_pre_rast_shader(cmd_buffer, tcs);

   cmd_buffer->tess_rings_needed = true;

   /* Always re-emit patch control points/domain origin when a new pipeline with tessellation is
    * bound because a bunch of parameters (user SGPRs, TCS vertices out, ccw, etc) can be different.
    */
   cmd_buffer->state.dirty |=
      RADV_CMD_DIRTY_LS_HS_CONFIG | RADV_CMD_DIRTY_TESS_DOMAIN_ORIGIN_STATE | RADV_CMD_DIRTY_TCS_TES_STATE;

   /* Re-emit the VS prolog when the tessellation control shader is compiled separately because
    * shader configs are combined and need to be updated.
    */
   if (tcs->info.merged_shader_compiled_separately)
      cmd_buffer->state.emitted_vs_prolog = NULL;
}

static void
radv_bind_tess_eval_shader(struct radv_cmd_buffer *cmd_buffer, const struct radv_shader *tes)
{
   radv_bind_pre_rast_shader(cmd_buffer, tes);

   /* Can't put anything else here due to merged shaders */
}

static void
radv_bind_geometry_shader(struct radv_cmd_buffer *cmd_buffer, const struct radv_shader *gs)
{
   radv_bind_pre_rast_shader(cmd_buffer, gs);

   cmd_buffer->esgs_ring_size_needed = MAX2(cmd_buffer->esgs_ring_size_needed, gs->info.gs_ring_info.esgs_ring_size);
   cmd_buffer->gsvs_ring_size_needed = MAX2(cmd_buffer->gsvs_ring_size_needed, gs->info.gs_ring_info.gsvs_ring_size);

   /* Re-emit the VS prolog when the geometry shader is compiled separately because shader configs
    * are combined and need to be updated.
    */
   if (gs->info.merged_shader_compiled_separately)
      cmd_buffer->state.emitted_vs_prolog = NULL;
}

static void
radv_bind_gs_copy_shader(struct radv_cmd_buffer *cmd_buffer, struct radv_shader *gs_copy_shader)
{
   struct radv_device *device = radv_cmd_buffer_device(cmd_buffer);
   struct radv_cmd_stream *cs = cmd_buffer->cs;

   cmd_buffer->state.gs_copy_shader = gs_copy_shader;

   if (gs_copy_shader) {
      cmd_buffer->shader_upload_seq = MAX2(cmd_buffer->shader_upload_seq, gs_copy_shader->upload_seq);

      radv_cs_add_buffer(device->ws, cs->b, gs_copy_shader->bo);

      if (radv_get_user_sgpr_info(gs_copy_shader, AC_UD_FORCE_VRS_RATES)->sgpr_idx != -1)
         cmd_buffer->state.dirty |= RADV_CMD_DIRTY_FORCE_VRS_STATE;
   }
}

static void
radv_bind_mesh_shader(struct radv_cmd_buffer *cmd_buffer, const struct radv_shader *ms)
{
   radv_bind_pre_rast_shader(cmd_buffer, ms);

   cmd_buffer->mesh_scratch_ring_needed |= ms->info.ms.needs_ms_scratch_ring;
}

static void
radv_bind_fragment_shader(struct radv_cmd_buffer *cmd_buffer, const struct radv_shader *ps)
{
   struct radv_device *device = radv_cmd_buffer_device(cmd_buffer);
   const struct radv_physical_device *pdev = radv_device_physical(device);
   const enum amd_gfx_level gfx_level = pdev->info.gfx_level;
   const struct radv_shader *previous_ps = cmd_buffer->state.shaders[MESA_SHADER_FRAGMENT];

   if (ps->info.ps.needs_sample_positions) {
      cmd_buffer->sample_positions_needed = true;
   }

   if (ps->info.ps.has_epilog)
      cmd_buffer->state.dirty |= RADV_CMD_DIRTY_PS_EPILOG_SHADER | RADV_CMD_DIRTY_PS_EPILOG_STATE;

   if (radv_get_user_sgpr_info(ps, AC_UD_PS_STATE)->sgpr_idx != -1)
      cmd_buffer->state.dirty |= RADV_CMD_DIRTY_PS_STATE;

   if (!previous_ps || previous_ps->info.ps.reads_fully_covered != ps->info.ps.reads_fully_covered)
      cmd_buffer->state.dirty |= RADV_CMD_DIRTY_MSAA_STATE;

   if (gfx_level >= GFX10_3 && (!previous_ps || previous_ps->info.ps.force_sample_iter_shading_rate !=
                                                   ps->info.ps.force_sample_iter_shading_rate)) {
      cmd_buffer->state.dirty |= RADV_CMD_DIRTY_FSR_STATE | RADV_CMD_DIRTY_RAST_SAMPLES_STATE;
   }

   if (!previous_ps || previous_ps->info.ps.uses_sample_shading != ps->info.ps.uses_sample_shading) {
      cmd_buffer->state.dirty |= RADV_CMD_DIRTY_RAST_SAMPLES_STATE | RADV_CMD_DIRTY_MSAA_STATE;
      if (gfx_level >= GFX10_3)
         cmd_buffer->state.dirty |= RADV_CMD_DIRTY_FSR_STATE;
      if (gfx_level == GFX9)
         cmd_buffer->state.dirty |= RADV_CMD_DIRTY_BINNING_STATE;
   }

   if (!previous_ps || previous_ps->info.regs.ps.db_shader_control != ps->info.regs.ps.db_shader_control ||
       previous_ps->info.ps.pops_is_per_sample != ps->info.ps.pops_is_per_sample)
      cmd_buffer->state.dirty |= RADV_CMD_DIRTY_DB_SHADER_CONTROL;

   if (!previous_ps || cmd_buffer->state.uses_fbfetch_output != ps->info.ps.uses_fbfetch_output) {
      cmd_buffer->state.uses_fbfetch_output = ps->info.ps.uses_fbfetch_output;
      cmd_buffer->state.dirty |= RADV_CMD_DIRTY_FBFETCH_OUTPUT;
   }
}

static void
radv_bind_task_shader(struct radv_cmd_buffer *cmd_buffer, const struct radv_shader *ts)
{
   if (!radv_gang_init(cmd_buffer))
      return;

   if (radv_get_user_sgpr_info(ts, AC_UD_TASK_STATE)->sgpr_idx != -1)
      cmd_buffer->state.dirty |= RADV_CMD_DIRTY_TASK_STATE;

   cmd_buffer->task_rings_needed = true;
}

static void
radv_bind_rt_prolog(struct radv_cmd_buffer *cmd_buffer, struct radv_shader *rt_prolog)
{
   struct radv_cmd_stream *cs = cmd_buffer->cs;

   cmd_buffer->state.rt_prolog = rt_prolog;

   struct radv_device *device = radv_cmd_buffer_device(cmd_buffer);
   const unsigned max_scratch_waves = radv_get_max_scratch_waves(device, rt_prolog);
   cmd_buffer->compute_scratch_waves_wanted = MAX2(cmd_buffer->compute_scratch_waves_wanted, max_scratch_waves);

   cmd_buffer->shader_upload_seq = MAX2(cmd_buffer->shader_upload_seq, rt_prolog->upload_seq);

   radv_cs_add_buffer(device->ws, cs->b, rt_prolog->bo);
}

static void
radv_bind_ps_epilog(struct radv_cmd_buffer *cmd_buffer)
{
   const struct radv_device *device = radv_cmd_buffer_device(cmd_buffer);
   const struct radv_shader *ps = cmd_buffer->state.shaders[MESA_SHADER_FRAGMENT];
   struct radv_cmd_stream *cs = cmd_buffer->cs;
   struct radv_shader_part *ps_epilog;

   if (!ps || !ps->info.ps.has_epilog)
      return;

   ps_epilog = lookup_ps_epilog(cmd_buffer);
   if (!ps_epilog) {
      vk_command_buffer_set_error(&cmd_buffer->vk, VK_ERROR_OUT_OF_HOST_MEMORY);
      return;
   }

   assert(cmd_buffer->state.custom_blend_mode == 0);
   radv_bind_fragment_output_state(cmd_buffer, ps, ps_epilog, 0);

   if (cmd_buffer->state.ps_epilog == ps_epilog)
      return;

   cmd_buffer->state.ps_epilog = ps_epilog;

   cmd_buffer->shader_upload_seq = MAX2(cmd_buffer->shader_upload_seq, ps_epilog->upload_seq);

   radv_cs_add_buffer(device->ws, cs->b, ps_epilog->bo);

   cmd_buffer->state.dirty |= RADV_CMD_DIRTY_PS_EPILOG_STATE;
}

/* This function binds/unbinds a shader to the cmdbuffer state. */
static void
radv_bind_shader(struct radv_cmd_buffer *cmd_buffer, struct radv_shader *shader, mesa_shader_stage stage)
{
   struct radv_device *device = radv_cmd_buffer_device(cmd_buffer);
   const struct radv_physical_device *pdev = radv_device_physical(device);
   struct radv_cmd_stream *cs = cmd_buffer->cs;

   if (!shader) {
      cmd_buffer->state.shaders[stage] = NULL;
      cmd_buffer->state.active_stages &= ~mesa_to_vk_shader_stage(stage);

      /* Reset some dynamic states when a shader stage is unbound. */
      switch (stage) {
      case MESA_SHADER_VERTEX:
         cmd_buffer->state.can_use_simple_vertex_input = false;
         break;
      case MESA_SHADER_FRAGMENT:
         cmd_buffer->state.dirty |=
            RADV_CMD_DIRTY_DB_SHADER_CONTROL | RADV_CMD_DIRTY_MSAA_STATE | RADV_CMD_DIRTY_RAST_SAMPLES_STATE;
         if (pdev->info.gfx_level >= GFX10_3)
            cmd_buffer->state.dirty |= RADV_CMD_DIRTY_FSR_STATE;
         if (pdev->info.gfx_level == GFX9)
            cmd_buffer->state.dirty |= RADV_CMD_DIRTY_BINNING_STATE;
         break;
      default:
         break;
      }
      return;
   }

   switch (stage) {
   case MESA_SHADER_VERTEX:
      radv_bind_vertex_shader(cmd_buffer, shader);
      break;
   case MESA_SHADER_TESS_CTRL:
      radv_bind_tess_ctrl_shader(cmd_buffer, shader);
      break;
   case MESA_SHADER_TESS_EVAL:
      radv_bind_tess_eval_shader(cmd_buffer, shader);
      break;
   case MESA_SHADER_GEOMETRY:
      radv_bind_geometry_shader(cmd_buffer, shader);
      break;
   case MESA_SHADER_FRAGMENT:
      radv_bind_fragment_shader(cmd_buffer, shader);
      break;
   case MESA_SHADER_MESH:
      radv_bind_mesh_shader(cmd_buffer, shader);
      break;
   case MESA_SHADER_TASK:
      radv_bind_task_shader(cmd_buffer, shader);
      break;
   case MESA_SHADER_COMPUTE: {
      cmd_buffer->compute_scratch_size_per_wave_needed =
         MAX2(cmd_buffer->compute_scratch_size_per_wave_needed, shader->config.scratch_bytes_per_wave);

      const unsigned max_stage_waves = radv_get_max_scratch_waves(device, shader);
      cmd_buffer->compute_scratch_waves_wanted = MAX2(cmd_buffer->compute_scratch_waves_wanted, max_stage_waves);
      break;
   }
   case MESA_SHADER_INTERSECTION:
      /* no-op */
      break;
   default:
      UNREACHABLE("invalid shader stage");
   }

   cmd_buffer->state.shaders[stage] = shader;
   cmd_buffer->state.active_stages |= mesa_to_vk_shader_stage(stage);

   if (mesa_to_vk_shader_stage(stage) & RADV_GRAPHICS_STAGE_BITS) {
      cmd_buffer->scratch_size_per_wave_needed =
         MAX2(cmd_buffer->scratch_size_per_wave_needed, shader->config.scratch_bytes_per_wave);

      const unsigned max_stage_waves = radv_get_max_scratch_waves(device, shader);
      cmd_buffer->scratch_waves_wanted = MAX2(cmd_buffer->scratch_waves_wanted, max_stage_waves);
   }

   cmd_buffer->shader_upload_seq = MAX2(cmd_buffer->shader_upload_seq, shader->upload_seq);

   radv_cs_add_buffer(device->ws, cs->b, shader->bo);
}

static void
radv_reset_shader_object_state(struct radv_cmd_buffer *cmd_buffer, VkPipelineBindPoint pipelineBindPoint)
{
   switch (pipelineBindPoint) {
   case VK_PIPELINE_BIND_POINT_COMPUTE:
      if (cmd_buffer->state.shader_objs[MESA_SHADER_COMPUTE]) {
         radv_bind_shader(cmd_buffer, NULL, MESA_SHADER_COMPUTE);
         cmd_buffer->state.shader_objs[MESA_SHADER_COMPUTE] = NULL;
      }
      break;
   case VK_PIPELINE_BIND_POINT_GRAPHICS:
      radv_foreach_stage (s, RADV_GRAPHICS_STAGE_BITS) {
         if (cmd_buffer->state.shader_objs[s]) {
            radv_bind_shader(cmd_buffer, NULL, s);
            cmd_buffer->state.shader_objs[s] = NULL;
         }
      }
      break;
   default:
      break;
   }

   cmd_buffer->state.dirty &= ~RADV_CMD_DIRTY_GRAPHICS_SHADERS;
}

VKAPI_ATTR void VKAPI_CALL
radv_CmdBindPipeline(VkCommandBuffer commandBuffer, VkPipelineBindPoint pipelineBindPoint, VkPipeline _pipeline)
{
   VK_FROM_HANDLE(radv_cmd_buffer, cmd_buffer, commandBuffer);
   VK_FROM_HANDLE(radv_pipeline, pipeline, _pipeline);
   struct radv_device *device = radv_cmd_buffer_device(cmd_buffer);
   struct radv_cmd_stream *cs = cmd_buffer->cs;

   radv_reset_shader_object_state(cmd_buffer, pipelineBindPoint);

   switch (pipelineBindPoint) {
   case VK_PIPELINE_BIND_POINT_COMPUTE: {
      struct radv_compute_pipeline *compute_pipeline = radv_pipeline_to_compute(pipeline);

      if (cmd_buffer->state.compute_pipeline == compute_pipeline)
         return;

      radv_bind_shader(cmd_buffer, compute_pipeline->base.shaders[MESA_SHADER_COMPUTE], MESA_SHADER_COMPUTE);

      cmd_buffer->state.compute_pipeline = compute_pipeline;
      cmd_buffer->push_constant_stages |= VK_SHADER_STAGE_COMPUTE_BIT;
      cmd_buffer->state.prefetch_L2_mask |= RADV_PREFETCH_CS;
      break;
   }
   case VK_PIPELINE_BIND_POINT_RAY_TRACING_KHR: {
      struct radv_ray_tracing_pipeline *rt_pipeline = radv_pipeline_to_ray_tracing(pipeline);

      if (cmd_buffer->state.rt_pipeline == rt_pipeline)
         return;

      radv_bind_shader(cmd_buffer, rt_pipeline->base.base.shaders[MESA_SHADER_INTERSECTION], MESA_SHADER_INTERSECTION);
      radv_bind_rt_prolog(cmd_buffer, rt_pipeline->prolog);

      for (unsigned i = 0; i < rt_pipeline->stage_count; ++i) {
         struct radv_shader *shader = rt_pipeline->stages[i].shader;
         if (!shader)
            continue;

         cmd_buffer->shader_upload_seq = MAX2(cmd_buffer->shader_upload_seq, shader->upload_seq);
         radv_cs_add_buffer(device->ws, cs->b, shader->bo);
      }

      cmd_buffer->state.rt_pipeline = rt_pipeline;
      cmd_buffer->push_constant_stages |= RADV_RT_STAGE_BITS;
      cmd_buffer->state.prefetch_L2_mask |= RADV_PREFETCH_RT;

      /* Bind the stack size when it's not dynamic. */
      if (rt_pipeline->stack_size != -1u)
         cmd_buffer->state.rt_stack_size = rt_pipeline->stack_size;

      break;
   }
   case VK_PIPELINE_BIND_POINT_GRAPHICS: {
      struct radv_graphics_pipeline *graphics_pipeline = radv_pipeline_to_graphics(pipeline);

      /* Bind the non-dynamic graphics state from the pipeline unconditionally because some PSO
       * might have been overwritten between two binds of the same pipeline.
       */
      radv_bind_dynamic_state(cmd_buffer, &graphics_pipeline->dynamic_state);

      if (cmd_buffer->state.graphics_pipeline == graphics_pipeline)
         return;

      radv_foreach_stage (
         stage, (cmd_buffer->state.active_stages | graphics_pipeline->active_stages) & RADV_GRAPHICS_STAGE_BITS) {
         radv_bind_shader(cmd_buffer, graphics_pipeline->base.shaders[stage], stage);
      }

      radv_bind_gs_copy_shader(cmd_buffer, graphics_pipeline->base.gs_copy_shader);

      cmd_buffer->state.graphics_pipeline = graphics_pipeline;

      cmd_buffer->state.dirty |= RADV_CMD_DIRTY_PIPELINE;
      cmd_buffer->push_constant_stages |= graphics_pipeline->active_stages;

      /* Prefetch all pipeline shaders at first draw time. */
      cmd_buffer->state.prefetch_L2_mask |= RADV_PREFETCH_GFX_SHADERS;

      const struct radv_shader *ps = radv_get_shader(graphics_pipeline->base.shaders, MESA_SHADER_FRAGMENT);

      radv_bind_fragment_output_state(cmd_buffer, ps, NULL, graphics_pipeline->custom_blend_mode);

      radv_bind_multisample_state(cmd_buffer, &graphics_pipeline->ms);

      radv_bind_custom_blend_mode(cmd_buffer, graphics_pipeline->custom_blend_mode);

      if (cmd_buffer->state.db_render_control != graphics_pipeline->db_render_control) {
         cmd_buffer->state.db_render_control = graphics_pipeline->db_render_control;
         cmd_buffer->state.dirty |= RADV_CMD_DIRTY_FRAMEBUFFER;
      }

      if (cmd_buffer->state.uses_out_of_order_rast != graphics_pipeline->uses_out_of_order_rast ||
          cmd_buffer->state.uses_vrs_attachment != graphics_pipeline->uses_vrs_attachment) {
         cmd_buffer->state.uses_out_of_order_rast = graphics_pipeline->uses_out_of_order_rast;
         cmd_buffer->state.uses_vrs_attachment = graphics_pipeline->uses_vrs_attachment;
         cmd_buffer->state.dirty |= RADV_CMD_DIRTY_RAST_SAMPLES_STATE;
      }

      cmd_buffer->state.ia_multi_vgt_param = graphics_pipeline->ia_multi_vgt_param;

      cmd_buffer->state.uses_vrs = graphics_pipeline->uses_vrs;
      cmd_buffer->state.uses_vrs_coarse_shading = graphics_pipeline->uses_vrs_coarse_shading;
      break;
   }
   default:
      assert(!"invalid bind point");
      break;
   }

   cmd_buffer->push_constant_state[vk_to_bind_point(pipelineBindPoint)].size = pipeline->push_constant_size;
   cmd_buffer->push_constant_state[vk_to_bind_point(pipelineBindPoint)].need_upload =
      pipeline->need_push_constants_upload;
   cmd_buffer->descriptors[vk_to_bind_point(pipelineBindPoint)].dynamic_offset_count = pipeline->dynamic_offset_count;
   cmd_buffer->descriptors[vk_to_bind_point(pipelineBindPoint)].need_indirect_descriptors =
      pipeline->need_indirect_descriptors;

   radv_mark_descriptors_dirty(cmd_buffer, pipelineBindPoint);
}

VKAPI_ATTR void VKAPI_CALL
radv_CmdSetViewport(VkCommandBuffer commandBuffer, uint32_t firstViewport, uint32_t viewportCount,
                    const VkViewport *pViewports)
{
   VK_FROM_HANDLE(radv_cmd_buffer, cmd_buffer, commandBuffer);
   struct radv_viewport_xform_state vp_xform[MAX_VIEWPORTS];

   for (unsigned i = 0; i < viewportCount; i++) {
      radv_get_viewport_xform(&pViewports[i], vp_xform[i].scale, vp_xform[i].translate);
   }

   radv_cmd_set_viewport(cmd_buffer, firstViewport, viewportCount, pViewports, vp_xform);
}

VKAPI_ATTR void VKAPI_CALL
radv_CmdSetScissor(VkCommandBuffer commandBuffer, uint32_t firstScissor, uint32_t scissorCount,
                   const VkRect2D *pScissors)
{
   VK_FROM_HANDLE(radv_cmd_buffer, cmd_buffer, commandBuffer);
   radv_cmd_set_scissor(cmd_buffer, firstScissor, scissorCount, pScissors);
}

VKAPI_ATTR void VKAPI_CALL
radv_CmdSetLineWidth(VkCommandBuffer commandBuffer, float lineWidth)
{
   VK_FROM_HANDLE(radv_cmd_buffer, cmd_buffer, commandBuffer);
   radv_cmd_set_line_width(cmd_buffer, lineWidth);
}

VKAPI_ATTR void VKAPI_CALL
radv_CmdSetBlendConstants(VkCommandBuffer commandBuffer, const float blendConstants[4])
{
   VK_FROM_HANDLE(radv_cmd_buffer, cmd_buffer, commandBuffer);
   radv_cmd_set_blend_constants(cmd_buffer, blendConstants);
}

VKAPI_ATTR void VKAPI_CALL
radv_CmdSetDepthBounds(VkCommandBuffer commandBuffer, float minDepthBounds, float maxDepthBounds)
{
   VK_FROM_HANDLE(radv_cmd_buffer, cmd_buffer, commandBuffer);
   radv_cmd_set_depth_bounds(cmd_buffer, minDepthBounds, maxDepthBounds);
}

VKAPI_ATTR void VKAPI_CALL
radv_CmdSetStencilCompareMask(VkCommandBuffer commandBuffer, VkStencilFaceFlags faceMask, uint32_t compareMask)
{
   VK_FROM_HANDLE(radv_cmd_buffer, cmd_buffer, commandBuffer);
   radv_cmd_set_stencil_compare_mask(cmd_buffer, faceMask, compareMask);
}

VKAPI_ATTR void VKAPI_CALL
radv_CmdSetStencilWriteMask(VkCommandBuffer commandBuffer, VkStencilFaceFlags faceMask, uint32_t writeMask)
{
   VK_FROM_HANDLE(radv_cmd_buffer, cmd_buffer, commandBuffer);
   radv_cmd_set_stencil_write_mask(cmd_buffer, faceMask, writeMask);
}

VKAPI_ATTR void VKAPI_CALL
radv_CmdSetStencilReference(VkCommandBuffer commandBuffer, VkStencilFaceFlags faceMask, uint32_t reference)
{
   VK_FROM_HANDLE(radv_cmd_buffer, cmd_buffer, commandBuffer);
   radv_cmd_set_stencil_reference(cmd_buffer, faceMask, reference);
}

VKAPI_ATTR void VKAPI_CALL
radv_CmdSetDiscardRectangleEXT(VkCommandBuffer commandBuffer, uint32_t firstDiscardRectangle,
                               uint32_t discardRectangleCount, const VkRect2D *pDiscardRectangles)
{
   VK_FROM_HANDLE(radv_cmd_buffer, cmd_buffer, commandBuffer);
   radv_cmd_set_discard_rectangle(cmd_buffer, firstDiscardRectangle, discardRectangleCount, pDiscardRectangles);
}

VKAPI_ATTR void VKAPI_CALL
radv_CmdSetSampleLocationsEXT(VkCommandBuffer commandBuffer, const VkSampleLocationsInfoEXT *pSampleLocationsInfo)
{
   VK_FROM_HANDLE(radv_cmd_buffer, cmd_buffer, commandBuffer);
   radv_cmd_set_sample_locations(cmd_buffer, pSampleLocationsInfo->sampleLocationsPerPixel,
                                 pSampleLocationsInfo->sampleLocationGridSize,
                                 pSampleLocationsInfo->sampleLocationsCount, pSampleLocationsInfo->pSampleLocations);
}

VKAPI_ATTR void VKAPI_CALL
radv_CmdSetLineStipple(VkCommandBuffer commandBuffer, uint32_t lineStippleFactor, uint16_t lineStipplePattern)
{
   VK_FROM_HANDLE(radv_cmd_buffer, cmd_buffer, commandBuffer);
   radv_cmd_set_line_stipple(cmd_buffer, lineStippleFactor, lineStipplePattern);
}

VKAPI_ATTR void VKAPI_CALL
radv_CmdSetCullMode(VkCommandBuffer commandBuffer, VkCullModeFlags cullMode)
{
   VK_FROM_HANDLE(radv_cmd_buffer, cmd_buffer, commandBuffer);
   radv_cmd_set_cull_mode(cmd_buffer, cullMode);
}

VKAPI_ATTR void VKAPI_CALL
radv_CmdSetFrontFace(VkCommandBuffer commandBuffer, VkFrontFace frontFace)
{
   VK_FROM_HANDLE(radv_cmd_buffer, cmd_buffer, commandBuffer);
   radv_cmd_set_front_face(cmd_buffer, frontFace);
}

VKAPI_ATTR void VKAPI_CALL
radv_CmdSetPrimitiveTopology(VkCommandBuffer commandBuffer, VkPrimitiveTopology primitiveTopology)
{
   VK_FROM_HANDLE(radv_cmd_buffer, cmd_buffer, commandBuffer);
   radv_cmd_set_primitive_topology(cmd_buffer, radv_translate_prim(primitiveTopology));
}

VKAPI_ATTR void VKAPI_CALL
radv_CmdSetViewportWithCount(VkCommandBuffer commandBuffer, uint32_t viewportCount, const VkViewport *pViewports)
{
   VK_FROM_HANDLE(radv_cmd_buffer, cmd_buffer, commandBuffer);

   radv_cmd_set_viewport_with_count(cmd_buffer, viewportCount);

   radv_CmdSetViewport(commandBuffer, 0, viewportCount, pViewports);
}

VKAPI_ATTR void VKAPI_CALL
radv_CmdSetScissorWithCount(VkCommandBuffer commandBuffer, uint32_t scissorCount, const VkRect2D *pScissors)
{
   VK_FROM_HANDLE(radv_cmd_buffer, cmd_buffer, commandBuffer);

   radv_cmd_set_scissor_with_count(cmd_buffer, scissorCount);

   radv_CmdSetScissor(commandBuffer, 0, scissorCount, pScissors);
}

VKAPI_ATTR void VKAPI_CALL
radv_CmdSetDepthTestEnable(VkCommandBuffer commandBuffer, VkBool32 depthTestEnable)
{
   VK_FROM_HANDLE(radv_cmd_buffer, cmd_buffer, commandBuffer);
   radv_cmd_set_depth_test_enable(cmd_buffer, depthTestEnable);
}

VKAPI_ATTR void VKAPI_CALL
radv_CmdSetDepthWriteEnable(VkCommandBuffer commandBuffer, VkBool32 depthWriteEnable)
{
   VK_FROM_HANDLE(radv_cmd_buffer, cmd_buffer, commandBuffer);
   radv_cmd_set_depth_write_enable(cmd_buffer, depthWriteEnable);
}

VKAPI_ATTR void VKAPI_CALL
radv_CmdSetDepthCompareOp(VkCommandBuffer commandBuffer, VkCompareOp depthCompareOp)
{
   VK_FROM_HANDLE(radv_cmd_buffer, cmd_buffer, commandBuffer);
   radv_cmd_set_depth_compare_op(cmd_buffer, depthCompareOp);
}

VKAPI_ATTR void VKAPI_CALL
radv_CmdSetDepthBoundsTestEnable(VkCommandBuffer commandBuffer, VkBool32 depthBoundsTestEnable)
{
   VK_FROM_HANDLE(radv_cmd_buffer, cmd_buffer, commandBuffer);
   radv_cmd_set_depth_bounds_test_enable(cmd_buffer, depthBoundsTestEnable);
}

VKAPI_ATTR void VKAPI_CALL
radv_CmdSetStencilTestEnable(VkCommandBuffer commandBuffer, VkBool32 stencilTestEnable)
{
   VK_FROM_HANDLE(radv_cmd_buffer, cmd_buffer, commandBuffer);
   radv_cmd_set_stencil_test_enable(cmd_buffer, stencilTestEnable);
}

VKAPI_ATTR void VKAPI_CALL
radv_CmdSetStencilOp(VkCommandBuffer commandBuffer, VkStencilFaceFlags faceMask, VkStencilOp failOp, VkStencilOp passOp,
                     VkStencilOp depthFailOp, VkCompareOp compareOp)
{
   VK_FROM_HANDLE(radv_cmd_buffer, cmd_buffer, commandBuffer);
   radv_cmd_set_stencil_op(cmd_buffer, faceMask, radv_translate_stencil_op(failOp), radv_translate_stencil_op(passOp),
                           radv_translate_stencil_op(depthFailOp), compareOp);
}

VKAPI_ATTR void VKAPI_CALL
radv_CmdSetFragmentShadingRateKHR(VkCommandBuffer commandBuffer, const VkExtent2D *pFragmentSize,
                                  const VkFragmentShadingRateCombinerOpKHR combinerOps[2])
{
   VK_FROM_HANDLE(radv_cmd_buffer, cmd_buffer, commandBuffer);
   radv_cmd_set_fragment_shading_rate(cmd_buffer, pFragmentSize, combinerOps);
}

VKAPI_ATTR void VKAPI_CALL
radv_CmdSetDepthBiasEnable(VkCommandBuffer commandBuffer, VkBool32 depthBiasEnable)
{
   VK_FROM_HANDLE(radv_cmd_buffer, cmd_buffer, commandBuffer);
   radv_cmd_set_depth_bias_enable(cmd_buffer, depthBiasEnable);
}

VKAPI_ATTR void VKAPI_CALL
radv_CmdSetPrimitiveRestartEnable(VkCommandBuffer commandBuffer, VkBool32 primitiveRestartEnable)
{
   VK_FROM_HANDLE(radv_cmd_buffer, cmd_buffer, commandBuffer);
   radv_cmd_set_primitive_restart_enable(cmd_buffer, primitiveRestartEnable);
}

VKAPI_ATTR void VKAPI_CALL
radv_CmdSetRasterizerDiscardEnable(VkCommandBuffer commandBuffer, VkBool32 rasterizerDiscardEnable)
{
   VK_FROM_HANDLE(radv_cmd_buffer, cmd_buffer, commandBuffer);
   radv_cmd_set_rasterizer_discard_enable(cmd_buffer, rasterizerDiscardEnable);
}

VKAPI_ATTR void VKAPI_CALL
radv_CmdSetPatchControlPointsEXT(VkCommandBuffer commandBuffer, uint32_t patchControlPoints)
{
   VK_FROM_HANDLE(radv_cmd_buffer, cmd_buffer, commandBuffer);
   radv_cmd_set_patch_control_points(cmd_buffer, patchControlPoints);
}

VKAPI_ATTR void VKAPI_CALL
radv_CmdSetLogicOpEXT(VkCommandBuffer commandBuffer, VkLogicOp logicOp)
{
   VK_FROM_HANDLE(radv_cmd_buffer, cmd_buffer, commandBuffer);
   radv_cmd_set_logic_op(cmd_buffer, radv_translate_blend_logic_op(logicOp));
}

VKAPI_ATTR void VKAPI_CALL
radv_CmdSetColorWriteEnableEXT(VkCommandBuffer commandBuffer, uint32_t attachmentCount,
                               const VkBool32 *pColorWriteEnables)
{
   VK_FROM_HANDLE(radv_cmd_buffer, cmd_buffer, commandBuffer);
   uint32_t color_write_enable = 0;

   assert(attachmentCount <= MAX_RTS);

   for (uint32_t i = 0; i < attachmentCount; i++) {
      if (pColorWriteEnables[i]) {
         color_write_enable |= BITFIELD_RANGE(i * 4, 4);
      }
   }

   radv_cmd_set_color_write_enable(cmd_buffer, color_write_enable);
}

VKAPI_ATTR void VKAPI_CALL
radv_CmdSetVertexInputEXT(VkCommandBuffer commandBuffer, uint32_t vertexBindingDescriptionCount,
                          const VkVertexInputBindingDescription2EXT *pVertexBindingDescriptions,
                          uint32_t vertexAttributeDescriptionCount,
                          const VkVertexInputAttributeDescription2EXT *pVertexAttributeDescriptions)
{
   VK_FROM_HANDLE(radv_cmd_buffer, cmd_buffer, commandBuffer);
   struct radv_device *device = radv_cmd_buffer_device(cmd_buffer);
   const struct radv_physical_device *pdev = radv_device_physical(device);
   const struct radv_cmd_state *state = &cmd_buffer->state;
   struct radv_vertex_input_state vertex_input = cmd_buffer->state.dynamic.vertex_input;

   const VkVertexInputBindingDescription2EXT *bindings[MAX_VBS];
   for (unsigned i = 0; i < vertexBindingDescriptionCount; i++)
      bindings[pVertexBindingDescriptions[i].binding] = &pVertexBindingDescriptions[i];

   vertex_input.vbo_misaligned_mask = 0;
   vertex_input.vbo_unaligned_mask = 0;
   vertex_input.vbo_misaligned_mask_invalid = 0;
   vertex_input.attribute_mask = 0;
   vertex_input.instance_rate_inputs = 0;
   vertex_input.nontrivial_divisors = 0;
   vertex_input.zero_divisors = 0;
   vertex_input.post_shuffle = 0;
   vertex_input.alpha_adjust_lo = 0;
   vertex_input.alpha_adjust_hi = 0;
   vertex_input.nontrivial_formats = 0;
   vertex_input.bindings_match_attrib = true;

   enum amd_gfx_level chip = pdev->info.gfx_level;
   enum radeon_family family = pdev->info.family;
   const struct ac_vtx_format_info *vtx_info_table = ac_get_vtx_format_info_table(chip, family);

   for (unsigned i = 0; i < vertexAttributeDescriptionCount; i++) {
      const VkVertexInputAttributeDescription2EXT *attrib = &pVertexAttributeDescriptions[i];
      const VkVertexInputBindingDescription2EXT *binding = bindings[attrib->binding];
      unsigned loc = attrib->location;

      vertex_input.attribute_mask |= 1u << loc;
      vertex_input.bindings[loc] = attrib->binding;
      if (attrib->binding != loc)
         vertex_input.bindings_match_attrib = false;
      if (binding->inputRate == VK_VERTEX_INPUT_RATE_INSTANCE) {
         vertex_input.instance_rate_inputs |= 1u << loc;
         vertex_input.divisors[loc] = binding->divisor;
         if (binding->divisor == 0) {
            vertex_input.zero_divisors |= 1u << loc;
         } else if (binding->divisor > 1) {
            vertex_input.nontrivial_divisors |= 1u << loc;
         }
      }

      radv_cmd_set_vertex_binding_strides(cmd_buffer, attrib->binding, 1, (uint16_t *)&binding->stride);
      vertex_input.offsets[loc] = attrib->offset;

      enum pipe_format format = vk_format_map[attrib->format];
      const struct ac_vtx_format_info *vtx_info = &vtx_info_table[format];

      vertex_input.formats[loc] = format;
      uint8_t format_align_req_minus_1 = vtx_info->chan_byte_size >= 4 ? 3 : (vtx_info->element_size - 1);
      vertex_input.format_align_req_minus_1[loc] = format_align_req_minus_1;
      uint8_t component_align_req_minus_1 =
         MIN2(vtx_info->chan_byte_size ? vtx_info->chan_byte_size : vtx_info->element_size, 4) - 1;
      vertex_input.component_align_req_minus_1[loc] = component_align_req_minus_1;
      vertex_input.format_sizes[loc] = vtx_info->element_size;
      vertex_input.alpha_adjust_lo |= (vtx_info->alpha_adjust & 0x1) << loc;
      vertex_input.alpha_adjust_hi |= (vtx_info->alpha_adjust >> 1) << loc;
      if (G_008F0C_DST_SEL_X(vtx_info->dst_sel) == V_008F0C_SQ_SEL_Z)
         vertex_input.post_shuffle |= BITFIELD_BIT(loc);

      if (vtx_info->has_hw_format & BITFIELD_BIT(vtx_info->num_channels - 1)) {
         const uint32_t hw_format = vtx_info->hw_format[vtx_info->num_channels - 1];

         if (pdev->info.gfx_level >= GFX10) {
            vertex_input.non_trivial_format[loc] = vtx_info->dst_sel | S_008F0C_FORMAT_GFX10(hw_format);
         } else {
            vertex_input.non_trivial_format[loc] =
               vtx_info->dst_sel | S_008F0C_NUM_FORMAT((hw_format >> 4) & 0x7) | S_008F0C_DATA_FORMAT(hw_format & 0xf);
         }
      } else {
         vertex_input.non_trivial_format[loc] = 0;
         vertex_input.nontrivial_formats |= BITFIELD_BIT(loc);
      }

      if (state->vbo_bound_mask & BITFIELD_BIT(attrib->binding)) {
         uint32_t stride = binding->stride;
         uint64_t addr = cmd_buffer->vertex_bindings[attrib->binding].addr + vertex_input.offsets[loc];
         if ((chip == GFX6 || chip >= GFX10) && ((stride | addr) & format_align_req_minus_1))
            vertex_input.vbo_misaligned_mask |= BITFIELD_BIT(loc);
         if ((stride | addr) & component_align_req_minus_1)
            vertex_input.vbo_unaligned_mask |= BITFIELD_BIT(loc);
      }
   }

   radv_cmd_set_vertex_input(cmd_buffer, &vertex_input);
}

VKAPI_ATTR void VKAPI_CALL
radv_CmdSetPolygonModeEXT(VkCommandBuffer commandBuffer, VkPolygonMode polygonMode)
{
   VK_FROM_HANDLE(radv_cmd_buffer, cmd_buffer, commandBuffer);
   radv_cmd_set_polygon_mode(cmd_buffer, radv_translate_fill(polygonMode));
}

VKAPI_ATTR void VKAPI_CALL
radv_CmdSetTessellationDomainOriginEXT(VkCommandBuffer commandBuffer, VkTessellationDomainOrigin domainOrigin)
{
   VK_FROM_HANDLE(radv_cmd_buffer, cmd_buffer, commandBuffer);
   radv_cmd_set_tessellation_domain_origin(cmd_buffer, domainOrigin);
}

VKAPI_ATTR void VKAPI_CALL
radv_CmdSetLogicOpEnableEXT(VkCommandBuffer commandBuffer, VkBool32 logicOpEnable)
{
   VK_FROM_HANDLE(radv_cmd_buffer, cmd_buffer, commandBuffer);
   radv_cmd_set_logic_op_enable(cmd_buffer, logicOpEnable);
}

VKAPI_ATTR void VKAPI_CALL
radv_CmdSetLineStippleEnableEXT(VkCommandBuffer commandBuffer, VkBool32 stippledLineEnable)
{
   VK_FROM_HANDLE(radv_cmd_buffer, cmd_buffer, commandBuffer);
   radv_cmd_set_line_stipple_enable(cmd_buffer, stippledLineEnable);
}

VKAPI_ATTR void VKAPI_CALL
radv_CmdSetAlphaToCoverageEnableEXT(VkCommandBuffer commandBuffer, VkBool32 alphaToCoverageEnable)
{
   VK_FROM_HANDLE(radv_cmd_buffer, cmd_buffer, commandBuffer);
   radv_cmd_set_alpha_to_coverage_enable(cmd_buffer, alphaToCoverageEnable);
}

VKAPI_ATTR void VKAPI_CALL
radv_CmdSetAlphaToOneEnableEXT(VkCommandBuffer commandBuffer, VkBool32 alphaToOneEnable)
{
   VK_FROM_HANDLE(radv_cmd_buffer, cmd_buffer, commandBuffer);
   radv_cmd_set_alpha_to_one_enable(cmd_buffer, alphaToOneEnable);
}

VKAPI_ATTR void VKAPI_CALL
radv_CmdSetSampleMaskEXT(VkCommandBuffer commandBuffer, VkSampleCountFlagBits samples, const VkSampleMask *pSampleMask)
{
   VK_FROM_HANDLE(radv_cmd_buffer, cmd_buffer, commandBuffer);
   radv_cmd_set_sample_mask(cmd_buffer, pSampleMask[0] & 0xffff);
}

VKAPI_ATTR void VKAPI_CALL
radv_CmdSetDepthClipEnableEXT(VkCommandBuffer commandBuffer, VkBool32 depthClipEnable)
{
   VK_FROM_HANDLE(radv_cmd_buffer, cmd_buffer, commandBuffer);
   radv_cmd_set_depth_clip_enable(cmd_buffer,
                                  depthClipEnable ? VK_MESA_DEPTH_CLIP_ENABLE_TRUE : VK_MESA_DEPTH_CLIP_ENABLE_FALSE);
}

VKAPI_ATTR void VKAPI_CALL
radv_CmdSetConservativeRasterizationModeEXT(VkCommandBuffer commandBuffer,
                                            VkConservativeRasterizationModeEXT conservativeRasterizationMode)
{
   VK_FROM_HANDLE(radv_cmd_buffer, cmd_buffer, commandBuffer);
   radv_cmd_set_conservative_rasterization_mode(cmd_buffer, conservativeRasterizationMode);
}

VKAPI_ATTR void VKAPI_CALL
radv_CmdSetDepthClipNegativeOneToOneEXT(VkCommandBuffer commandBuffer, VkBool32 negativeOneToOne)
{
   VK_FROM_HANDLE(radv_cmd_buffer, cmd_buffer, commandBuffer);
   radv_cmd_set_depth_clip_negative_one_to_one(cmd_buffer, negativeOneToOne);
}

VKAPI_ATTR void VKAPI_CALL
radv_CmdSetProvokingVertexModeEXT(VkCommandBuffer commandBuffer, VkProvokingVertexModeEXT provokingVertexMode)
{
   VK_FROM_HANDLE(radv_cmd_buffer, cmd_buffer, commandBuffer);
   radv_cmd_set_provoking_vertex_mode(cmd_buffer, provokingVertexMode);
}

VKAPI_ATTR void VKAPI_CALL
radv_CmdSetDepthClampEnableEXT(VkCommandBuffer commandBuffer, VkBool32 depthClampEnable)
{
   VK_FROM_HANDLE(radv_cmd_buffer, cmd_buffer, commandBuffer);
   radv_cmd_set_depth_clamp_enable(cmd_buffer, depthClampEnable);
}

VKAPI_ATTR void VKAPI_CALL
radv_CmdSetColorWriteMaskEXT(VkCommandBuffer commandBuffer, uint32_t firstAttachment, uint32_t attachmentCount,
                             const VkColorComponentFlags *pColorWriteMasks)
{
   VK_FROM_HANDLE(radv_cmd_buffer, cmd_buffer, commandBuffer);
   struct radv_cmd_state *state = &cmd_buffer->state;
   uint32_t color_write_mask = state->dynamic.color_write_mask;

   assert(firstAttachment + attachmentCount <= MAX_RTS);

   for (uint32_t i = 0; i < attachmentCount; i++) {
      uint32_t idx = firstAttachment + i;

      color_write_mask &= ~BITFIELD_RANGE(4 * idx, 4);
      color_write_mask |= pColorWriteMasks[i] << (4 * idx);
   }

   radv_cmd_set_color_write_mask(cmd_buffer, color_write_mask);
}

VKAPI_ATTR void VKAPI_CALL
radv_CmdSetColorBlendEnableEXT(VkCommandBuffer commandBuffer, uint32_t firstAttachment, uint32_t attachmentCount,
                               const VkBool32 *pColorBlendEnables)
{
   VK_FROM_HANDLE(radv_cmd_buffer, cmd_buffer, commandBuffer);
   struct radv_cmd_state *state = &cmd_buffer->state;
   uint32_t color_blend_enable = state->dynamic.color_blend_enable;

   assert(firstAttachment + attachmentCount <= MAX_RTS);

   for (uint32_t i = 0; i < attachmentCount; i++) {
      uint32_t idx = firstAttachment + i;

      color_blend_enable &= ~BITFIELD_RANGE(idx, 1);
      color_blend_enable |= pColorBlendEnables[i] << idx;
   }

   radv_cmd_set_color_blend_enable(cmd_buffer, color_blend_enable);
}

VKAPI_ATTR void VKAPI_CALL
radv_CmdSetRasterizationSamplesEXT(VkCommandBuffer commandBuffer, VkSampleCountFlagBits rasterizationSamples)
{
   VK_FROM_HANDLE(radv_cmd_buffer, cmd_buffer, commandBuffer);
   radv_cmd_set_rasterization_samples(cmd_buffer, rasterizationSamples);
}

VKAPI_ATTR void VKAPI_CALL
radv_CmdSetLineRasterizationModeEXT(VkCommandBuffer commandBuffer, VkLineRasterizationMode lineRasterizationMode)
{
   VK_FROM_HANDLE(radv_cmd_buffer, cmd_buffer, commandBuffer);
   radv_cmd_set_line_rasterization_mode(cmd_buffer, lineRasterizationMode);
}

VKAPI_ATTR void VKAPI_CALL
radv_CmdSetColorBlendEquationEXT(VkCommandBuffer commandBuffer, uint32_t firstAttachment, uint32_t attachmentCount,
                                 const VkColorBlendEquationEXT *pColorBlendEquations)
{
   VK_FROM_HANDLE(radv_cmd_buffer, cmd_buffer, commandBuffer);
   const struct radv_device *device = radv_cmd_buffer_device(cmd_buffer);
   const struct radv_physical_device *pdev = radv_device_physical(device);
   struct radv_blend_equation_state blend_eq;

   for (uint32_t i = 0; i < attachmentCount; i++) {
      radv_translate_blend_equation(
         pdev, pColorBlendEquations[i].colorBlendOp, pColorBlendEquations[i].srcColorBlendFactor,
         pColorBlendEquations[i].dstColorBlendFactor, pColorBlendEquations[i].alphaBlendOp,
         pColorBlendEquations[i].srcAlphaBlendFactor, pColorBlendEquations[i].dstAlphaBlendFactor,
         &blend_eq.att[i].cb_blend_control, &blend_eq.att[i].sx_mrt_blend_opt);
   }

   if (firstAttachment == 0) {
      const struct vk_color_blend_attachment_state blend_att = {
         .color_blend_op = pColorBlendEquations[0].colorBlendOp,
         .src_color_blend_factor = pColorBlendEquations[0].srcColorBlendFactor,
         .dst_color_blend_factor = pColorBlendEquations[0].dstColorBlendFactor,
         .alpha_blend_op = pColorBlendEquations[0].alphaBlendOp,
         .src_alpha_blend_factor = pColorBlendEquations[0].srcAlphaBlendFactor,
         .dst_alpha_blend_factor = pColorBlendEquations[0].dstAlphaBlendFactor,
      };

      blend_eq.mrt0_is_dual_src = radv_can_enable_dual_src(&blend_att);
   }

   radv_cmd_set_color_blend_equation(cmd_buffer, firstAttachment, attachmentCount, &blend_eq);
}

VKAPI_ATTR void VKAPI_CALL
radv_CmdSetSampleLocationsEnableEXT(VkCommandBuffer commandBuffer, VkBool32 sampleLocationsEnable)
{
   VK_FROM_HANDLE(radv_cmd_buffer, cmd_buffer, commandBuffer);
   radv_cmd_set_sample_locations_enable(cmd_buffer, sampleLocationsEnable);
}

VKAPI_ATTR void VKAPI_CALL
radv_CmdSetDiscardRectangleEnableEXT(VkCommandBuffer commandBuffer, VkBool32 discardRectangleEnable)
{
   VK_FROM_HANDLE(radv_cmd_buffer, cmd_buffer, commandBuffer);
   struct radv_cmd_state *state = &cmd_buffer->state;

   /* Special case to allow setting the number of rectangles dynamically. */
   state->dynamic.vk.dr.rectangle_count = discardRectangleEnable ? MAX_DISCARD_RECTANGLES : 0;

   radv_cmd_set_discard_rectangle_enable(cmd_buffer, discardRectangleEnable);
}

VKAPI_ATTR void VKAPI_CALL
radv_CmdSetDiscardRectangleModeEXT(VkCommandBuffer commandBuffer, VkDiscardRectangleModeEXT discardRectangleMode)
{
   VK_FROM_HANDLE(radv_cmd_buffer, cmd_buffer, commandBuffer);
   radv_cmd_set_discard_rectangle_mode(cmd_buffer, discardRectangleMode);
}

VKAPI_ATTR void VKAPI_CALL
radv_CmdSetAttachmentFeedbackLoopEnableEXT(VkCommandBuffer commandBuffer, VkImageAspectFlags aspectMask)
{
   VK_FROM_HANDLE(radv_cmd_buffer, cmd_buffer, commandBuffer);
   radv_cmd_set_attachment_feedback_loop_enable(cmd_buffer, aspectMask);
}

VKAPI_ATTR void VKAPI_CALL
radv_CmdSetDepthBias2EXT(VkCommandBuffer commandBuffer, const VkDepthBiasInfoEXT *pDepthBiasInfo)
{
   VK_FROM_HANDLE(radv_cmd_buffer, cmd_buffer, commandBuffer);

   const VkDepthBiasRepresentationInfoEXT *dbr_info =
      vk_find_struct_const(pDepthBiasInfo->pNext, DEPTH_BIAS_REPRESENTATION_INFO_EXT);

   const struct radv_cmd_set_depth_bias_info info = {
      .constant_factor = pDepthBiasInfo->depthBiasConstantFactor,
      .clamp = pDepthBiasInfo->depthBiasClamp,
      .slope_factor = pDepthBiasInfo->depthBiasSlopeFactor,
      .representation = dbr_info ? dbr_info->depthBiasRepresentation
                                 : VK_DEPTH_BIAS_REPRESENTATION_LEAST_REPRESENTABLE_VALUE_FORMAT_EXT,
   };

   radv_cmd_set_depth_bias(cmd_buffer, &info);
}

VKAPI_ATTR void VKAPI_CALL
radv_CmdSetRenderingAttachmentLocations(VkCommandBuffer commandBuffer,
                                        const VkRenderingAttachmentLocationInfo *pLocationInfo)
{
   VK_FROM_HANDLE(radv_cmd_buffer, cmd_buffer, commandBuffer);
   uint8_t color_map[MAX_RTS];

   assume(pLocationInfo->colorAttachmentCount <= MESA_VK_MAX_COLOR_ATTACHMENTS);
   for (uint32_t i = 0; i < pLocationInfo->colorAttachmentCount; i++) {
      uint8_t val;

      if (!pLocationInfo->pColorAttachmentLocations) {
         val = i;
      } else if (pLocationInfo->pColorAttachmentLocations[i] == VK_ATTACHMENT_UNUSED) {
         val = MESA_VK_ATTACHMENT_UNUSED;
      } else {
         val = pLocationInfo->pColorAttachmentLocations[i];
      }

      color_map[i] = val;
   }

   radv_cmd_set_rendering_attachment_locations(cmd_buffer, pLocationInfo->colorAttachmentCount, color_map);
}

VKAPI_ATTR void VKAPI_CALL
radv_CmdSetRenderingInputAttachmentIndices(VkCommandBuffer commandBuffer,
                                           const VkRenderingInputAttachmentIndexInfo *pLocationInfo)
{
   VK_FROM_HANDLE(radv_cmd_buffer, cmd_buffer, commandBuffer);
   uint8_t depth_att, stencil_att;
   uint8_t color_map[MAX_RTS];

   assume(pLocationInfo->colorAttachmentCount <= MESA_VK_MAX_COLOR_ATTACHMENTS);
   for (uint32_t i = 0; i < pLocationInfo->colorAttachmentCount; i++) {
      uint8_t val;

      if (!pLocationInfo->pColorAttachmentInputIndices) {
         val = i;
      } else if (pLocationInfo->pColorAttachmentInputIndices[i] == VK_ATTACHMENT_UNUSED) {
         val = MESA_VK_ATTACHMENT_UNUSED;
      } else {
         val = pLocationInfo->pColorAttachmentInputIndices[i];
      }

      color_map[i] = val;
   }

   depth_att = (pLocationInfo->pDepthInputAttachmentIndex == NULL ||
                *pLocationInfo->pDepthInputAttachmentIndex == VK_ATTACHMENT_UNUSED)
                  ? MESA_VK_ATTACHMENT_UNUSED
                  : *pLocationInfo->pDepthInputAttachmentIndex;
   stencil_att = (pLocationInfo->pStencilInputAttachmentIndex == NULL ||
                  *pLocationInfo->pStencilInputAttachmentIndex == VK_ATTACHMENT_UNUSED)
                    ? MESA_VK_ATTACHMENT_UNUSED
                    : *pLocationInfo->pStencilInputAttachmentIndex;

   radv_cmd_set_rendering_input_attachment_indices(cmd_buffer, pLocationInfo->colorAttachmentCount, color_map,
                                                   depth_att, stencil_att);
}

static void
radv_handle_color_fbfetch_output(struct radv_cmd_buffer *cmd_buffer, uint32_t index)
{
   const struct radv_device *device = radv_cmd_buffer_device(cmd_buffer);
   struct radv_rendering_state *render = &cmd_buffer->state.render;
   const struct radv_dynamic_state *d = &cmd_buffer->state.dynamic;
   struct radv_attachment *att = &render->color_att[index];

   if (!att->iview)
      return;

   const struct radv_image *image = att->iview->image;
   if (!(image->vk.usage & VK_IMAGE_USAGE_INPUT_ATTACHMENT_BIT))
      return;

   const uint32_t queue_mask = radv_image_queue_family_mask(att->iview->image, cmd_buffer->qf, cmd_buffer->qf);
   const bool is_dcc_compressed =
      radv_layout_dcc_compressed(device, image, att->iview->vk.base_mip_level, att->layout, queue_mask);
   const enum radv_fmask_compression fmask_comp = radv_layout_fmask_compression(device, image, att->layout, queue_mask);

   if (!is_dcc_compressed && fmask_comp == RADV_FMASK_COMPRESSION_NONE)
      return;

   const uint32_t color_att_idx = d->vk.cal.color_map[index];
   if (color_att_idx == MESA_VK_ATTACHMENT_UNUSED)
      return;

   if (d->vk.ial.color_map[color_att_idx] != color_att_idx)
      return;

   const VkImageSubresourceRange range = vk_image_view_subresource_range(&att->iview->vk);

   /* Consider previous rendering work for WAW hazards. */
   cmd_buffer->state.flush_bits |=
      radv_src_access_flush(cmd_buffer, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT, 0,
                            att->iview->image, &range);

   radv_describe_barrier_start(cmd_buffer, RGP_BARRIER_UNKNOWN_REASON);

   /* Force a transition to FEEDBACK_LOOP_OPTIMAL to decompress DCC. */
   radv_handle_image_transition(cmd_buffer, att->iview->image, att->layout,
                                VK_IMAGE_LAYOUT_ATTACHMENT_FEEDBACK_LOOP_OPTIMAL_EXT, RADV_QUEUE_GENERAL,
                                RADV_QUEUE_GENERAL, &range, NULL);

   radv_describe_barrier_end(cmd_buffer);

   att->layout = VK_IMAGE_LAYOUT_ATTACHMENT_FEEDBACK_LOOP_OPTIMAL_EXT;

   cmd_buffer->state.flush_bits |= radv_dst_access_flush(
      cmd_buffer, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
      VK_ACCESS_2_INPUT_ATTACHMENT_READ_BIT | VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT, 0, att->iview->image, &range);

   cmd_buffer->state.dirty |= RADV_CMD_DIRTY_FRAMEBUFFER;
}

static void
radv_handle_depth_fbfetch_output(struct radv_cmd_buffer *cmd_buffer)
{
   const struct radv_device *device = radv_cmd_buffer_device(cmd_buffer);
   struct radv_rendering_state *render = &cmd_buffer->state.render;
   const struct radv_dynamic_state *d = &cmd_buffer->state.dynamic;
   struct radv_attachment *att = &render->ds_att;

   if (!att->iview)
      return;

   const struct radv_image *image = att->iview->image;
   if (!(image->vk.usage & VK_IMAGE_USAGE_INPUT_ATTACHMENT_BIT))
      return;

   if (!radv_layout_is_htile_compressed(
          device, att->iview->image, att->iview->vk.base_mip_level, att->layout,
          radv_image_queue_family_mask(att->iview->image, cmd_buffer->qf, cmd_buffer->qf)))
      return;

   if (d->vk.ial.depth_att == MESA_VK_ATTACHMENT_UNUSED && d->vk.ial.stencil_att == MESA_VK_ATTACHMENT_UNUSED)
      return;

   const VkImageSubresourceRange range = vk_image_view_subresource_range(&att->iview->vk);

   /* Consider previous rendering work for WAW hazards. */
   cmd_buffer->state.flush_bits |=
      radv_src_access_flush(cmd_buffer, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                            VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT, 0, att->iview->image, &range);

   radv_describe_barrier_start(cmd_buffer, RGP_BARRIER_UNKNOWN_REASON);

   /* Force a transition to FEEDBACK_LOOP_OPTIMAL to decompress HTILE. */
   radv_handle_image_transition(cmd_buffer, att->iview->image, att->layout,
                                VK_IMAGE_LAYOUT_ATTACHMENT_FEEDBACK_LOOP_OPTIMAL_EXT, RADV_QUEUE_GENERAL,
                                RADV_QUEUE_GENERAL, &range, NULL);

   radv_describe_barrier_end(cmd_buffer);

   att->layout = VK_IMAGE_LAYOUT_ATTACHMENT_FEEDBACK_LOOP_OPTIMAL_EXT;
   att->stencil_layout = VK_IMAGE_LAYOUT_ATTACHMENT_FEEDBACK_LOOP_OPTIMAL_EXT;

   cmd_buffer->state.flush_bits |=
      radv_dst_access_flush(cmd_buffer, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                            VK_ACCESS_2_INPUT_ATTACHMENT_READ_BIT | VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT, 0,
                            att->iview->image, &range);

   cmd_buffer->state.dirty |= RADV_CMD_DIRTY_FRAMEBUFFER;
}

static void
radv_handle_fbfetch_output(struct radv_cmd_buffer *cmd_buffer)
{
   const struct radv_rendering_state *render = &cmd_buffer->state.render;

   /* Nothing to do when dynamic rendering doesn't use concurrent input attachment writes. */
   if (render->has_input_attachment_no_concurrent_writes)
      return;

   /* Nothing to do when the bound fragment shader doesn't use subpass input attachments. */
   if (!cmd_buffer->state.uses_fbfetch_output)
      return;

   /* Check if any color attachments are compressed and also used as input attachments. */
   for (uint32_t i = 0; i < render->color_att_count; i++) {
      radv_handle_color_fbfetch_output(cmd_buffer, i);
   }

   /* Check if the depth/stencil attachment is compressed and also used as input attachment. */
   radv_handle_depth_fbfetch_output(cmd_buffer);
}

VKAPI_ATTR void VKAPI_CALL
radv_CmdExecuteCommands(VkCommandBuffer commandBuffer, uint32_t commandBufferCount, const VkCommandBuffer *pCmdBuffers)
{
   VK_FROM_HANDLE(radv_cmd_buffer, primary, commandBuffer);
   struct radv_device *device = radv_cmd_buffer_device(primary);
   const struct radv_physical_device *pdev = radv_device_physical(device);

   assert(commandBufferCount > 0);

   radv_emit_mip_change_flush_default(primary);

   /* Emit pending flushes on primary prior to executing secondary */
   radv_emit_cache_flush(primary);

   /* Make sure CP DMA is idle on primary prior to executing secondary. */
   radv_cp_dma_wait_for_idle(primary);

   for (uint32_t i = 0; i < commandBufferCount; i++) {
      VK_FROM_HANDLE(radv_cmd_buffer, secondary, pCmdBuffers[i]);

      /* Do not launch an IB2 for secondary command buffers that contain
       * DRAW_{INDEX}_INDIRECT_{MULTI} on GFX6-7 because it's illegal and hangs the GPU.
       */
      const bool allow_ib2 = !secondary->state.uses_draw_indirect || pdev->info.gfx_level >= GFX8;

      primary->scratch_size_per_wave_needed =
         MAX2(primary->scratch_size_per_wave_needed, secondary->scratch_size_per_wave_needed);
      primary->scratch_waves_wanted = MAX2(primary->scratch_waves_wanted, secondary->scratch_waves_wanted);
      primary->compute_scratch_size_per_wave_needed =
         MAX2(primary->compute_scratch_size_per_wave_needed, secondary->compute_scratch_size_per_wave_needed);
      primary->compute_scratch_waves_wanted =
         MAX2(primary->compute_scratch_waves_wanted, secondary->compute_scratch_waves_wanted);

      if (secondary->esgs_ring_size_needed > primary->esgs_ring_size_needed)
         primary->esgs_ring_size_needed = secondary->esgs_ring_size_needed;
      if (secondary->gsvs_ring_size_needed > primary->gsvs_ring_size_needed)
         primary->gsvs_ring_size_needed = secondary->gsvs_ring_size_needed;
      if (secondary->tess_rings_needed)
         primary->tess_rings_needed = true;
      if (secondary->task_rings_needed)
         primary->task_rings_needed = true;
      if (secondary->mesh_scratch_ring_needed)
         primary->mesh_scratch_ring_needed = true;
      if (secondary->sample_positions_needed)
         primary->sample_positions_needed = true;
      if (secondary->gds_needed)
         primary->gds_needed = true;
      if (secondary->gds_oa_needed)
         primary->gds_oa_needed = true;

      primary->shader_upload_seq = MAX2(primary->shader_upload_seq, secondary->shader_upload_seq);

      primary->state.uses_fbfetch_output |= secondary->state.uses_fbfetch_output;

      if (!secondary->state.render.has_image_views) {
         if (primary->state.dirty & RADV_CMD_DIRTY_FBFETCH_OUTPUT) {
            radv_handle_fbfetch_output(primary);
            primary->state.dirty &= ~RADV_CMD_DIRTY_FBFETCH_OUTPUT;
         }

         if (primary->state.render.active && (primary->state.dirty & RADV_CMD_DIRTY_FRAMEBUFFER)) {
            /* Emit the framebuffer state from primary if secondary
             * has been recorded without a framebuffer, otherwise
             * fast color/depth clears can't work.
             */
            radv_emit_framebuffer_state(primary);

            if (pdev->gfx12_hiz_wa == RADV_GFX12_HIZ_WA_FULL) {
               const struct radv_rendering_state *render = &primary->state.render;
               const struct radv_image_view *iview = render->ds_att.iview;

               if (iview && iview->image->hiz_valid_offset) {
                  /* On GFX12, if the HiZ workaround using metadata is enabled, we need to consider
                   * that any of the draws in the secondary command buffer could trigger the issue
                   * and HiZ needs to be disabled completely.
                   */
                  const VkImageSubresourceRange range = {
                     .aspectMask = render->ds_att_aspects,
                     .baseMipLevel = iview->vk.base_mip_level,
                     .levelCount = iview->vk.level_count,
                     .baseArrayLayer = iview->vk.base_array_layer,
                     .layerCount = iview->vk.layer_count,
                  };

                  radv_gfx12_override_hiz_enable(primary, false);
                  radv_update_hiz_metadata(primary, iview->image, &range, false);
               }
            }
         }
      }

      if (secondary->gang.cs) {
         if (!radv_gang_init(primary))
            return;

         struct radv_cmd_stream *ace_primary = primary->gang.cs;
         struct radv_cmd_stream *ace_secondary = secondary->gang.cs;

         /* Emit pending flushes on primary prior to executing secondary. */
         radv_gang_cache_flush(primary);

         /* Wait for gang semaphores, if necessary. */
         if (radv_flush_gang_leader_semaphore(primary))
            radv_wait_gang_leader(primary);
         if (radv_flush_gang_follower_semaphore(primary))
            radv_wait_gang_follower(primary);

         /* Execute the secondary compute cmdbuf.
          * Don't use IB2 packets because they are not supported on compute queues.
          */
         device->ws->cs_execute_secondary(ace_primary->b, ace_secondary->b, false);
      }

      /* Update pending ACE internal flush bits from the secondary cmdbuf */
      primary->gang.flush_bits |= secondary->gang.flush_bits;

      /* Increment gang semaphores if secondary was dirty.
       * This happens when the secondary cmdbuf has a barrier which
       * isn't consumed by a draw call.
       */
      if (radv_gang_leader_sem_dirty(secondary))
         primary->gang.sem.leader_value++;
      if (radv_gang_follower_sem_dirty(secondary))
         primary->gang.sem.follower_value++;

      struct radv_cmd_stream *primary_cs = primary->cs;
      struct radv_cmd_stream *secondary_cs = secondary->cs;

      device->ws->cs_execute_secondary(primary_cs->b, secondary_cs->b, allow_ib2);

      /* When the secondary command buffer is compute only we don't
       * need to re-emit the current graphics pipeline.
       */
      if (secondary->state.emitted_graphics_pipeline) {
         primary->state.emitted_graphics_pipeline = secondary->state.emitted_graphics_pipeline;
      }

      /* When the secondary command buffer is graphics only we don't
       * need to re-emit the current compute pipeline.
       */
      if (secondary->state.emitted_compute_pipeline) {
         primary->state.emitted_compute_pipeline = secondary->state.emitted_compute_pipeline;
      }

      if (secondary->state.last_ia_multi_vgt_param) {
         primary->state.last_ia_multi_vgt_param = secondary->state.last_ia_multi_vgt_param;
      }

      if (secondary->state.last_ge_cntl) {
         primary->state.last_ge_cntl = secondary->state.last_ge_cntl;
      }

      primary->state.last_num_instances = secondary->state.last_num_instances;
      primary->state.last_subpass_color_count = secondary->state.last_subpass_color_count;

      if (secondary->state.last_index_type != -1) {
         primary->state.last_index_type = secondary->state.last_index_type;
      }

      if (secondary->state.last_primitive_restart_en != -1) {
         primary->state.last_primitive_restart_en = secondary->state.last_primitive_restart_en;
      }

      if (secondary->state.last_primitive_reset_index) {
         primary->state.last_primitive_reset_index = secondary->state.last_primitive_reset_index;
      }

      primary->state.rb_noncoherent_dirty |= secondary->state.rb_noncoherent_dirty;

      primary->state.uses_draw_indirect |= secondary->state.uses_draw_indirect;

      for (uint32_t reg = 0; reg < RADV_NUM_ALL_TRACKED_REGS; reg++) {
         if (!BITSET_TEST(secondary_cs->tracked_regs.reg_saved_mask, reg))
            continue;

         BITSET_SET(primary_cs->tracked_regs.reg_saved_mask, reg);
         primary_cs->tracked_regs.reg_value[reg] = secondary_cs->tracked_regs.reg_value[reg];
      }

      memcpy(primary_cs->tracked_regs.spi_ps_input_cntl, secondary_cs->tracked_regs.spi_ps_input_cntl,
             sizeof(primary_cs->tracked_regs.spi_ps_input_cntl));
      memcpy(primary_cs->tracked_regs.cb_blend_control, secondary_cs->tracked_regs.cb_blend_control,
             sizeof(primary_cs->tracked_regs.cb_blend_control));
      memcpy(primary_cs->tracked_regs.sx_mrt_blend_opt, secondary_cs->tracked_regs.sx_mrt_blend_opt,
             sizeof(primary_cs->tracked_regs.sx_mrt_blend_opt));
   }

   /* After executing commands from secondary buffers we have to dirty
    * some states.
    */
   primary->state.dirty_dynamic |= RADV_DYNAMIC_ALL;
   primary->state.dirty |= RADV_CMD_DIRTY_PIPELINE | RADV_CMD_DIRTY_INDEX_BUFFER | RADV_CMD_DIRTY_GUARDBAND |
                           RADV_CMD_DIRTY_SHADER_QUERY | RADV_CMD_DIRTY_OCCLUSION_QUERY |
                           RADV_CMD_DIRTY_DB_SHADER_CONTROL | RADV_CMD_DIRTY_FRAGMENT_OUTPUT;
   radv_mark_descriptors_dirty(primary, VK_PIPELINE_BIND_POINT_GRAPHICS);
   radv_mark_descriptors_dirty(primary, VK_PIPELINE_BIND_POINT_COMPUTE);

   primary->state.last_first_instance = -1;
   primary->state.last_drawid = -1;
   primary->state.last_vertex_offset_valid = false;
}

static void
radv_mark_noncoherent_rb(struct radv_cmd_buffer *cmd_buffer)
{
   const struct radv_device *device = radv_cmd_buffer_device(cmd_buffer);
   struct radv_rendering_state *render = &cmd_buffer->state.render;

   /* Have to be conservative in cmdbuffers with inherited attachments. */
   if (!render->has_image_views) {
      cmd_buffer->state.rb_noncoherent_dirty = true;
      return;
   }

   for (uint32_t i = 0; i < render->color_att_count; i++) {
      const struct radv_image_view *iview = render->color_att[i].iview;

      if (!iview)
         continue;

      const VkImageSubresourceRange range = vk_image_view_subresource_range(&iview->vk);

      if (!radv_image_is_l2_coherent(device, iview->image, &range)) {
         cmd_buffer->state.rb_noncoherent_dirty = true;
         return;
      }
   }

   const struct radv_image_view *iview = render->ds_att.iview;

   if (iview) {
      const VkImageSubresourceRange range = vk_image_view_subresource_range(&iview->vk);

      if (!radv_image_is_l2_coherent(device, iview->image, &range))
         cmd_buffer->state.rb_noncoherent_dirty = true;
   }
}

static VkImageLayout
attachment_initial_layout(const VkRenderingAttachmentInfo *att)
{
   const VkRenderingAttachmentInitialLayoutInfoMESA *layout_info =
      vk_find_struct_const(att->pNext, RENDERING_ATTACHMENT_INITIAL_LAYOUT_INFO_MESA);
   if (layout_info != NULL)
      return layout_info->initialLayout;

   return att->imageLayout;
}

static VkImageLayout
get_image_layout(const VkRenderingAttachmentInfo *att)
{
   const VkAttachmentFeedbackLoopInfoEXT *feedback_loop_info =
      vk_find_struct_const(att->pNext, ATTACHMENT_FEEDBACK_LOOP_INFO_EXT);
   if (feedback_loop_info && feedback_loop_info->feedbackLoopEnable)
      return VK_IMAGE_LAYOUT_ATTACHMENT_FEEDBACK_LOOP_OPTIMAL_EXT;

   return att->imageLayout;
}

VKAPI_ATTR void VKAPI_CALL
radv_CmdBeginRendering(VkCommandBuffer commandBuffer, const VkRenderingInfo *pRenderingInfo)
{
   VK_FROM_HANDLE(radv_cmd_buffer, cmd_buffer, commandBuffer);
   struct radv_device *device = radv_cmd_buffer_device(cmd_buffer);
   const struct radv_physical_device *pdev = radv_device_physical(device);
   VkExtent2D screen_scissor = {MAX_FRAMEBUFFER_WIDTH, MAX_FRAMEBUFFER_HEIGHT};
   struct radv_cmd_stream *cs = cmd_buffer->cs;
   bool disable_constant_encode_ac01 = false;

   const struct VkSampleLocationsInfoEXT *sample_locs_info =
      vk_find_struct_const(pRenderingInfo->pNext, SAMPLE_LOCATIONS_INFO_EXT);

   struct radv_sample_locations_state sample_locations = {
      .count = 0,
   };
   if (sample_locs_info) {
      sample_locations = (struct radv_sample_locations_state){
         .per_pixel = sample_locs_info->sampleLocationsPerPixel,
         .grid_size = sample_locs_info->sampleLocationGridSize,
         .count = sample_locs_info->sampleLocationsCount,
      };
      typed_memcpy(sample_locations.locations, sample_locs_info->pSampleLocations,
                   sample_locs_info->sampleLocationsCount);
   }

   /* Dynamic rendering does not have implicit transitions, so limit the marker to
    * when a render pass is used.
    * Additionally, some internal meta operations called inside a barrier may issue
    * render calls (with dynamic rendering), so this makes sure those case don't
    * create a nested barrier scope.
    */
   if (cmd_buffer->vk.render_pass)
      radv_describe_barrier_start(cmd_buffer, RGP_BARRIER_EXTERNAL_RENDER_PASS_SYNC);
   uint32_t color_samples = 0, ds_samples = 0;
   struct radv_attachment color_att[MAX_RTS];
   for (uint32_t i = 0; i < pRenderingInfo->colorAttachmentCount; i++) {
      const VkRenderingAttachmentInfo *att_info = &pRenderingInfo->pColorAttachments[i];

      color_att[i] = (struct radv_attachment){.iview = NULL};
      if (att_info->imageView == VK_NULL_HANDLE)
         continue;

      VK_FROM_HANDLE(radv_image_view, iview, att_info->imageView);

      color_att[i].format = iview->vk.format;
      color_att[i].iview = iview;
      color_att[i].layout = get_image_layout(att_info);
      radv_initialise_color_surface(device, &color_att[i].cb, iview);

      if (att_info->resolveMode != VK_RESOLVE_MODE_NONE && att_info->resolveImageView != VK_NULL_HANDLE) {
         color_att[i].resolve_mode = att_info->resolveMode;
         color_att[i].resolve_iview = radv_image_view_from_handle(att_info->resolveImageView);
         color_att[i].resolve_layout = att_info->resolveImageLayout;
      }

      color_samples = MAX2(color_samples, color_att[i].iview->vk.image->samples);

      VkImageLayout initial_layout = attachment_initial_layout(att_info);
      if (initial_layout != color_att[i].layout) {
         assert(!(pRenderingInfo->flags & VK_RENDERING_RESUMING_BIT));
         radv_handle_rendering_image_transition(cmd_buffer, color_att[i].iview, pRenderingInfo->layerCount,
                                                pRenderingInfo->viewMask, initial_layout, VK_IMAGE_LAYOUT_UNDEFINED,
                                                color_att[i].layout, VK_IMAGE_LAYOUT_UNDEFINED, &sample_locations);
      }

      if (pdev->info.gfx_level >= GFX9 && iview->image->dcc_sign_reinterpret) {
         /* Disable constant encoding with the clear value of "1" with different DCC signedness
          * because the hardware will fill "1" instead of the clear value.
          */
         disable_constant_encode_ac01 = true;
      }

      screen_scissor.width = MIN2(screen_scissor.width, iview->vk.extent.width);
      screen_scissor.height = MIN2(screen_scissor.height, iview->vk.extent.height);
   }

   struct radv_attachment ds_att = {.iview = NULL};
   VkImageAspectFlags ds_att_aspects = 0;
   const VkRenderingAttachmentInfo *d_att_info = pRenderingInfo->pDepthAttachment;
   const VkRenderingAttachmentInfo *s_att_info = pRenderingInfo->pStencilAttachment;
   bool has_hiz_his = false;

   if ((d_att_info != NULL && d_att_info->imageView != VK_NULL_HANDLE) ||
       (s_att_info != NULL && s_att_info->imageView != VK_NULL_HANDLE)) {
      struct radv_image_view *d_iview = NULL, *s_iview = NULL;
      struct radv_image_view *d_res_iview = NULL, *s_res_iview = NULL;
      VkImageLayout initial_depth_layout = VK_IMAGE_LAYOUT_UNDEFINED;
      VkImageLayout initial_stencil_layout = VK_IMAGE_LAYOUT_UNDEFINED;

      if (d_att_info != NULL && d_att_info->imageView != VK_NULL_HANDLE) {
         d_iview = radv_image_view_from_handle(d_att_info->imageView);
         initial_depth_layout = attachment_initial_layout(d_att_info);
         ds_att.layout = get_image_layout(d_att_info);

         if (d_att_info->resolveMode != VK_RESOLVE_MODE_NONE && d_att_info->resolveImageView != VK_NULL_HANDLE) {
            d_res_iview = radv_image_view_from_handle(d_att_info->resolveImageView);
            ds_att.resolve_mode = d_att_info->resolveMode;
            ds_att.resolve_layout = d_att_info->resolveImageLayout;
         }
      }

      if (s_att_info != NULL && s_att_info->imageView != VK_NULL_HANDLE) {
         s_iview = radv_image_view_from_handle(s_att_info->imageView);
         initial_stencil_layout = attachment_initial_layout(s_att_info);
         ds_att.stencil_layout = get_image_layout(s_att_info);

         if (s_att_info->resolveMode != VK_RESOLVE_MODE_NONE && s_att_info->resolveImageView != VK_NULL_HANDLE) {
            s_res_iview = radv_image_view_from_handle(s_att_info->resolveImageView);
            ds_att.stencil_resolve_mode = s_att_info->resolveMode;
            ds_att.stencil_resolve_layout = s_att_info->resolveImageLayout;
         }
      }

      assert(d_iview == NULL || s_iview == NULL || d_iview == s_iview);
      ds_att.iview = d_iview ? d_iview : s_iview, ds_att.format = ds_att.iview->vk.format;

      if (d_iview && s_iview) {
         ds_att_aspects = VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT;
      } else if (d_iview) {
         ds_att_aspects = VK_IMAGE_ASPECT_DEPTH_BIT;
      } else {
         ds_att_aspects = VK_IMAGE_ASPECT_STENCIL_BIT;
      }

      if (pdev->info.gfx_level >= GFX12) {
         const struct radeon_surf *surf = &ds_att.iview->image->planes[0].surface;

         has_hiz_his = surf->u.gfx9.zs.hiz.offset || surf->u.gfx9.zs.his.offset;
      }

      radv_initialise_ds_surface(device, &ds_att.ds, ds_att.iview, ds_att_aspects);

      assert(d_res_iview == NULL || s_res_iview == NULL || d_res_iview == s_res_iview);
      ds_att.resolve_iview = d_res_iview ? d_res_iview : s_res_iview;

      ds_samples = ds_att.iview->vk.image->samples;

      if (initial_depth_layout != ds_att.layout || initial_stencil_layout != ds_att.stencil_layout) {
         assert(!(pRenderingInfo->flags & VK_RENDERING_RESUMING_BIT));
         radv_handle_rendering_image_transition(cmd_buffer, ds_att.iview, pRenderingInfo->layerCount,
                                                pRenderingInfo->viewMask, initial_depth_layout, initial_stencil_layout,
                                                ds_att.layout, ds_att.stencil_layout, &sample_locations);
      }

      screen_scissor.width = MIN2(screen_scissor.width, ds_att.iview->vk.extent.width);
      screen_scissor.height = MIN2(screen_scissor.height, ds_att.iview->vk.extent.height);
   }
   if (cmd_buffer->vk.render_pass)
      radv_describe_barrier_end(cmd_buffer);

   const VkRenderingFragmentShadingRateAttachmentInfoKHR *fsr_info =
      vk_find_struct_const(pRenderingInfo->pNext, RENDERING_FRAGMENT_SHADING_RATE_ATTACHMENT_INFO_KHR);
   struct radv_attachment vrs_att = {.iview = NULL};
   VkExtent2D vrs_texel_size = {.width = 0};
   if (fsr_info && fsr_info->imageView) {
      VK_FROM_HANDLE(radv_image_view, iview, fsr_info->imageView);
      vrs_att = (struct radv_attachment){
         .format = iview->vk.format,
         .iview = iview,
         .layout = fsr_info->imageLayout,
      };
      vrs_texel_size = fsr_info->shadingRateAttachmentTexelSize;
   }

   /* Now that we've done any layout transitions which may invoke meta, we can
    * fill out the actual rendering info and set up for the client's render pass.
    */
   radv_cmd_buffer_reset_rendering(cmd_buffer);

   struct radv_rendering_state *render = &cmd_buffer->state.render;
   render->active = true;
   render->has_image_views = true;
   render->has_input_attachment_no_concurrent_writes =
      !!(pRenderingInfo->flags & VK_RENDERING_INPUT_ATTACHMENT_NO_CONCURRENT_WRITES_BIT_MESA);
   render->area = pRenderingInfo->renderArea;
   render->view_mask = pRenderingInfo->viewMask;
   render->layer_count = pRenderingInfo->layerCount;
   render->color_samples = color_samples;
   render->ds_samples = ds_samples;
   render->max_samples = MAX2(color_samples, ds_samples);
   render->sample_locations = sample_locations;
   render->color_att_count = pRenderingInfo->colorAttachmentCount;
   typed_memcpy(render->color_att, color_att, render->color_att_count);
   render->ds_att = ds_att;
   render->ds_att_aspects = ds_att_aspects;
   render->has_hiz_his = has_hiz_his;
   render->vrs_att = vrs_att;
   render->vrs_texel_size = vrs_texel_size;
   cmd_buffer->state.dirty |= RADV_CMD_DIRTY_FRAMEBUFFER | RADV_CMD_DIRTY_BINNING_STATE |
                              RADV_CMD_DIRTY_FBFETCH_OUTPUT | RADV_CMD_DIRTY_DEPTH_BIAS_STATE |
                              RADV_CMD_DIRTY_DEPTH_STENCIL_STATE | RADV_CMD_DIRTY_CB_RENDER_STATE |
                              RADV_CMD_DIRTY_MSAA_STATE | RADV_CMD_DIRTY_RAST_SAMPLES_STATE | RADV_CMD_DIRTY_PS_STATE |
                              RADV_CMD_DIRTY_PS_EPILOG_SHADER;

   if (pdev->info.rbplus_allowed)
      cmd_buffer->state.dirty |= RADV_CMD_DIRTY_RBPLUS;
   if (pdev->info.gfx_level >= GFX10_3)
      cmd_buffer->state.dirty |= RADV_CMD_DIRTY_FSR_STATE;

   if (render->vrs_att.iview && pdev->info.gfx_level == GFX10_3) {
      if (render->ds_att.iview &&
          radv_htile_enabled(render->ds_att.iview->image, render->ds_att.iview->vk.base_mip_level)) {
         /* When we have a VRS attachment and a depth/stencil attachment, we just need to copy the
          * VRS rates to the HTILE buffer of the attachment.
          */
         struct radv_image_view *ds_iview = render->ds_att.iview;
         struct radv_image *ds_image = ds_iview->image;
         uint32_t level = ds_iview->vk.base_mip_level;

         /* HTILE buffer */
         uint64_t htile_offset =
            ds_image->planes[0].surface.meta_offset + ds_image->planes[0].surface.u.gfx9.meta_levels[level].offset;
         const uint64_t htile_va = ds_image->bindings[0].addr + htile_offset;

         assert(render->area.offset.x + render->area.extent.width <= ds_image->vk.extent.width &&
                render->area.offset.x + render->area.extent.height <= ds_image->vk.extent.height);

         /* Copy the VRS rates to the HTILE buffer. */
         radv_copy_vrs_htile(cmd_buffer, render->vrs_att.iview, &render->area, ds_image, htile_va, true);
      } else {
         /* When a subpass uses a VRS attachment without binding a depth/stencil attachment, or when
          * HTILE isn't enabled, we use a fallback that copies the VRS rates to our internal HTILE buffer.
          */
         struct radv_image *ds_image = radv_cmd_buffer_get_vrs_image(cmd_buffer);

         if (ds_image && render->area.offset.x < ds_image->vk.extent.width &&
             render->area.offset.y < ds_image->vk.extent.height) {
            /* HTILE buffer */
            struct radv_buffer *htile_buffer = device->vrs.buffer;
            const uint64_t htile_va = htile_buffer->vk.device_address;

            VkRect2D area = render->area;
            area.extent.width = MIN2(area.extent.width, ds_image->vk.extent.width - area.offset.x);
            area.extent.height = MIN2(area.extent.height, ds_image->vk.extent.height - area.offset.y);

            /* Copy the VRS rates to the HTILE buffer. */
            radv_copy_vrs_htile(cmd_buffer, render->vrs_att.iview, &area, ds_image, htile_va, false);
         }
      }
   }

   const uint32_t minx = render->area.offset.x;
   const uint32_t miny = render->area.offset.y;
   const uint32_t maxx = minx + render->area.extent.width;
   const uint32_t maxy = miny + render->area.extent.height;

   radeon_check_space(device->ws, cs->b, 10);

   if (pdev->info.gfx_level >= GFX12) {
      radeon_begin(cs);
      gfx12_begin_context_regs();
      gfx12_set_context_reg(R_028204_PA_SC_WINDOW_SCISSOR_TL, S_028204_TL_X(minx) | S_028204_TL_Y_GFX12(miny));
      gfx12_set_context_reg(R_028208_PA_SC_WINDOW_SCISSOR_BR,
                            S_028208_BR_X(maxx - 1) | S_028208_BR_Y(maxy - 1)); /* inclusive */
      gfx12_set_context_reg(R_028184_PA_SC_SCREEN_SCISSOR_BR,
                            S_028034_BR_X(screen_scissor.width) | S_028034_BR_Y(screen_scissor.height));
      gfx12_end_context_regs();
      radeon_end();
   } else {
      radeon_begin(cs);
      radeon_set_context_reg_seq(R_028204_PA_SC_WINDOW_SCISSOR_TL, 2);
      radeon_emit(S_028204_TL_X(minx) | S_028204_TL_Y_GFX6(miny));
      radeon_emit(S_028208_BR_X(maxx) | S_028208_BR_Y(maxy));
      radeon_set_context_reg(R_028034_PA_SC_SCREEN_SCISSOR_BR,
                             S_028034_BR_X(screen_scissor.width) | S_028034_BR_Y(screen_scissor.height));

      if (pdev->info.gfx_level >= GFX8 && pdev->info.gfx_level < GFX11) {
         const bool disable_constant_encode = pdev->info.has_dcc_constant_encode;
         const uint8_t watermark = pdev->info.gfx_level >= GFX10 ? 6 : 4;

         radeon_set_context_reg(R_028424_CB_DCC_CONTROL,
                                S_028424_OVERWRITE_COMBINER_MRT_SHARING_DISABLE(pdev->info.gfx_level <= GFX9) |
                                   S_028424_OVERWRITE_COMBINER_WATERMARK(watermark) |
                                   S_028424_DISABLE_CONSTANT_ENCODE_AC01(disable_constant_encode_ac01) |
                                   S_028424_DISABLE_CONSTANT_ENCODE_REG(disable_constant_encode));
      }
      radeon_end();
   }

   radv_emit_fb_mip_change_flush(cmd_buffer);

   if (!(pRenderingInfo->flags & VK_RENDERING_RESUMING_BIT))
      radv_cmd_buffer_clear_rendering(cmd_buffer, pRenderingInfo);
}

VKAPI_ATTR void VKAPI_CALL
radv_CmdEndRendering(VkCommandBuffer commandBuffer)
{
   VK_FROM_HANDLE(radv_cmd_buffer, cmd_buffer, commandBuffer);

   radv_mark_noncoherent_rb(cmd_buffer);
   radv_cmd_buffer_resolve_rendering(cmd_buffer);
   radv_cmd_buffer_reset_rendering(cmd_buffer);
}

static void
radv_emit_view_index_per_stage(struct radv_cmd_stream *cs, const struct radv_shader *shader, uint32_t base_reg,
                               unsigned index)
{
   const uint32_t view_index_offset = radv_get_user_sgpr_loc(shader, AC_UD_VIEW_INDEX);

   if (!view_index_offset)
      return;

   radeon_begin(cs);
   radeon_set_sh_reg(view_index_offset, index);
   radeon_end();
}

static void
radv_emit_view_index(const struct radv_cmd_state *cmd_state, struct radv_cmd_stream *cs, unsigned index)
{
   radv_foreach_stage (stage, cmd_state->active_stages & ~VK_SHADER_STAGE_TASK_BIT_EXT) {
      const struct radv_shader *shader = radv_get_shader(cmd_state->shaders, stage);

      radv_emit_view_index_per_stage(cs, shader, shader->info.user_data_0, index);
   }

   if (cmd_state->gs_copy_shader) {
      radv_emit_view_index_per_stage(cs, cmd_state->gs_copy_shader, R_00B130_SPI_SHADER_USER_DATA_VS_0, index);
   }
}

static void
radv_emit_copy_data_imm(const struct radv_physical_device *pdev, struct radv_cmd_stream *cs, uint32_t src_imm,
                        uint64_t dst_va)
{
   ac_emit_cp_copy_data(cs->b, COPY_DATA_IMM, COPY_DATA_DST_MEM, src_imm, dst_va,
                        AC_CP_COPY_DATA_WR_CONFIRM | (pdev->info.gfx_level == GFX6 ? AC_CP_COPY_DATA_ENGINE_PFP : 0));
}

/**
 * Emulates predication for MEC using COND_EXEC.
 * When the current command buffer is predicating, emit a COND_EXEC packet
 * so that the MEC skips the next few dwords worth of packets.
 *
 * To make it work with inverted conditional rendering, we allocate
 * space in the upload BO and emit some packets to invert the condition.
 */
static void
radv_cs_emit_compute_predication(const struct radv_device *device, struct radv_cmd_state *state,
                                 struct radv_cmd_stream *cs, uint64_t inv_va, bool *inv_emitted, unsigned dwords)
{
   const struct radv_physical_device *pdev = radv_device_physical(device);

   if (!state->predicating)
      return;

   uint64_t va = state->user_predication_va;

   if (!state->predication_type) {
      /* Invert the condition the first time it is needed. */
      if (!*inv_emitted) {
         *inv_emitted = true;

         /* Write 1 to the inverted predication VA. */
         radv_emit_copy_data_imm(pdev, cs, 1, inv_va);

         /* If the API predication VA == 0, skip next command. */
         ac_emit_cond_exec(cs->b, pdev->info.gfx_level, va, 6 /* 1x COPY_DATA size */);

         /* Write 0 to the new predication VA (when the API condition != 0) */
         radv_emit_copy_data_imm(pdev, cs, 0, inv_va);
      }

      va = inv_va;
   }

   ac_emit_cond_exec(cs->b, pdev->info.gfx_level, va, dwords);
}

ALWAYS_INLINE static void
radv_gfx12_emit_hiz_his_wa(const struct radv_device *device, const struct radv_cmd_state *cmd_state,
                           struct radv_cmd_stream *cs)
{
   const struct radv_physical_device *pdev = radv_device_physical(device);
   const struct radv_rendering_state *render = &cmd_state->render;

   if (pdev->gfx12_hiz_wa == RADV_GFX12_HIZ_WA_PARTIAL && render->has_hiz_his) {
      radeon_begin(cs);
      radeon_emit(PKT3(PKT3_RELEASE_MEM, 6, 0));
      radeon_emit(S_490_EVENT_TYPE(V_028A90_BOTTOM_OF_PIPE_TS) | S_490_EVENT_INDEX(5));
      radeon_emit(0); /* DST_SEL, INT_SEL = no write confirm, DATA_SEL = no data */
      radeon_emit(0); /* ADDRESS_LO */
      radeon_emit(0); /* ADDRESS_HI */
      radeon_emit(0); /* DATA_LO */
      radeon_emit(0); /* DATA_HI */
      radeon_emit(0); /* INT_CTXID */
      radeon_end();
   }
}

static void
radv_cs_emit_draw_packet(struct radv_cmd_buffer *cmd_buffer, uint32_t vertex_count, uint32_t use_opaque)
{
   const struct radv_device *device = radv_cmd_buffer_device(cmd_buffer);
   struct radv_cmd_stream *cs = cmd_buffer->cs;

   radeon_begin(cs);
   radeon_emit(PKT3(PKT3_DRAW_INDEX_AUTO, 1, cmd_buffer->state.predicating));
   radeon_emit(vertex_count);
   radeon_emit(V_0287F0_DI_SRC_SEL_AUTO_INDEX | use_opaque);
   radeon_end();

   radv_gfx12_emit_hiz_his_wa(device, &cmd_buffer->state, cs);
}

/**
 * Emit a PKT3_DRAW_INDEX_2 packet to render "index_count` vertices.
 *
 * The starting address "index_va" may point anywhere within the index buffer. The number of
 * indexes allocated in the index buffer *past that point* is specified by "max_index_count".
 * Hardware uses this information to return 0 for out-of-bounds reads.
 */
static void
radv_cs_emit_draw_indexed_packet(struct radv_cmd_buffer *cmd_buffer, uint64_t index_va, uint32_t max_index_count,
                                 uint32_t index_count, bool not_eop)
{
   const struct radv_device *device = radv_cmd_buffer_device(cmd_buffer);
   struct radv_cmd_stream *cs = cmd_buffer->cs;

   radeon_begin(cs);
   radeon_emit(PKT3(PKT3_DRAW_INDEX_2, 4, cmd_buffer->state.predicating));
   radeon_emit(max_index_count);
   radeon_emit(index_va);
   radeon_emit(index_va >> 32);
   radeon_emit(index_count);
   /* NOT_EOP allows merging multiple draws into 1 wave, but only user VGPRs
    * can be changed between draws and GS fast launch must be disabled.
    * NOT_EOP doesn't work on gfx6-gfx9 and gfx12.
    */
   radeon_emit(V_0287F0_DI_SRC_SEL_DMA | S_0287F0_NOT_EOP(not_eop));
   radeon_end();

   radv_gfx12_emit_hiz_his_wa(device, &cmd_buffer->state, cs);
}

/* MUST inline this function to avoid massive perf loss in drawoverhead */
ALWAYS_INLINE static void
radv_cs_emit_indirect_draw_packet(struct radv_cmd_buffer *cmd_buffer, bool indexed, uint32_t draw_count,
                                  uint64_t count_va, uint32_t stride)
{
   const struct radv_device *device = radv_cmd_buffer_device(cmd_buffer);
   struct radv_cmd_stream *cs = cmd_buffer->cs;
   const unsigned di_src_sel = indexed ? V_0287F0_DI_SRC_SEL_DMA : V_0287F0_DI_SRC_SEL_AUTO_INDEX;
   bool draw_id_enable = cmd_buffer->state.uses_drawid;
   uint32_t base_reg = cmd_buffer->state.vtx_base_sgpr;
   uint32_t vertex_offset_reg, start_instance_reg = 0, draw_id_reg = 0;
   bool predicating = cmd_buffer->state.predicating;
   assert(base_reg);

   /* just reset draw state for vertex data */
   cmd_buffer->state.last_first_instance = -1;
   cmd_buffer->state.last_num_instances = -1;
   cmd_buffer->state.last_drawid = -1;
   cmd_buffer->state.last_vertex_offset_valid = false;

   vertex_offset_reg = (base_reg - SI_SH_REG_OFFSET) >> 2;
   if (cmd_buffer->state.uses_baseinstance)
      start_instance_reg = ((base_reg + (draw_id_enable ? 8 : 4)) - SI_SH_REG_OFFSET) >> 2;
   if (draw_id_enable)
      draw_id_reg = ((base_reg + 4) - SI_SH_REG_OFFSET) >> 2;

   radeon_begin(cs);

   if (draw_count == 1 && !count_va && !draw_id_enable) {
      radeon_emit(PKT3(indexed ? PKT3_DRAW_INDEX_INDIRECT : PKT3_DRAW_INDIRECT, 3, predicating));
      radeon_emit(0);
      radeon_emit(vertex_offset_reg);
      radeon_emit(start_instance_reg);
      radeon_emit(di_src_sel);
   } else {
      radeon_emit(PKT3(indexed ? PKT3_DRAW_INDEX_INDIRECT_MULTI : PKT3_DRAW_INDIRECT_MULTI, 8, predicating));
      radeon_emit(0);
      radeon_emit(vertex_offset_reg);
      radeon_emit(start_instance_reg);
      radeon_emit(draw_id_reg | S_2C3_DRAW_INDEX_ENABLE(draw_id_enable) | S_2C3_COUNT_INDIRECT_ENABLE(!!count_va));
      radeon_emit(draw_count); /* count */
      radeon_emit(count_va);   /* count_addr */
      radeon_emit(count_va >> 32);
      radeon_emit(stride); /* stride */
      radeon_emit(di_src_sel);
   }

   radeon_end();

   radv_gfx12_emit_hiz_his_wa(device, &cmd_buffer->state, cs);

   cmd_buffer->state.uses_draw_indirect = true;
}

ALWAYS_INLINE static void
radv_cs_emit_indirect_mesh_draw_packet(struct radv_cmd_buffer *cmd_buffer, uint32_t draw_count, uint64_t count_va,
                                       uint32_t stride)
{
   struct radv_device *device = radv_cmd_buffer_device(cmd_buffer);
   const struct radv_physical_device *pdev = radv_device_physical(device);
   const struct radv_shader *mesh_shader = cmd_buffer->state.shaders[MESA_SHADER_MESH];
   struct radv_cmd_stream *cs = cmd_buffer->cs;
   uint32_t base_reg = cmd_buffer->state.vtx_base_sgpr;
   bool predicating = cmd_buffer->state.predicating;
   assert(base_reg || (!cmd_buffer->state.uses_drawid && !mesh_shader->info.cs.uses_grid_size));

   /* Reset draw state. */
   cmd_buffer->state.last_first_instance = -1;
   cmd_buffer->state.last_num_instances = -1;
   cmd_buffer->state.last_drawid = -1;
   cmd_buffer->state.last_vertex_offset_valid = false;

   uint32_t xyz_dim_enable = mesh_shader->info.cs.uses_grid_size;
   uint32_t xyz_dim_reg = !xyz_dim_enable ? 0 : (base_reg - SI_SH_REG_OFFSET) >> 2;
   uint32_t draw_id_enable = !!cmd_buffer->state.uses_drawid;
   uint32_t draw_id_reg = !draw_id_enable ? 0 : (base_reg + (xyz_dim_enable ? 12 : 0) - SI_SH_REG_OFFSET) >> 2;

   uint32_t mode1_enable = !pdev->info.mesh_fast_launch_2;

   radeon_begin(cs);
   radeon_emit(PKT3(PKT3_DISPATCH_MESH_INDIRECT_MULTI, 7, predicating) | PKT3_RESET_FILTER_CAM_S(1));
   radeon_emit(0); /* data_offset */
   radeon_emit(S_4C1_XYZ_DIM_REG(xyz_dim_reg) | S_4C1_DRAW_INDEX_REG(draw_id_reg));
   if (pdev->info.gfx_level >= GFX11)
      radeon_emit(S_4C2_DRAW_INDEX_ENABLE(draw_id_enable) | S_4C2_COUNT_INDIRECT_ENABLE(!!count_va) |
                  S_4C2_XYZ_DIM_ENABLE(xyz_dim_enable) | S_4C2_MODE1_ENABLE(mode1_enable));
   else
      radeon_emit(S_4C2_DRAW_INDEX_ENABLE(draw_id_enable) | S_4C2_COUNT_INDIRECT_ENABLE(!!count_va));
   radeon_emit(draw_count);
   radeon_emit(count_va);
   radeon_emit(count_va >> 32);
   radeon_emit(stride);
   radeon_emit(V_0287F0_DI_SRC_SEL_AUTO_INDEX);
   radeon_end();

   radv_gfx12_emit_hiz_his_wa(device, &cmd_buffer->state, cs);
}

ALWAYS_INLINE static void
radv_cs_emit_dispatch_taskmesh_direct_ace_packet(const struct radv_device *device,
                                                 const struct radv_cmd_state *cmd_state, struct radv_cmd_stream *ace_cs,
                                                 const uint32_t x, const uint32_t y, const uint32_t z)
{
   const struct radv_shader *task_shader = cmd_state->shaders[MESA_SHADER_TASK];
   const bool predicating = cmd_state->predicating;
   const uint32_t dispatch_initiator =
      device->dispatch_initiator_task | S_00B800_CS_W32_EN(task_shader->info.wave_size == 32);
   const uint32_t ring_entry_reg = radv_get_user_sgpr(task_shader, AC_UD_TASK_RING_ENTRY);

   radeon_begin(ace_cs);
   radeon_emit(PKT3(PKT3_DISPATCH_TASKMESH_DIRECT_ACE, 4, predicating) | PKT3_SHADER_TYPE_S(1));
   radeon_emit(x);
   radeon_emit(y);
   radeon_emit(z);
   radeon_emit(dispatch_initiator);
   radeon_emit(ring_entry_reg & 0xFFFF);
   radeon_end();
}

ALWAYS_INLINE static void
radv_cs_emit_dispatch_taskmesh_indirect_multi_ace_packet(const struct radv_device *device,
                                                         const struct radv_cmd_state *cmd_state,
                                                         struct radv_cmd_stream *ace_cs, uint64_t data_va,
                                                         uint32_t draw_count, uint64_t count_va, uint32_t stride)
{
   assert((data_va & 0x03) == 0);
   assert((count_va & 0x03) == 0);

   const struct radv_shader *task_shader = cmd_state->shaders[MESA_SHADER_TASK];

   const uint32_t dispatch_initiator =
      device->dispatch_initiator_task | S_00B800_CS_W32_EN(task_shader->info.wave_size == 32);
   const uint32_t ring_entry_reg = radv_get_user_sgpr(task_shader, AC_UD_TASK_RING_ENTRY);
   const uint32_t xyz_dim_reg = radv_get_user_sgpr(task_shader, AC_UD_CS_GRID_SIZE);
   const uint32_t draw_id_reg = radv_get_user_sgpr(task_shader, AC_UD_CS_TASK_DRAW_ID);

   radeon_begin(ace_cs);
   radeon_emit(PKT3(PKT3_DISPATCH_TASKMESH_INDIRECT_MULTI_ACE, 9, 0) | PKT3_SHADER_TYPE_S(1));
   radeon_emit(data_va);
   radeon_emit(data_va >> 32);
   radeon_emit(S_AD2_RING_ENTRY_REG(ring_entry_reg));
   radeon_emit(S_AD3_COUNT_INDIRECT_ENABLE(!!count_va) | S_AD3_DRAW_INDEX_ENABLE(!!draw_id_reg) |
               S_AD3_XYZ_DIM_ENABLE(!!xyz_dim_reg) | S_AD3_DRAW_INDEX_REG(draw_id_reg));
   radeon_emit(S_AD4_XYZ_DIM_REG(xyz_dim_reg));
   radeon_emit(draw_count);
   radeon_emit(count_va);
   radeon_emit(count_va >> 32);
   radeon_emit(stride);
   radeon_emit(dispatch_initiator);
   radeon_end();
}

ALWAYS_INLINE static void
radv_cs_emit_dispatch_taskmesh_gfx_packet(const struct radv_device *device, const struct radv_cmd_state *cmd_state,
                                          struct radv_cmd_stream *cs)
{
   const struct radv_physical_device *pdev = radv_device_physical(device);
   const struct radv_shader *mesh_shader = cmd_state->shaders[MESA_SHADER_MESH];
   const bool predicating = cmd_state->predicating;

   const uint32_t ring_entry_reg = radv_get_user_sgpr(mesh_shader, AC_UD_TASK_RING_ENTRY);

   uint32_t xyz_dim_en = mesh_shader->info.cs.uses_grid_size;
   uint32_t xyz_dim_reg = !xyz_dim_en ? 0 : (cmd_state->vtx_base_sgpr - SI_SH_REG_OFFSET) >> 2;
   uint32_t mode1_en = !pdev->info.mesh_fast_launch_2;
   uint32_t linear_dispatch_en = cmd_state->shaders[MESA_SHADER_TASK]->info.cs.linear_taskmesh_dispatch;
   const bool sqtt_en = !!device->sqtt.bo;

   radeon_begin(cs);
   radeon_emit(PKT3(PKT3_DISPATCH_TASKMESH_GFX, 2, predicating) | PKT3_RESET_FILTER_CAM_S(1));
   radeon_emit(S_4D0_RING_ENTRY_REG(ring_entry_reg) | S_4D0_XYZ_DIM_REG(xyz_dim_reg));
   if (pdev->info.gfx_level >= GFX11)
      radeon_emit(S_4D1_XYZ_DIM_ENABLE(xyz_dim_en) | S_4D1_MODE1_ENABLE(mode1_en) |
                  S_4D1_LINEAR_DISPATCH_ENABLE(linear_dispatch_en) | S_4D1_THREAD_TRACE_MARKER_ENABLE(sqtt_en));
   else
      radeon_emit(S_4D1_THREAD_TRACE_MARKER_ENABLE(sqtt_en));
   radeon_emit(V_0287F0_DI_SRC_SEL_AUTO_INDEX);
   radeon_end();

   radv_gfx12_emit_hiz_his_wa(device, cmd_state, cs);
}

ALWAYS_INLINE static void
radv_emit_userdata_vertex_internal(struct radv_cmd_buffer *cmd_buffer, const struct radv_draw_info *info,
                                   const uint32_t vertex_offset)
{
   struct radv_cmd_state *state = &cmd_buffer->state;
   struct radv_cmd_stream *cs = cmd_buffer->cs;
   const bool uses_baseinstance = state->uses_baseinstance;
   const bool uses_drawid = state->uses_drawid;

   radeon_begin(cs);
   radeon_set_sh_reg_seq(state->vtx_base_sgpr, state->vtx_emit_num);

   radeon_emit(vertex_offset);
   state->last_vertex_offset_valid = true;
   state->last_vertex_offset = vertex_offset;
   if (uses_drawid) {
      radeon_emit(0);
      state->last_drawid = 0;
   }
   if (uses_baseinstance) {
      radeon_emit(info->first_instance);
      state->last_first_instance = info->first_instance;
   }

   radeon_end();
}

ALWAYS_INLINE static void
radv_emit_userdata_vertex(struct radv_cmd_buffer *cmd_buffer, const struct radv_draw_info *info,
                          const uint32_t vertex_offset)
{
   const struct radv_cmd_state *state = &cmd_buffer->state;
   const bool uses_baseinstance = state->uses_baseinstance;
   const bool uses_drawid = state->uses_drawid;

   if (!state->last_vertex_offset_valid || vertex_offset != state->last_vertex_offset ||
       (uses_drawid && 0 != state->last_drawid) ||
       (uses_baseinstance && info->first_instance != state->last_first_instance))
      radv_emit_userdata_vertex_internal(cmd_buffer, info, vertex_offset);
}

ALWAYS_INLINE static void
radv_emit_userdata_vertex_drawid(struct radv_cmd_buffer *cmd_buffer, uint32_t vertex_offset, uint32_t drawid)
{
   struct radv_cmd_state *state = &cmd_buffer->state;
   struct radv_cmd_stream *cs = cmd_buffer->cs;

   radeon_begin(cs);
   radeon_set_sh_reg_seq(state->vtx_base_sgpr, 1 + !!drawid);
   radeon_emit(vertex_offset);
   state->last_vertex_offset_valid = true;
   state->last_vertex_offset = vertex_offset;
   if (drawid)
      radeon_emit(drawid);
   radeon_end();
}

ALWAYS_INLINE static void
radv_emit_userdata_mesh(struct radv_cmd_buffer *cmd_buffer, const uint32_t x, const uint32_t y, const uint32_t z)
{
   struct radv_cmd_state *state = &cmd_buffer->state;
   const struct radv_shader *mesh_shader = state->shaders[MESA_SHADER_MESH];
   struct radv_cmd_stream *cs = cmd_buffer->cs;
   const bool uses_drawid = state->uses_drawid;
   const bool uses_grid_size = mesh_shader->info.cs.uses_grid_size;

   if (!uses_drawid && !uses_grid_size)
      return;

   radeon_begin(cs);
   radeon_set_sh_reg_seq(state->vtx_base_sgpr, state->vtx_emit_num);
   if (uses_grid_size) {
      radeon_emit(x);
      radeon_emit(y);
      radeon_emit(z);
   }
   if (uses_drawid) {
      radeon_emit(0);
      state->last_drawid = 0;
   }
   radeon_end();
}

ALWAYS_INLINE static void
radv_emit_userdata_task(const struct radv_cmd_state *cmd_state, struct radv_cmd_stream *ace_cs, uint32_t x, uint32_t y,
                        uint32_t z)
{
   const struct radv_shader *task_shader = cmd_state->shaders[MESA_SHADER_TASK];

   const uint32_t xyz_offset = radv_get_user_sgpr_loc(task_shader, AC_UD_CS_GRID_SIZE);
   const uint32_t draw_id_offset = radv_get_user_sgpr_loc(task_shader, AC_UD_CS_TASK_DRAW_ID);

   radeon_begin(ace_cs);

   if (xyz_offset) {
      radeon_set_sh_reg_seq(xyz_offset, 3);
      radeon_emit(x);
      radeon_emit(y);
      radeon_emit(z);
   }

   if (draw_id_offset) {
      radeon_set_sh_reg(draw_id_offset, 0);
   }

   radeon_end();
}

ALWAYS_INLINE static void
radv_emit_draw_packets_indexed(struct radv_cmd_buffer *cmd_buffer, const struct radv_draw_info *info,
                               uint32_t drawCount, const VkMultiDrawIndexedInfoEXT *minfo, uint32_t stride,
                               const int32_t *vertexOffset)

{
   struct radv_device *device = radv_cmd_buffer_device(cmd_buffer);
   const struct radv_physical_device *pdev = radv_device_physical(device);
   struct radv_cmd_state *state = &cmd_buffer->state;
   struct radv_cmd_stream *cs = cmd_buffer->cs;
   const int index_size = radv_get_vgt_index_size(state->index_type);
   unsigned i = 0;
   const bool uses_drawid = state->uses_drawid;
   const bool can_eop = !uses_drawid && pdev->info.gfx_level >= GFX10 && pdev->info.gfx_level < GFX12;

   if (uses_drawid) {
      if (vertexOffset) {
         radv_emit_userdata_vertex(cmd_buffer, info, *vertexOffset);
         vk_foreach_multi_draw_indexed (draw, i, minfo, drawCount, stride) {
            uint32_t remaining_indexes = MAX2(state->max_index_count, draw->firstIndex) - draw->firstIndex;
            uint64_t index_va = state->index_va + draw->firstIndex * index_size;

            /* Handle draw calls with 0-sized index buffers if the GPU can't support them. */
            if (!remaining_indexes && pdev->info.has_zero_index_buffer_bug)
               radv_handle_zero_index_buffer_bug(cmd_buffer, &index_va, &remaining_indexes);

            if (i > 0) {
               radeon_begin(cs);
               radeon_set_sh_reg(state->vtx_base_sgpr + sizeof(uint32_t), i);
               radeon_end();
            }

            if (!state->render.view_mask) {
               radv_cs_emit_draw_indexed_packet(cmd_buffer, index_va, remaining_indexes, draw->indexCount, false);
            } else {
               u_foreach_bit (view, state->render.view_mask) {
                  radv_emit_view_index(&cmd_buffer->state, cs, view);

                  radv_cs_emit_draw_indexed_packet(cmd_buffer, index_va, remaining_indexes, draw->indexCount, false);
               }
            }
         }
      } else {
         vk_foreach_multi_draw_indexed (draw, i, minfo, drawCount, stride) {
            uint32_t remaining_indexes = MAX2(state->max_index_count, draw->firstIndex) - draw->firstIndex;
            uint64_t index_va = state->index_va + draw->firstIndex * index_size;

            /* Handle draw calls with 0-sized index buffers if the GPU can't support them. */
            if (!remaining_indexes && pdev->info.has_zero_index_buffer_bug)
               radv_handle_zero_index_buffer_bug(cmd_buffer, &index_va, &remaining_indexes);

            if (i > 0) {
               assert(state->last_vertex_offset_valid);
               if (state->last_vertex_offset != draw->vertexOffset) {
                  radv_emit_userdata_vertex_drawid(cmd_buffer, draw->vertexOffset, i);
               } else {
                  radeon_begin(cs);
                  radeon_set_sh_reg(state->vtx_base_sgpr + sizeof(uint32_t), i);
                  radeon_end();
               }
            } else
               radv_emit_userdata_vertex(cmd_buffer, info, draw->vertexOffset);

            if (!state->render.view_mask) {
               radv_cs_emit_draw_indexed_packet(cmd_buffer, index_va, remaining_indexes, draw->indexCount, false);
            } else {
               u_foreach_bit (view, state->render.view_mask) {
                  radv_emit_view_index(&cmd_buffer->state, cs, view);

                  radv_cs_emit_draw_indexed_packet(cmd_buffer, index_va, remaining_indexes, draw->indexCount, false);
               }
            }
         }
      }
      if (drawCount > 1) {
         state->last_drawid = drawCount - 1;
      }
   } else {
      if (vertexOffset) {
         if (pdev->info.gfx_level == GFX10) {
            /* GFX10 has a bug that consecutive draw packets with NOT_EOP must not have
             * count == 0 for the last draw that doesn't have NOT_EOP.
             */
            while (drawCount > 1) {
               const VkMultiDrawIndexedInfoEXT *last =
                  (const VkMultiDrawIndexedInfoEXT *)(((const uint8_t *)minfo) + (drawCount - 1) * stride);
               if (last->indexCount)
                  break;
               drawCount--;
            }
         }

         radv_emit_userdata_vertex(cmd_buffer, info, *vertexOffset);
         vk_foreach_multi_draw_indexed (draw, i, minfo, drawCount, stride) {
            uint32_t remaining_indexes = MAX2(state->max_index_count, draw->firstIndex) - draw->firstIndex;
            uint64_t index_va = state->index_va + draw->firstIndex * index_size;

            /* Handle draw calls with 0-sized index buffers if the GPU can't support them. */
            if (!remaining_indexes && pdev->info.has_zero_index_buffer_bug)
               radv_handle_zero_index_buffer_bug(cmd_buffer, &index_va, &remaining_indexes);

            if (!state->render.view_mask) {
               radv_cs_emit_draw_indexed_packet(cmd_buffer, index_va, remaining_indexes, draw->indexCount,
                                                can_eop && i < drawCount - 1);
            } else {
               u_foreach_bit (view, state->render.view_mask) {
                  radv_emit_view_index(&cmd_buffer->state, cs, view);

                  radv_cs_emit_draw_indexed_packet(cmd_buffer, index_va, remaining_indexes, draw->indexCount, false);
               }
            }
         }
      } else {
         vk_foreach_multi_draw_indexed (draw, i, minfo, drawCount, stride) {
            uint32_t remaining_indexes = MAX2(state->max_index_count, draw->firstIndex) - draw->firstIndex;
            uint64_t index_va = state->index_va + draw->firstIndex * index_size;

            /* Handle draw calls with 0-sized index buffers if the GPU can't support them. */
            if (!remaining_indexes && pdev->info.has_zero_index_buffer_bug)
               radv_handle_zero_index_buffer_bug(cmd_buffer, &index_va, &remaining_indexes);

            const VkMultiDrawIndexedInfoEXT *next =
               (const VkMultiDrawIndexedInfoEXT *)(i < drawCount - 1 ? ((uint8_t *)draw + stride) : NULL);
            const bool offset_changes = next && next->vertexOffset != draw->vertexOffset;
            radv_emit_userdata_vertex(cmd_buffer, info, draw->vertexOffset);

            if (!state->render.view_mask) {
               radv_cs_emit_draw_indexed_packet(cmd_buffer, index_va, remaining_indexes, draw->indexCount,
                                                can_eop && !offset_changes && i < drawCount - 1);
            } else {
               u_foreach_bit (view, state->render.view_mask) {
                  radv_emit_view_index(&cmd_buffer->state, cs, view);

                  radv_cs_emit_draw_indexed_packet(cmd_buffer, index_va, remaining_indexes, draw->indexCount, false);
               }
            }
         }
      }
      if (drawCount > 1) {
         state->last_drawid = drawCount - 1;
      }
   }
}

ALWAYS_INLINE static void
radv_emit_direct_draw_packets(struct radv_cmd_buffer *cmd_buffer, const struct radv_draw_info *info, uint32_t drawCount,
                              const VkMultiDrawInfoEXT *minfo, uint32_t use_opaque, uint32_t stride)
{
   unsigned i = 0;
   const uint32_t view_mask = cmd_buffer->state.render.view_mask;
   const bool uses_drawid = cmd_buffer->state.uses_drawid;
   struct radv_cmd_stream *cs = cmd_buffer->cs;
   uint32_t last_start = 0;

   vk_foreach_multi_draw (draw, i, minfo, drawCount, stride) {
      if (!i)
         radv_emit_userdata_vertex(cmd_buffer, info, draw->firstVertex);
      else
         radv_emit_userdata_vertex_drawid(cmd_buffer, draw->firstVertex, uses_drawid ? i : 0);

      if (!view_mask) {
         radv_cs_emit_draw_packet(cmd_buffer, draw->vertexCount, use_opaque);
      } else {
         u_foreach_bit (view, view_mask) {
            radv_emit_view_index(&cmd_buffer->state, cs, view);
            radv_cs_emit_draw_packet(cmd_buffer, draw->vertexCount, use_opaque);
         }
      }
      last_start = draw->firstVertex;
   }
   if (drawCount > 1) {
      struct radv_cmd_state *state = &cmd_buffer->state;
      assert(state->last_vertex_offset_valid);
      state->last_vertex_offset = last_start;
      if (uses_drawid)
         state->last_drawid = drawCount - 1;
   }
}

static void
radv_cs_emit_mesh_dispatch_packet(struct radv_cmd_buffer *cmd_buffer, uint32_t x, uint32_t y, uint32_t z)
{
   const struct radv_device *device = radv_cmd_buffer_device(cmd_buffer);
   struct radv_cmd_stream *cs = cmd_buffer->cs;

   radeon_begin(cs);
   radeon_emit(PKT3(PKT3_DISPATCH_MESH_DIRECT, 3, cmd_buffer->state.predicating));
   radeon_emit(x);
   radeon_emit(y);
   radeon_emit(z);
   radeon_emit(S_0287F0_SOURCE_SELECT(V_0287F0_DI_SRC_SEL_AUTO_INDEX));
   radeon_end();

   radv_gfx12_emit_hiz_his_wa(device, &cmd_buffer->state, cs);
}

ALWAYS_INLINE static void
radv_emit_direct_mesh_draw_packet(struct radv_cmd_buffer *cmd_buffer, uint32_t x, uint32_t y, uint32_t z)
{
   struct radv_device *device = radv_cmd_buffer_device(cmd_buffer);
   const struct radv_physical_device *pdev = radv_device_physical(device);
   const uint32_t view_mask = cmd_buffer->state.render.view_mask;
   struct radv_cmd_stream *cs = cmd_buffer->cs;

   radv_emit_userdata_mesh(cmd_buffer, x, y, z);

   if (pdev->info.mesh_fast_launch_2) {
      if (!view_mask) {
         radv_cs_emit_mesh_dispatch_packet(cmd_buffer, x, y, z);
      } else {
         u_foreach_bit (view, view_mask) {
            radv_emit_view_index(&cmd_buffer->state, cs, view);
            radv_cs_emit_mesh_dispatch_packet(cmd_buffer, x, y, z);
         }
      }
   } else {
      const uint32_t count = x * y * z;
      if (!view_mask) {
         radv_cs_emit_draw_packet(cmd_buffer, count, 0);
      } else {
         u_foreach_bit (view, view_mask) {
            radv_emit_view_index(&cmd_buffer->state, cs, view);
            radv_cs_emit_draw_packet(cmd_buffer, count, 0);
         }
      }
   }
}

static void
radv_emit_indirect_buffer(struct radv_cmd_stream *cs, uint64_t va, bool is_compute)
{
   radeon_begin(cs);
   radeon_emit(PKT3(PKT3_SET_BASE, 2, 0) | (is_compute ? PKT3_SHADER_TYPE_S(1) : 0));
   radeon_emit(1);
   radeon_emit(va);
   radeon_emit(va >> 32);
   radeon_end();
}

ALWAYS_INLINE static void
radv_emit_indirect_mesh_draw_packets(struct radv_cmd_buffer *cmd_buffer, const struct radv_draw_info *info)
{
   const struct radv_cmd_state *state = &cmd_buffer->state;
   struct radv_cmd_stream *cs = cmd_buffer->cs;

   radv_emit_indirect_buffer(cs, info->indirect_va, false);

   if (state->uses_drawid) {
      const struct radv_shader *mesh_shader = state->shaders[MESA_SHADER_MESH];
      unsigned reg = state->vtx_base_sgpr + (mesh_shader->info.cs.uses_grid_size ? 12 : 0);

      radeon_begin(cs);
      radeon_set_sh_reg(reg, 0);
      radeon_end();
   }

   if (!state->render.view_mask) {
      radv_cs_emit_indirect_mesh_draw_packet(cmd_buffer, info->count, info->count_va, info->stride);
   } else {
      u_foreach_bit (i, state->render.view_mask) {
         radv_emit_view_index(&cmd_buffer->state, cs, i);
         radv_cs_emit_indirect_mesh_draw_packet(cmd_buffer, info->count, info->count_va, info->stride);
      }
   }
}

ALWAYS_INLINE static void
radv_emit_direct_taskmesh_draw_packets(const struct radv_device *device, struct radv_cmd_state *cmd_state,
                                       struct radv_cmd_stream *cs, struct radv_cmd_stream *ace_cs, uint32_t x,
                                       uint32_t y, uint32_t z)
{
   const uint32_t view_mask = cmd_state->render.view_mask;
   const unsigned num_views = MAX2(1, util_bitcount(view_mask));
   const unsigned ace_predication_size = num_views * 6; /* DISPATCH_TASKMESH_DIRECT_ACE size */

   radv_emit_userdata_task(cmd_state, ace_cs, x, y, z);
   radv_cs_emit_compute_predication(device, cmd_state, ace_cs, cmd_state->mec_inv_pred_va,
                                    &cmd_state->mec_inv_pred_emitted, ace_predication_size);

   if (!view_mask) {
      radv_cs_emit_dispatch_taskmesh_direct_ace_packet(device, cmd_state, ace_cs, x, y, z);
      radv_cs_emit_dispatch_taskmesh_gfx_packet(device, cmd_state, cs);
   } else {
      u_foreach_bit (view, view_mask) {
         radv_emit_view_index(cmd_state, cs, view);

         radv_cs_emit_dispatch_taskmesh_direct_ace_packet(device, cmd_state, ace_cs, x, y, z);
         radv_cs_emit_dispatch_taskmesh_gfx_packet(device, cmd_state, cs);
      }
   }
}

static void
radv_emit_indirect_taskmesh_draw_packets(const struct radv_device *device, struct radv_cmd_state *cmd_state,
                                         struct radv_cmd_stream *cs, struct radv_cmd_stream *ace_cs,
                                         const struct radv_draw_info *info, uint64_t workaround_cond_va)
{
   const struct radv_physical_device *pdev = radv_device_physical(device);
   const uint32_t view_mask = cmd_state->render.view_mask;
   const unsigned num_views = MAX2(1, util_bitcount(view_mask));
   unsigned ace_predication_size = num_views * 11; /* DISPATCH_TASKMESH_INDIRECT_MULTI_ACE size */

   if (pdev->info.has_taskmesh_indirect0_bug && info->count_va) {
      /* MEC firmware bug workaround.
       * When the count buffer contains zero, DISPATCH_TASKMESH_INDIRECT_MULTI_ACE hangs.
       * - We must ensure that DISPATCH_TASKMESH_INDIRECT_MULTI_ACE
       *   is only executed when the count buffer contains non-zero.
       * - Furthermore, we must also ensure that each DISPATCH_TASKMESH_GFX packet
       *   has a matching ACE packet.
       *
       * As a workaround:
       * - Reserve a dword in the upload buffer and initialize it to 1 for the workaround
       * - When count != 0, write 0 to the workaround BO and execute the indirect dispatch
       * - When workaround BO != 0 (count was 0), execute an empty direct dispatch
       */
      ac_emit_cp_copy_data(ace_cs->b, COPY_DATA_IMM, COPY_DATA_DST_MEM, 1, workaround_cond_va,
                           AC_CP_COPY_DATA_WR_CONFIRM);

      /* 2x COND_EXEC + 1x COPY_DATA + Nx DISPATCH_TASKMESH_DIRECT_ACE */
      ace_predication_size += 2 * 5 + 6 + 6 * num_views;
   }

   radv_cs_emit_compute_predication(device, cmd_state, ace_cs, cmd_state->mec_inv_pred_va,
                                    &cmd_state->mec_inv_pred_emitted, ace_predication_size);

   if (workaround_cond_va) {
      ac_emit_cond_exec(ace_cs->b, pdev->info.gfx_level, info->count_va,
                        6 + 11 * num_views /* 1x COPY_DATA + Nx DISPATCH_TASKMESH_INDIRECT_MULTI_ACE */);

      ac_emit_cp_copy_data(ace_cs->b, COPY_DATA_IMM, COPY_DATA_DST_MEM, 0, workaround_cond_va,
                           AC_CP_COPY_DATA_WR_CONFIRM);
   }

   if (!view_mask) {
      radv_cs_emit_dispatch_taskmesh_indirect_multi_ace_packet(device, cmd_state, ace_cs, info->indirect_va,
                                                               info->count, info->count_va, info->stride);
      radv_cs_emit_dispatch_taskmesh_gfx_packet(device, cmd_state, cs);
   } else {
      u_foreach_bit (view, view_mask) {
         radv_emit_view_index(cmd_state, cs, view);

         radv_cs_emit_dispatch_taskmesh_indirect_multi_ace_packet(device, cmd_state, ace_cs, info->indirect_va,
                                                                  info->count, info->count_va, info->stride);
         radv_cs_emit_dispatch_taskmesh_gfx_packet(device, cmd_state, cs);
      }
   }

   if (workaround_cond_va) {
      ac_emit_cond_exec(ace_cs->b, pdev->info.gfx_level, workaround_cond_va,
                        6 * num_views /* Nx DISPATCH_TASKMESH_DIRECT_ACE */);

      for (unsigned v = 0; v < num_views; ++v) {
         radv_cs_emit_dispatch_taskmesh_direct_ace_packet(device, cmd_state, ace_cs, 0, 0, 0);
      }
   }
}

static void
radv_emit_indirect_draw_packets(struct radv_cmd_buffer *cmd_buffer, const struct radv_draw_info *info)
{
   const struct radv_cmd_state *state = &cmd_buffer->state;
   struct radv_cmd_stream *cs = cmd_buffer->cs;

   radv_emit_indirect_buffer(cs, info->indirect_va, false);

   if (!state->render.view_mask) {
      radv_cs_emit_indirect_draw_packet(cmd_buffer, info->indexed, info->count, info->count_va, info->stride);
   } else {
      u_foreach_bit (i, state->render.view_mask) {
         radv_emit_view_index(&cmd_buffer->state, cs, i);

         radv_cs_emit_indirect_draw_packet(cmd_buffer, info->indexed, info->count, info->count_va, info->stride);
      }
   }
}

static uint64_t
radv_get_needed_dynamic_states(struct radv_cmd_buffer *cmd_buffer)
{
   struct radv_device *device = radv_cmd_buffer_device(cmd_buffer);
   const struct radv_physical_device *pdev = radv_device_physical(device);
   uint64_t dynamic_states;

   if (cmd_buffer->state.graphics_pipeline) {
      dynamic_states = cmd_buffer->state.graphics_pipeline->needed_dynamic_state;
   } else {
      dynamic_states = RADV_DYNAMIC_ALL;

      /* Clear unnecessary dynamic states for shader objects. */
      if (!cmd_buffer->state.shaders[MESA_SHADER_TESS_CTRL])
         dynamic_states &= ~(RADV_DYNAMIC_PATCH_CONTROL_POINTS | RADV_DYNAMIC_TESS_DOMAIN_ORIGIN);

      if (pdev->info.gfx_level >= GFX10_3) {
         if (cmd_buffer->state.shaders[MESA_SHADER_MESH])
            dynamic_states &= ~(RADV_DYNAMIC_VERTEX_INPUT | RADV_DYNAMIC_VERTEX_INPUT_BINDING_STRIDE |
                                RADV_DYNAMIC_PRIMITIVE_TOPOLOGY);
      } else {
         dynamic_states &= ~RADV_DYNAMIC_FRAGMENT_SHADING_RATE;
      }
   }

   /* Primitive restart enable is emitted as part of the draw registers. */
   return dynamic_states & ~RADV_DYNAMIC_PRIMITIVE_RESTART_ENABLE;
}

/*
 * Vega and raven have a bug which triggers if there are multiple context
 * register contexts active at the same time with different scissor values.
 *
 * There are two possible workarounds:
 * 1) Wait for PS_PARTIAL_FLUSH every time the scissor is changed. That way
 *    there is only ever 1 active set of scissor values at the same time.
 *
 * 2) Whenever the hardware switches contexts we have to set the scissor
 *    registers again even if it is a noop. That way the new context gets
 *    the correct scissor values.
 *
 * This implements option 2. radv_need_late_scissor_emission needs to
 * return true on affected HW if radv_emit_all_graphics_states sets
 * any context registers.
 */
static bool
radv_need_late_scissor_emission(struct radv_cmd_buffer *cmd_buffer, const struct radv_draw_info *info)
{
   if (cmd_buffer->cs->context_roll_without_scissor_emitted || info->strmout_va)
      return true;

   uint64_t used_dynamic_states = radv_get_needed_dynamic_states(cmd_buffer);

   used_dynamic_states &= ~RADV_DYNAMIC_VERTEX_INPUT;
   used_dynamic_states &= ~RADV_DYNAMIC_VERTEX_INPUT_BINDING_STRIDE;

   if (cmd_buffer->state.dirty_dynamic & used_dynamic_states)
      return true;

   /* Index, vertex and streamout buffers don't change context regs.
    * We assume that any other dirty flag causes context rolls.
    */
   uint64_t used_states = RADV_CMD_DIRTY_ALL;
   used_states &= ~(RADV_CMD_DIRTY_INDEX_BUFFER | RADV_CMD_DIRTY_VERTEX_BUFFER | RADV_CMD_DIRTY_STREAMOUT_BUFFER);

   return cmd_buffer->state.dirty & used_states;
}

ALWAYS_INLINE static uint32_t
radv_get_nggc_settings(struct radv_cmd_buffer *cmd_buffer, bool vp_y_inverted)
{
   const struct radv_dynamic_state *d = &cmd_buffer->state.dynamic;

   /* Disable shader culling entirely when conservative overestimate is used.
    * The face culling algorithm can delete very tiny triangles (even if unintended).
    */
   if (d->vk.rs.conservative_mode == VK_CONSERVATIVE_RASTERIZATION_MODE_OVERESTIMATE_EXT)
      return radv_nggc_none;

   /* With graphics pipeline library, NGG culling is unconditionally compiled into shaders
    * because we don't know the primitive topology at compile time, so we should
    * disable it dynamically for points or lines.
    */
   const unsigned num_vertices_per_prim = cmd_buffer->state.vgt_outprim_type + 1;
   if (num_vertices_per_prim != 3)
      return radv_nggc_none;

   /* Cull every triangle when rasterizer discard is enabled. */
   if (d->vk.rs.rasterizer_discard_enable)
      return radv_nggc_front_face | radv_nggc_back_face;

   uint32_t nggc_settings = radv_nggc_none;

   /* The culling code needs to know whether face is CW or CCW. */
   bool ccw = d->vk.rs.front_face == VK_FRONT_FACE_COUNTER_CLOCKWISE;

   /* Take inverted viewport into account. */
   ccw ^= vp_y_inverted;

   if (ccw)
      nggc_settings |= radv_nggc_face_is_ccw;

   /* Face culling settings. */
   if (d->vk.rs.cull_mode & VK_CULL_MODE_FRONT_BIT)
      nggc_settings |= radv_nggc_front_face;
   if (d->vk.rs.cull_mode & VK_CULL_MODE_BACK_BIT)
      nggc_settings |= radv_nggc_back_face;

   /* Small primitive culling assumes a sample position at (0.5, 0.5)
    * so don't enable it with user sample locations.
    */
   if (!d->vk.ms.sample_locations_enable) {
      nggc_settings |= radv_nggc_small_primitives;

      /* small_prim_precision = num_samples / 2^subpixel_bits
       * num_samples is also always a power of two, so the small prim precision can only be
       * a power of two between 2^-2 and 2^-6, therefore it's enough to remember the exponent.
       */
      unsigned rasterization_samples = cmd_buffer->state.num_rast_samples;
      unsigned subpixel_bits = 256;
      int32_t small_prim_precision_log2 = util_logbase2(rasterization_samples) - util_logbase2(subpixel_bits);
      nggc_settings |= ((uint32_t)small_prim_precision_log2 << 24u);
   }

   return nggc_settings;
}

static void
radv_emit_ps_state(struct radv_cmd_buffer *cmd_buffer)
{
   const struct radv_device *device = radv_cmd_buffer_device(cmd_buffer);
   const struct radv_physical_device *pdev = radv_device_physical(device);
   const struct radv_shader *ps = cmd_buffer->state.shaders[MESA_SHADER_FRAGMENT];

   if (!ps)
      return;

   const uint32_t ps_state_offset = radv_get_user_sgpr_loc(ps, AC_UD_PS_STATE);
   if (!ps_state_offset)
      return;

   const VkLineRasterizationModeEXT line_rast_mode = cmd_buffer->state.line_rast_mode;
   const unsigned rasterization_samples = cmd_buffer->state.num_rast_samples;
   const unsigned ps_iter_samples = radv_get_ps_iter_samples(cmd_buffer);
   const uint16_t ps_iter_mask = ac_get_ps_iter_mask(ps_iter_samples);
   const unsigned vgt_outprim_type = cmd_buffer->state.vgt_outprim_type;
   const unsigned ps_state = SET_SGPR_FIELD(PS_STATE_NUM_SAMPLES, rasterization_samples) |
                             SET_SGPR_FIELD(PS_STATE_PS_ITER_MASK, ps_iter_mask) |
                             SET_SGPR_FIELD(PS_STATE_LINE_RAST_MODE, line_rast_mode) |
                             SET_SGPR_FIELD(PS_STATE_RAST_PRIM, vgt_outprim_type);

   radeon_begin(cmd_buffer->cs);
   if (pdev->info.gfx_level >= GFX12) {
      gfx12_push_sh_reg(ps_state_offset, ps_state);
   } else {
      radeon_set_sh_reg(ps_state_offset, ps_state);
   }
   radeon_end();
}

static uint32_t
radv_get_ngg_state_num_verts_per_prim(struct radv_cmd_buffer *cmd_buffer)
{
   const struct radv_shader *last_vgt_shader = cmd_buffer->state.last_vgt_shader;
   uint32_t num_verts_per_prim = 0;

   if (last_vgt_shader->info.stage == MESA_SHADER_VERTEX)
      num_verts_per_prim = cmd_buffer->state.vgt_outprim_type + 1;

   return num_verts_per_prim;
}

static uint32_t
radv_get_ngg_state_provoking_vtx(struct radv_cmd_buffer *cmd_buffer)
{
   const struct radv_shader *last_vgt_shader = cmd_buffer->state.last_vgt_shader;
   const struct radv_dynamic_state *d = &cmd_buffer->state.dynamic;
   const unsigned stage = last_vgt_shader->info.stage;
   unsigned provoking_vtx = 0;

   if (d->vk.rs.provoking_vertex == VK_PROVOKING_VERTEX_MODE_LAST_VERTEX_EXT) {
      if (stage == MESA_SHADER_VERTEX) {
         provoking_vtx = cmd_buffer->state.vgt_outprim_type;
      } else if (stage == MESA_SHADER_GEOMETRY) {
         provoking_vtx = last_vgt_shader->info.gs.vertices_in - 1;
      }
   }

   return provoking_vtx;
}

static uint32_t
radv_get_ngg_state_query(struct radv_cmd_buffer *cmd_buffer)
{
   struct radv_device *device = radv_cmd_buffer_device(cmd_buffer);
   const struct radv_physical_device *pdev = radv_device_physical(device);
   enum radv_shader_query_state shader_query_state = radv_shader_query_none;

   /* By default shader queries are disabled but they are enabled if the command buffer has active GDS
    * queries or if it's a secondary command buffer that inherits the number of generated
    * primitives.
    */
   if (cmd_buffer->state.active_emulated_pipeline_queries ||
       (cmd_buffer->state.inherited_pipeline_statistics & VK_QUERY_PIPELINE_STATISTIC_GEOMETRY_SHADER_PRIMITIVES_BIT) ||
       (pdev->emulate_mesh_shader_queries && (cmd_buffer->state.inherited_pipeline_statistics &
                                              VK_QUERY_PIPELINE_STATISTIC_MESH_SHADER_INVOCATIONS_BIT_EXT)))
      shader_query_state |= radv_shader_query_pipeline_stat;

   if (cmd_buffer->state.active_emulated_prims_gen_queries)
      shader_query_state |= radv_shader_query_prim_gen;

   if (cmd_buffer->state.active_emulated_prims_xfb_queries && radv_is_streamout_enabled(cmd_buffer))
      shader_query_state |= radv_shader_query_prim_xfb | radv_shader_query_prim_gen;

   return shader_query_state;
}

static void
radv_emit_ngg_state(struct radv_cmd_buffer *cmd_buffer)
{
   const struct radv_device *device = radv_cmd_buffer_device(cmd_buffer);
   const struct radv_physical_device *pdev = radv_device_physical(device);
   const struct radv_shader *last_vgt_shader = cmd_buffer->state.last_vgt_shader;

   const uint32_t ngg_state_offset = radv_get_user_sgpr_loc(last_vgt_shader, AC_UD_NGG_STATE);
   if (!ngg_state_offset)
      return;

   const uint32_t ngg_state =
      SET_SGPR_FIELD(NGG_STATE_NUM_VERTS_PER_PRIM, radv_get_ngg_state_num_verts_per_prim(cmd_buffer)) |
      SET_SGPR_FIELD(NGG_STATE_PROVOKING_VTX, radv_get_ngg_state_provoking_vtx(cmd_buffer)) |
      SET_SGPR_FIELD(NGG_STATE_QUERY, radv_get_ngg_state_query(cmd_buffer));

   const uint32_t ngg_query_buf_va_offset = radv_get_user_sgpr_loc(last_vgt_shader, AC_UD_NGG_QUERY_BUF_VA);

   radeon_begin(cmd_buffer->cs);
   if (pdev->info.gfx_level >= GFX12) {
      gfx12_push_sh_reg(ngg_state_offset, ngg_state);
      if (ngg_query_buf_va_offset)
         gfx12_push_sh_reg(ngg_query_buf_va_offset, cmd_buffer->state.shader_query_buf_va);
   } else {
      radeon_set_sh_reg(ngg_state_offset, ngg_state);
      if (ngg_query_buf_va_offset)
         radeon_set_sh_reg(ngg_query_buf_va_offset, cmd_buffer->state.shader_query_buf_va);
   }
   radeon_end();
}

static bool
radv_is_viewport_y_inverted(struct radv_cmd_buffer *cmd_buffer)
{
   const struct radv_dynamic_state *d = &cmd_buffer->state.dynamic;
   const float y_scale = d->vp_xform[0].scale[1];
   const float y_translate = d->vp_xform[0].translate[1];

   return (-y_scale + y_translate) > (y_scale + y_translate);
}

static void
radv_emit_nggc_settings(struct radv_cmd_buffer *cmd_buffer)
{
   const struct radv_shader *last_vgt_shader = cmd_buffer->state.last_vgt_shader;

   const uint32_t nggc_settings_offset = radv_get_user_sgpr_loc(last_vgt_shader, AC_UD_NGGC_SETTINGS);
   if (!nggc_settings_offset)
      return;

   const bool vp_y_inverted = radv_is_viewport_y_inverted(cmd_buffer);
   const uint32_t nggc_settings = radv_get_nggc_settings(cmd_buffer, vp_y_inverted);

   radeon_begin(cmd_buffer->cs);
   radeon_set_sh_reg(nggc_settings_offset, nggc_settings);
   radeon_end();
}

static void
radv_emit_nggc_viewport(struct radv_cmd_buffer *cmd_buffer)
{
   const struct radv_shader *last_vgt_shader = cmd_buffer->state.last_vgt_shader;
   const struct radv_dynamic_state *d = &cmd_buffer->state.dynamic;

   const uint32_t nggc_viewport_offset = radv_get_user_sgpr_loc(last_vgt_shader, AC_UD_NGGC_VIEWPORT);
   if (!nggc_viewport_offset)
      return;

   /* Get viewport transform. */
   float vp_scale[2], vp_translate[2];
   memcpy(vp_scale, d->vp_xform[0].scale, 2 * sizeof(float));
   memcpy(vp_translate, d->vp_xform[0].translate, 2 * sizeof(float));

   /* Correction for inverted Y */
   if (radv_is_viewport_y_inverted(cmd_buffer)) {
      vp_scale[1] = -vp_scale[1];
      vp_translate[1] = -vp_translate[1];
   }

   /* Correction for number of samples per pixel. */
   for (unsigned i = 0; i < 2; ++i) {
      vp_scale[i] *= (float)d->vk.ms.rasterization_samples;
      vp_translate[i] *= (float)d->vk.ms.rasterization_samples;
   }

   const uint32_t vp_reg_values[4] = {fui(vp_scale[0]), fui(vp_scale[1]), fui(vp_translate[0]), fui(vp_translate[1])};

   radeon_begin(cmd_buffer->cs);
   radeon_set_sh_reg_seq(nggc_viewport_offset, 4);
   radeon_emit_array(vp_reg_values, 4);
   radeon_end();
}

static void
radv_emit_task_state(struct radv_cmd_buffer *cmd_buffer)
{
   struct radv_device *device = radv_cmd_buffer_device(cmd_buffer);
   const struct radv_physical_device *pdev = radv_device_physical(device);
   const struct radv_shader *task_shader = cmd_buffer->state.shaders[MESA_SHADER_TASK];

   if (!task_shader || !pdev->emulate_mesh_shader_queries)
      return;

   const uint32_t task_state_offset = radv_get_user_sgpr_loc(task_shader, AC_UD_TASK_STATE);
   enum radv_shader_query_state shader_query_state = radv_shader_query_none;

   if (!task_state_offset)
      return;

   /* By default shader queries are disabled but they are enabled if the command buffer has active ACE
    * queries or if it's a secondary command buffer that inherits the number of task shader
    * invocations query.
    */
   if (cmd_buffer->state.active_pipeline_ace_queries ||
       (cmd_buffer->state.inherited_pipeline_statistics & VK_QUERY_PIPELINE_STATISTIC_TASK_SHADER_INVOCATIONS_BIT_EXT))
      shader_query_state |= radv_shader_query_pipeline_stat;

   radeon_begin(cmd_buffer->gang.cs);
   if (pdev->info.gfx_level >= GFX12) {
      gfx12_push_sh_reg(task_state_offset, shader_query_state);
   } else {
      radeon_set_sh_reg(task_state_offset, shader_query_state);
   }
   radeon_end();
}

static void
radv_emit_tcs_tes_state(struct radv_cmd_buffer *cmd_buffer)
{
   const struct radv_device *device = radv_cmd_buffer_device(cmd_buffer);
   const struct radv_physical_device *pdev = radv_device_physical(device);
   const struct radv_shader *vs = radv_get_shader(cmd_buffer->state.shaders, MESA_SHADER_VERTEX);
   const struct radv_shader *tcs = cmd_buffer->state.shaders[MESA_SHADER_TESS_CTRL];
   const struct radv_shader *tes = radv_get_shader(cmd_buffer->state.shaders, MESA_SHADER_TESS_EVAL);
   const struct radv_dynamic_state *d = &cmd_buffer->state.dynamic;
   struct radv_cmd_stream *cs = cmd_buffer->cs;
   uint32_t tcs_offchip_layout = 0, tes_offchip_layout = 0;
   uint32_t pgm_hs_rsrc2 = 0;

   if (!tcs)
      return;

   unsigned lds_alloc =
      ac_shader_encode_lds_size(cmd_buffer->state.tess_lds_size, pdev->info.gfx_level, MESA_SHADER_VERTEX);

   if (pdev->info.gfx_level >= GFX9) {
      if (tcs->info.merged_shader_compiled_separately) {
         radv_shader_combine_cfg_vs_tcs(cmd_buffer->state.shaders[MESA_SHADER_VERTEX], tcs, NULL, &pgm_hs_rsrc2);
      } else {
         pgm_hs_rsrc2 = tcs->config.rsrc2;
      }

      if (pdev->info.gfx_level >= GFX10) {
         pgm_hs_rsrc2 |= S_00B42C_LDS_SIZE_GFX10(lds_alloc);
      } else {
         pgm_hs_rsrc2 |= S_00B42C_LDS_SIZE_GFX9(lds_alloc);
      }
   }

   const uint32_t tcs_offchip_layout_offset = radv_get_user_sgpr_loc(tcs, AC_UD_TCS_OFFCHIP_LAYOUT);
   const uint32_t tes_offchip_layout_offset = radv_get_user_sgpr_loc(tes, AC_UD_TCS_OFFCHIP_LAYOUT);
   if (tcs_offchip_layout_offset) {
      unsigned tcs_out_mem_attrib_stride =
         align(cmd_buffer->state.tess_num_patches * tcs->info.tcs.tcs_vertices_out * 16, 256) / 256;

      uint32_t tmp =
         SET_SGPR_FIELD(TCS_OFFCHIP_LAYOUT_NUM_PATCHES, cmd_buffer->state.tess_num_patches) |
         SET_SGPR_FIELD(TCS_OFFCHIP_LAYOUT_TCS_MEM_ATTRIB_STRIDE, tcs_out_mem_attrib_stride) |
         SET_SGPR_FIELD(TCS_OFFCHIP_LAYOUT_NUM_LS_OUTPUTS, vs->info.vs.num_linked_outputs) |
         SET_SGPR_FIELD(TCS_OFFCHIP_LAYOUT_NUM_HS_OUTPUTS, tcs->info.tcs.io_info.highest_remapped_vram_output) |
         SET_SGPR_FIELD(TCS_OFFCHIP_LAYOUT_TES_READS_TF, tes->info.tes.reads_tess_factors) |
         SET_SGPR_FIELD(TCS_OFFCHIP_LAYOUT_PRIMITIVE_MODE, tes->info.tes._primitive_mode);
      tcs_offchip_layout =
         tmp | SET_SGPR_FIELD(TCS_OFFCHIP_LAYOUT_PATCH_VERTICES_IN, d->vk.ts.patch_control_points - 1);
      tes_offchip_layout =
         tmp | SET_SGPR_FIELD(TCS_OFFCHIP_LAYOUT_PATCH_VERTICES_IN, tcs->info.tcs.tcs_vertices_out - 1);
      assert(tes_offchip_layout_offset);
   }

   radeon_begin(cs);
   if (pdev->info.gfx_level >= GFX12) {
      gfx12_push_sh_reg(tcs->info.regs.pgm_rsrc2, pgm_hs_rsrc2);
      if (tcs_offchip_layout || tes_offchip_layout) {
         gfx12_push_sh_reg(tcs_offchip_layout_offset, tcs_offchip_layout);
         gfx12_push_sh_reg(tes_offchip_layout_offset, tes_offchip_layout);
      }
   } else {
      if (pdev->info.gfx_level >= GFX9) {
         radeon_set_sh_reg(tcs->info.regs.pgm_rsrc2, pgm_hs_rsrc2);
      } else {
         const uint32_t ls_rsrc2 = vs->config.rsrc2 | S_00B52C_LDS_SIZE(lds_alloc);

         radeon_set_sh_reg(vs->info.regs.pgm_rsrc2, ls_rsrc2);
      }

      if (tcs_offchip_layout || tes_offchip_layout) {
         radeon_set_sh_reg(tcs_offchip_layout_offset, tcs_offchip_layout);
         radeon_set_sh_reg(tes_offchip_layout_offset, tes_offchip_layout);
      }
   }
   radeon_end();
}

static void
radv_emit_force_vrs_state(struct radv_cmd_buffer *cmd_buffer)
{
   const struct radv_device *device = radv_cmd_buffer_device(cmd_buffer);
   const struct radv_physical_device *pdev = radv_device_physical(device);
   struct radv_cmd_stream *cs = cmd_buffer->cs;
   uint32_t force_vrs_rates_offset;
   struct radv_shader *shader;
   uint32_t vrs_rates = 0;

   shader = cmd_buffer->state.gs_copy_shader ? cmd_buffer->state.gs_copy_shader : cmd_buffer->state.last_vgt_shader;
   if (!shader)
      return;

   force_vrs_rates_offset = radv_get_user_sgpr_loc(shader, AC_UD_FORCE_VRS_RATES);
   if (!force_vrs_rates_offset)
      return;

   switch (device->force_vrs) {
   case RADV_FORCE_VRS_2x2:
      vrs_rates = pdev->info.gfx_level >= GFX11 ? V_0283D0_VRS_SHADING_RATE_2X2 : (1u << 2) | (1u << 4);
      break;
   case RADV_FORCE_VRS_2x1:
      vrs_rates = pdev->info.gfx_level >= GFX11 ? V_0283D0_VRS_SHADING_RATE_2X1 : (1u << 2) | (0u << 4);
      break;
   case RADV_FORCE_VRS_1x2:
      vrs_rates = pdev->info.gfx_level >= GFX11 ? V_0283D0_VRS_SHADING_RATE_1X2 : (0u << 2) | (1u << 4);
      break;
   default:
      break;
   }

   radeon_begin(cs);
   if (pdev->info.gfx_level >= GFX12) {
      gfx12_push_sh_reg(force_vrs_rates_offset, vrs_rates);
   } else {
      radeon_set_sh_reg(force_vrs_rates_offset, vrs_rates);
   }
   radeon_end();
}

static void
radv_emit_shaders_state(struct radv_cmd_buffer *cmd_buffer)
{
   if (cmd_buffer->state.dirty & RADV_CMD_DIRTY_PS_STATE) {
      radv_emit_ps_state(cmd_buffer);
      cmd_buffer->state.dirty &= ~RADV_CMD_DIRTY_PS_STATE;
   }

   if (cmd_buffer->state.dirty & RADV_CMD_DIRTY_PS_EPILOG_STATE) {
      radv_emit_ps_epilog_state(cmd_buffer);
      cmd_buffer->state.dirty &= ~RADV_CMD_DIRTY_PS_EPILOG_STATE;
   }

   if (cmd_buffer->state.dirty & RADV_CMD_DIRTY_NGG_STATE) {
      radv_emit_ngg_state(cmd_buffer);
      cmd_buffer->state.dirty &= ~RADV_CMD_DIRTY_NGG_STATE;
   }

   if (cmd_buffer->state.dirty & RADV_CMD_DIRTY_NGGC_SETTINGS) {
      radv_emit_nggc_settings(cmd_buffer);
      cmd_buffer->state.dirty &= ~RADV_CMD_DIRTY_NGGC_SETTINGS;
   }

   if (cmd_buffer->state.dirty & RADV_CMD_DIRTY_NGGC_VIEWPORT) {
      radv_emit_nggc_viewport(cmd_buffer);
      cmd_buffer->state.dirty &= ~RADV_CMD_DIRTY_NGGC_VIEWPORT;
   }

   if (cmd_buffer->state.dirty & RADV_CMD_DIRTY_TASK_STATE) {
      radv_emit_task_state(cmd_buffer);
      cmd_buffer->state.dirty &= ~RADV_CMD_DIRTY_TASK_STATE;
   }

   if (cmd_buffer->state.dirty & RADV_CMD_DIRTY_TCS_TES_STATE) {
      radv_emit_tcs_tes_state(cmd_buffer);
      cmd_buffer->state.dirty &= ~RADV_CMD_DIRTY_TCS_TES_STATE;
   }

   if (cmd_buffer->state.dirty & RADV_CMD_DIRTY_FORCE_VRS_STATE) {
      radv_emit_force_vrs_state(cmd_buffer);
      cmd_buffer->state.dirty &= ~RADV_CMD_DIRTY_FORCE_VRS_STATE;
   }
}

static void
radv_emit_db_shader_control(struct radv_cmd_buffer *cmd_buffer)
{
   struct radv_device *device = radv_cmd_buffer_device(cmd_buffer);
   const struct radv_physical_device *pdev = radv_device_physical(device);
   const struct radeon_info *gpu_info = &pdev->info;
   const struct radv_shader *ps = cmd_buffer->state.shaders[MESA_SHADER_FRAGMENT];
   const struct radv_dynamic_state *d = &cmd_buffer->state.dynamic;
   const bool uses_ds_feedback_loop =
      !!(d->feedback_loop_aspects & (VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT));
   const VkLineRasterizationModeEXT line_rast_mode = cmd_buffer->state.line_rast_mode;
   const unsigned rasterization_samples = cmd_buffer->state.num_rast_samples;
   uint32_t db_dfsm_control = S_028060_PUNCHOUT_MODE(V_028060_FORCE_OFF);
   uint32_t db_shader_control;

   if (ps) {
      db_shader_control = ps->info.regs.ps.db_shader_control;
   } else {
      db_shader_control = S_02880C_CONSERVATIVE_Z_EXPORT(V_02880C_EXPORT_ANY_Z) |
                          S_02880C_Z_ORDER(V_02880C_EARLY_Z_THEN_LATE_Z) |
                          S_02880C_DUAL_QUAD_DISABLE(gpu_info->has_rbplus && !gpu_info->rbplus_allowed);
   }

   /* When a depth/stencil attachment is used inside feedback loops, use LATE_Z to make sure shader invocations read the
    * correct value.
    * Also apply the bug workaround for smoothing (overrasterization) on GFX6.
    */
   if (uses_ds_feedback_loop ||
       (gpu_info->gfx_level == GFX6 && line_rast_mode == VK_LINE_RASTERIZATION_MODE_RECTANGULAR_SMOOTH))
      db_shader_control = (db_shader_control & C_02880C_Z_ORDER) | S_02880C_Z_ORDER(V_02880C_LATE_Z);

   if (ps && ps->info.ps.pops) {
      /* POPS_OVERLAP_NUM_SAMPLES (OVERRIDE_INTRINSIC_RATE on GFX11, must always be enabled for POPS) controls the
       * interlock granularity.
       * PixelInterlock: 1x.
       * SampleInterlock: MSAA_EXPOSED_SAMPLES (much faster at common edges of adjacent primitives with MSAA).
       */
      if (gpu_info->gfx_level >= GFX11) {
         db_shader_control |= S_02880C_OVERRIDE_INTRINSIC_RATE_ENABLE(1);
         if (ps->info.ps.pops_is_per_sample)
            db_shader_control |= S_02880C_OVERRIDE_INTRINSIC_RATE(util_logbase2(rasterization_samples));
      } else {
         if (ps->info.ps.pops_is_per_sample)
            db_shader_control |= S_02880C_POPS_OVERLAP_NUM_SAMPLES(util_logbase2(rasterization_samples));

         if (gpu_info->has_pops_missed_overlap_bug)
            db_dfsm_control |= S_028060_POPS_DRAIN_PS_ON_OVERLAP(rasterization_samples >= 8);
      }
   } else if (gpu_info->has_export_conflict_bug && rasterization_samples == 1) {
      for (uint32_t i = 0; i < MAX_RTS; i++) {
         if (((d->color_write_mask >> (4 * i)) & 0xfu) && ((d->color_blend_enable >> i) & 0x1u)) {
            db_shader_control |= S_02880C_OVERRIDE_INTRINSIC_RATE_ENABLE(1) | S_02880C_OVERRIDE_INTRINSIC_RATE(2);
            break;
         }
      }
   }

   /* Use the alpha value from MRTZ.a for alpha-to-coverage when alpha-to-one is also enabled.
    * GFX11+ selects MRTZ.a by default if present.
    */
   db_shader_control |= S_02880C_COVERAGE_TO_MASK_ENABLE(
      pdev->info.gfx_level < GFX11 && d->vk.ms.alpha_to_coverage_enable && d->vk.ms.alpha_to_one_enable);

   radeon_begin(cmd_buffer->cs);

   if (pdev->info.gfx_level >= GFX12) {
      radeon_opt_set_context_reg(R_02806C_DB_SHADER_CONTROL, RADV_TRACKED_DB_SHADER_CONTROL, db_shader_control);
   } else {
      radeon_opt_set_context_reg(R_02880C_DB_SHADER_CONTROL, RADV_TRACKED_DB_SHADER_CONTROL, db_shader_control);

      if (gpu_info->has_pops_missed_overlap_bug)
         radeon_set_context_reg(R_028060_DB_DFSM_CONTROL, db_dfsm_control);
   }

   radeon_end();
}

static void
radv_emit_streamout_enable_state(struct radv_cmd_buffer *cmd_buffer)
{
   struct radv_device *device = radv_cmd_buffer_device(cmd_buffer);
   const struct radv_physical_device *pdev = radv_device_physical(device);
   const struct radv_streamout_state *so = &cmd_buffer->state.streamout;
   const bool streamout_enabled = radv_is_streamout_enabled(cmd_buffer);
   uint32_t enabled_stream_buffers_mask = 0;

   assert(!pdev->use_ngg_streamout);

   radeon_begin(cmd_buffer->cs);

   if (streamout_enabled && cmd_buffer->state.last_vgt_shader) {
      const struct radv_shader_info *info = &cmd_buffer->state.last_vgt_shader->info;

      enabled_stream_buffers_mask = info->so.enabled_stream_buffers_mask;

      u_foreach_bit (i, so->enabled_mask) {
         radeon_set_context_reg(R_028AD4_VGT_STRMOUT_VTX_STRIDE_0 + 16 * i, info->so.strides[i]);
      }
   }

   radeon_set_context_reg_seq(R_028B94_VGT_STRMOUT_CONFIG, 2);
   radeon_emit(S_028B94_STREAMOUT_0_EN(streamout_enabled) | S_028B94_RAST_STREAM(0) |
               S_028B94_STREAMOUT_1_EN(streamout_enabled) | S_028B94_STREAMOUT_2_EN(streamout_enabled) |
               S_028B94_STREAMOUT_3_EN(streamout_enabled));
   radeon_emit(so->hw_enabled_mask & enabled_stream_buffers_mask);
   radeon_end();
}

static unsigned
radv_compact_spi_shader_col_format(uint32_t spi_shader_col_format)
{
   unsigned value = 0, num_mrts = 0;
   unsigned i, num_targets;

   /* Compute the number of MRTs. */
   num_targets = DIV_ROUND_UP(util_last_bit(spi_shader_col_format), 4);

   /* Remove holes in spi_shader_col_format. */
   for (i = 0; i < num_targets; i++) {
      unsigned spi_format = (spi_shader_col_format >> (i * 4)) & 0xf;

      if (spi_format) {
         value |= spi_format << (num_mrts * 4);
         num_mrts++;
      }
   }

   return value;
}

static void
radv_emit_fragment_output_state(struct radv_cmd_buffer *cmd_buffer)
{
   const struct radv_device *device = radv_cmd_buffer_device(cmd_buffer);
   const struct radv_physical_device *pdev = radv_device_physical(device);

   uint32_t col_format_compacted = radv_compact_spi_shader_col_format(cmd_buffer->state.spi_shader_col_format);

   if (pdev->info.gfx_level >= GFX12) {
      radeon_begin(cmd_buffer->cs);
      gfx12_begin_context_regs();
      gfx12_set_context_reg(R_028854_CB_SHADER_MASK, cmd_buffer->state.cb_shader_mask);
      gfx12_set_context_reg(R_028650_SPI_SHADER_Z_FORMAT, cmd_buffer->state.spi_shader_z_format);
      gfx12_set_context_reg(R_028654_SPI_SHADER_COL_FORMAT, col_format_compacted);
      gfx12_end_context_regs();
      radeon_end();
   } else {
      radeon_begin(cmd_buffer->cs);
      radeon_opt_set_context_reg(R_02823C_CB_SHADER_MASK, RADV_TRACKED_CB_SHADER_MASK,
                                 cmd_buffer->state.cb_shader_mask);
      radeon_opt_set_context_reg2(R_028710_SPI_SHADER_Z_FORMAT, RADV_TRACKED_SPI_SHADER_Z_FORMAT,
                                  cmd_buffer->state.spi_shader_z_format, col_format_compacted);
      radeon_end();
   }
}

static void
radv_emit_depth_stencil_state(struct radv_cmd_buffer *cmd_buffer)
{
   const struct radv_device *device = radv_cmd_buffer_device(cmd_buffer);
   const struct radv_physical_device *pdev = radv_device_physical(device);
   const struct radv_rendering_state *render = &cmd_buffer->state.render;
   const struct radv_dynamic_state *d = &cmd_buffer->state.dynamic;
   struct vk_depth_stencil_state ds = d->vk.ds;

   vk_optimize_depth_stencil_state(&ds, render->ds_att_aspects, true);

   const uint32_t db_depth_control =
      S_028800_Z_ENABLE(ds.depth.test_enable) | S_028800_Z_WRITE_ENABLE(ds.depth.write_enable) |
      S_028800_ZFUNC(ds.depth.compare_op) | S_028800_DEPTH_BOUNDS_ENABLE(ds.depth.bounds_test.enable) |
      S_028800_STENCIL_ENABLE(ds.stencil.test_enable) | S_028800_BACKFACE_ENABLE(ds.stencil.test_enable) |
      S_028800_STENCILFUNC(ds.stencil.front.op.compare) | S_028800_STENCILFUNC_BF(ds.stencil.back.op.compare);

   const uint32_t db_stencil_control =
      S_02842C_STENCILFAIL(ds.stencil.front.op.fail) | S_02842C_STENCILZPASS(ds.stencil.front.op.pass) |
      S_02842C_STENCILZFAIL(ds.stencil.front.op.depth_fail) | S_02842C_STENCILFAIL_BF(ds.stencil.back.op.fail) |
      S_02842C_STENCILZPASS_BF(ds.stencil.back.op.pass) | S_02842C_STENCILZFAIL_BF(ds.stencil.back.op.depth_fail);

   const uint32_t depth_bounds_min = fui(ds.depth.bounds_test.min);
   const uint32_t depth_bounds_max = fui(ds.depth.bounds_test.max);

   if (pdev->info.gfx_level >= GFX12) {
      const bool force_s_valid =
         ds.stencil.test_enable && ((ds.stencil.front.op.pass != ds.stencil.front.op.depth_fail) ||
                                    (ds.stencil.back.op.pass != ds.stencil.back.op.depth_fail));

      radeon_begin(cmd_buffer->cs);
      gfx12_begin_context_regs();
      gfx12_opt_set_context_reg(R_02800C_DB_RENDER_OVERRIDE, RADV_TRACKED_DB_RENDER_OVERRIDE,
                                S_02800C_FORCE_STENCIL_READ(1) | S_02800C_FORCE_STENCIL_VALID(force_s_valid));

      gfx12_opt_set_context_reg(R_028070_DB_DEPTH_CONTROL, RADV_TRACKED_DB_DEPTH_CONTROL, db_depth_control);

      if (ds.stencil.test_enable) {
         gfx12_opt_set_context_reg(R_028074_DB_STENCIL_CONTROL, RADV_TRACKED_DB_STENCIL_CONTROL, db_stencil_control);

         gfx12_opt_set_context_reg(
            R_028088_DB_STENCIL_REF, RADV_TRACKED_DB_STENCIL_REF,
            S_028088_TESTVAL(ds.stencil.front.reference) | S_028088_TESTVAL_BF(ds.stencil.back.reference));

         gfx12_opt_set_context_reg2(
            R_028090_DB_STENCIL_READ_MASK, RADV_TRACKED_DB_STENCIL_READ_MASK,
            S_028090_TESTMASK(ds.stencil.front.compare_mask) | S_028090_TESTMASK_BF(ds.stencil.back.compare_mask),
            S_028094_WRITEMASK(ds.stencil.front.write_mask) | S_028094_WRITEMASK_BF(ds.stencil.back.write_mask));
      }

      if (ds.depth.bounds_test.enable) {
         gfx12_opt_set_context_reg2(R_028050_DB_DEPTH_BOUNDS_MIN, RADV_TRACKED_DB_DEPTH_BOUNDS_MIN, depth_bounds_min,
                                    depth_bounds_max);
      }
      gfx12_end_context_regs();
      radeon_end();
   } else {
      radeon_begin(cmd_buffer->cs);
      radeon_opt_set_context_reg(R_028800_DB_DEPTH_CONTROL, RADV_TRACKED_DB_DEPTH_CONTROL, db_depth_control);

      if (ds.stencil.test_enable) {
         radeon_opt_set_context_reg(R_02842C_DB_STENCIL_CONTROL, RADV_TRACKED_DB_STENCIL_CONTROL, db_stencil_control);

         radeon_opt_set_context_reg2(
            R_028430_DB_STENCILREFMASK, RADV_TRACKED_DB_STENCILREFMASK,
            S_028430_STENCILTESTVAL(ds.stencil.front.reference) | S_028430_STENCILMASK(ds.stencil.front.compare_mask) |
               S_028430_STENCILWRITEMASK(ds.stencil.front.write_mask) | S_028430_STENCILOPVAL(1),
            S_028434_STENCILTESTVAL_BF(ds.stencil.back.reference) |
               S_028434_STENCILMASK_BF(ds.stencil.back.compare_mask) |
               S_028434_STENCILWRITEMASK_BF(ds.stencil.back.write_mask) | S_028434_STENCILOPVAL_BF(1));
      }

      if (ds.depth.bounds_test.enable) {
         radeon_opt_set_context_reg2(R_028020_DB_DEPTH_BOUNDS_MIN, RADV_TRACKED_DB_DEPTH_BOUNDS_MIN, depth_bounds_min,
                                     depth_bounds_max);
      }
      radeon_end();
   }
}

static void
radv_emit_raster_state(struct radv_cmd_buffer *cmd_buffer)
{
   const struct radv_device *device = radv_cmd_buffer_device(cmd_buffer);
   const struct radv_physical_device *pdev = radv_device_physical(device);
   const struct radv_dynamic_state *d = &cmd_buffer->state.dynamic;
   const bool depth_clip_enable = cmd_buffer->state.depth_clip_enable;
   const VkLineRasterizationModeEXT line_rast_mode = cmd_buffer->state.line_rast_mode;

   /* GFX9 chips fail linestrip CTS tests unless this is set to 0 = no reset */
   uint32_t auto_reset_cntl = (pdev->info.gfx_level == GFX9) ? 0 : 2;

   if (radv_primitive_topology_is_line_list(d->vk.ia.primitive_topology))
      auto_reset_cntl = 1;

   unsigned pa_su_sc_mode_cntl =
      S_028814_CULL_FRONT(!!(d->vk.rs.cull_mode & VK_CULL_MODE_FRONT_BIT)) |
      S_028814_CULL_BACK(!!(d->vk.rs.cull_mode & VK_CULL_MODE_BACK_BIT)) | S_028814_FACE(d->vk.rs.front_face) |
      S_028814_POLY_OFFSET_FRONT_ENABLE(d->vk.rs.depth_bias.enable) |
      S_028814_POLY_OFFSET_BACK_ENABLE(d->vk.rs.depth_bias.enable) |
      S_028814_POLY_OFFSET_PARA_ENABLE(d->vk.rs.depth_bias.enable) |
      S_028814_POLY_MODE(d->vk.rs.polygon_mode != V_028814_X_DRAW_TRIANGLES) |
      S_028814_POLYMODE_FRONT_PTYPE(d->vk.rs.polygon_mode) | S_028814_POLYMODE_BACK_PTYPE(d->vk.rs.polygon_mode) |
      S_028814_PROVOKING_VTX_LAST(d->vk.rs.provoking_vertex == VK_PROVOKING_VERTEX_MODE_LAST_VERTEX_EXT);

   if (pdev->info.gfx_level >= GFX10 && pdev->info.gfx_level < GFX12) {
      /* Ensure that SC processes the primitive group in the same order as PA produced them.  Needed
       * when either POLY_MODE or PERPENDICULAR_ENDCAP_ENA is set.
       */
      pa_su_sc_mode_cntl |= S_028814_KEEP_TOGETHER_ENABLE(d->vk.rs.polygon_mode != V_028814_X_DRAW_TRIANGLES ||
                                                          line_rast_mode == VK_LINE_RASTERIZATION_MODE_RECTANGULAR);
   }

   const uint32_t pa_su_line_cntl = S_028A08_WIDTH(CLAMP(d->vk.rs.line.width * 8, 0, 0xFFFF));

   /* The DX10 diamond test is unnecessary with Vulkan and it decreases line rasterization
    * performance.
    */
   const uint32_t pa_sc_line_cntl =
      S_028BDC_PERPENDICULAR_ENDCAP_ENA(line_rast_mode == VK_LINE_RASTERIZATION_MODE_RECTANGULAR);

   if (pdev->info.gfx_level >= GFX12) {
      radeon_begin(cmd_buffer->cs);
      gfx12_begin_context_regs();
      gfx12_opt_set_context_reg(R_028A08_PA_SU_LINE_CNTL, RADV_TRACKED_PA_SU_LINE_CNTL, pa_su_line_cntl);

      gfx12_opt_set_context_reg(R_028A0C_PA_SC_LINE_STIPPLE, RADV_TRACKED_PA_SC_LINE_STIPPLE,
                                S_028A0C_LINE_PATTERN(d->vk.rs.line.stipple.pattern) |
                                   S_028A0C_REPEAT_COUNT(d->vk.rs.line.stipple.factor - 1));

      gfx12_opt_set_context_reg(R_028BDC_PA_SC_LINE_CNTL, RADV_TRACKED_PA_SC_LINE_CNTL, pa_sc_line_cntl);

      gfx12_opt_set_context_reg(
         R_028810_PA_CL_CLIP_CNTL, RADV_TRACKED_PA_CL_CLIP_CNTL,
         S_028810_DX_RASTERIZATION_KILL(d->vk.rs.rasterizer_discard_enable) |
            S_028810_ZCLIP_NEAR_DISABLE(!depth_clip_enable) | S_028810_ZCLIP_FAR_DISABLE(!depth_clip_enable) |
            S_028810_DX_CLIP_SPACE_DEF(!d->vk.vp.depth_clip_negative_one_to_one) | S_028810_DX_LINEAR_ATTR_CLIP_ENA(1));
      gfx12_opt_set_context_reg(R_028A44_PA_SC_LINE_STIPPLE_RESET, RADV_TRACKED_PA_SC_LINE_STIPPLE_RESET,
                                S_028A44_AUTO_RESET_CNTL(auto_reset_cntl));

      gfx12_opt_set_context_reg(R_02881C_PA_SU_SC_MODE_CNTL, RADV_TRACKED_PA_SU_SC_MODE_CNTL, pa_su_sc_mode_cntl);
      gfx12_end_context_regs();
      radeon_end();
   } else {
      radeon_begin(cmd_buffer->cs);
      radeon_opt_set_context_reg(R_028A08_PA_SU_LINE_CNTL, RADV_TRACKED_PA_SU_LINE_CNTL, pa_su_line_cntl);

      radeon_opt_set_context_reg(R_028A0C_PA_SC_LINE_STIPPLE, RADV_TRACKED_PA_SC_LINE_STIPPLE,
                                 S_028A0C_LINE_PATTERN(d->vk.rs.line.stipple.pattern) |
                                    S_028A0C_REPEAT_COUNT(d->vk.rs.line.stipple.factor - 1) |
                                    S_028A0C_AUTO_RESET_CNTL(auto_reset_cntl));

      radeon_opt_set_context_reg(R_028BDC_PA_SC_LINE_CNTL, RADV_TRACKED_PA_SC_LINE_CNTL, pa_sc_line_cntl);

      radeon_opt_set_context_reg(
         R_028810_PA_CL_CLIP_CNTL, RADV_TRACKED_PA_CL_CLIP_CNTL,
         S_028810_DX_RASTERIZATION_KILL(d->vk.rs.rasterizer_discard_enable) |
            S_028810_ZCLIP_NEAR_DISABLE(!depth_clip_enable) | S_028810_ZCLIP_FAR_DISABLE(!depth_clip_enable) |
            S_028810_DX_CLIP_SPACE_DEF(!d->vk.vp.depth_clip_negative_one_to_one) | S_028810_DX_LINEAR_ATTR_CLIP_ENA(1));
      radeon_opt_set_context_reg(R_028814_PA_SU_SC_MODE_CNTL, RADV_TRACKED_PA_SU_SC_MODE_CNTL, pa_su_sc_mode_cntl);
      radeon_end();
   }
}

static void
radv_emit_cb_render_state(struct radv_cmd_buffer *cmd_buffer)
{
   const struct radv_device *device = radv_cmd_buffer_device(cmd_buffer);
   const struct radv_physical_device *pdev = radv_device_physical(device);
   const struct radv_binning_settings *settings = &pdev->binning_settings;
   const struct radv_rendering_state *render = &cmd_buffer->state.render;
   const struct radv_dynamic_state *d = &cmd_buffer->state.dynamic;
   unsigned cb_blend_control[MAX_RTS], sx_mrt_blend_opt[MAX_RTS];
   const bool mrt0_is_dual_src = radv_is_dual_src_enabled(&cmd_buffer->state.dynamic);
   uint32_t cb_color_control = 0;

   const uint32_t cb_target_mask = d->color_write_enable & d->color_write_mask;

   if (device->pbb_allowed && settings->context_states_per_bin > 1 &&
       cmd_buffer->state.last_cb_target_mask != cb_target_mask) {
      /* Flush DFSM on CB_TARGET_MASK changes. */
      radeon_begin(cmd_buffer->cs);
      radeon_event_write(V_028A90_BREAK_BATCH);
      radeon_end();

      cmd_buffer->state.last_cb_target_mask = cb_target_mask;
   }

   if (d->vk.cb.logic_op_enable) {
      cb_color_control |= S_028808_ROP3(d->vk.cb.logic_op);
   } else {
      cb_color_control |= S_028808_ROP3(V_028808_ROP3_COPY);
   }

   if (cmd_buffer->state.custom_blend_mode) {
      cb_color_control |= S_028808_MODE(cmd_buffer->state.custom_blend_mode);
   } else {
      if (d->color_write_mask) {
         cb_color_control |= S_028808_MODE(V_028808_CB_NORMAL);
      } else {
         cb_color_control |= S_028808_MODE(V_028808_CB_DISABLE);
      }
   }

   for (unsigned i = 0; i < MAX_RTS; i++) {
      cb_blend_control[i] = sx_mrt_blend_opt[i] = 0;

      /* Ignore other blend targets if dual-source blending is enabled to prevent wrong behaviour.
       */
      if (i > 0 && mrt0_is_dual_src)
         continue;

      /* Disable logic op for float/srgb formats because it shouldn't be applied. */
      if (d->vk.cb.logic_op_enable &&
          (vk_format_is_float(render->color_att[i].format) || vk_format_is_srgb(render->color_att[i].format))) {
         cb_blend_control[i] |= S_028780_DISABLE_ROP3(1);
         continue;
      }

      if (!((d->color_blend_enable >> i) & 0x1u)) {
         sx_mrt_blend_opt[i] |= S_028760_COLOR_COMB_FCN(V_028760_OPT_COMB_BLEND_DISABLED) |
                                S_028760_ALPHA_COMB_FCN(V_028760_OPT_COMB_BLEND_DISABLED);
         continue;
      }

      cb_blend_control[i] = d->blend_eq.att[i].cb_blend_control;
      sx_mrt_blend_opt[i] = d->blend_eq.att[i].sx_mrt_blend_opt;
   }

   if (pdev->info.has_rbplus) {
      /* RB+ doesn't work with dual source blending, logic op and CB_RESOLVE. */
      cb_color_control |= S_028808_DISABLE_DUAL_QUAD(mrt0_is_dual_src || d->vk.cb.logic_op_enable ||
                                                     cmd_buffer->state.custom_blend_mode == V_028808_CB_RESOLVE);

      if (mrt0_is_dual_src) {
         for (unsigned i = 0; i < MAX_RTS; i++) {
            sx_mrt_blend_opt[i] =
               S_028760_COLOR_COMB_FCN(V_028760_OPT_COMB_NONE) | S_028760_ALPHA_COMB_FCN(V_028760_OPT_COMB_NONE);
         }
      }

      /* Disable RB+ blend optimizations on GFX11 when alpha-to-coverage is enabled. */
      if (pdev->info.gfx_level >= GFX11 && d->vk.ms.alpha_to_coverage_enable) {
         sx_mrt_blend_opt[0] =
            S_028760_COLOR_COMB_FCN(V_028760_OPT_COMB_NONE) | S_028760_ALPHA_COMB_FCN(V_028760_OPT_COMB_NONE);
      }
   }

   radeon_begin(cmd_buffer->cs);
   radeon_opt_set_context_regn(R_028780_CB_BLEND0_CONTROL, cb_blend_control,
                               cmd_buffer->cs->tracked_regs.cb_blend_control, MAX_RTS);
   if (pdev->info.has_rbplus) {
      radeon_opt_set_context_regn(R_028760_SX_MRT0_BLEND_OPT, sx_mrt_blend_opt,
                                  cmd_buffer->cs->tracked_regs.sx_mrt_blend_opt, MAX_RTS);
   }

   if (pdev->info.gfx_level >= GFX12) {
      radeon_opt_set_context_reg(R_028850_CB_TARGET_MASK, RADV_TRACKED_CB_TARGET_MASK, cb_target_mask);
      radeon_opt_set_context_reg(R_028858_CB_COLOR_CONTROL, RADV_TRACKED_CB_COLOR_CONTROL, cb_color_control);
   } else {
      radeon_opt_set_context_reg(R_028238_CB_TARGET_MASK, RADV_TRACKED_CB_TARGET_MASK, cb_target_mask);
      radeon_opt_set_context_reg(R_028808_CB_COLOR_CONTROL, RADV_TRACKED_CB_COLOR_CONTROL, cb_color_control);
   }
   radeon_end();
}

static void
radv_emit_msaa_state(struct radv_cmd_buffer *cmd_buffer)
{
   const struct radv_device *device = radv_cmd_buffer_device(cmd_buffer);
   const struct radv_physical_device *pdev = radv_device_physical(device);
   const struct radv_instance *instance = radv_physical_device_instance(pdev);
   const struct radv_shader *ps = cmd_buffer->state.shaders[MESA_SHADER_FRAGMENT];
   const unsigned rasterization_samples = cmd_buffer->state.num_rast_samples;
   const struct radv_rendering_state *render = &cmd_buffer->state.render;
   const struct radv_dynamic_state *d = &cmd_buffer->state.dynamic;
   const uint32_t sample_mask = d->vk.ms.sample_mask | ((uint32_t)d->vk.ms.sample_mask << 16);
   const bool enable_1x_user_sample_locs =
      d->vk.ms.sample_locations_enable && d->sample_location.count > 0 && d->sample_location.per_pixel == 1;
   const VkLineRasterizationModeEXT line_rast_mode = cmd_buffer->state.line_rast_mode;
   const bool msaa_enable = rasterization_samples > 1 || enable_1x_user_sample_locs;
   unsigned log_samples = util_logbase2(rasterization_samples);
   unsigned pa_sc_conservative_rast = 0;
   unsigned db_alpha_to_mask = 0;
   unsigned pa_sc_aa_config = 0;
   unsigned max_sample_dist = 0;
   unsigned db_eqaa;

   db_eqaa = S_028804_HIGH_QUALITY_INTERSECTIONS(1) | S_028804_INCOHERENT_EQAA_READS(pdev->info.gfx_level < GFX12) |
             S_028804_STATIC_ANCHOR_ASSOCIATIONS(1);

   if (pdev->info.gfx_level >= GFX9) {
      if (d->vk.rs.conservative_mode != VK_CONSERVATIVE_RASTERIZATION_MODE_DISABLED_EXT) {
         const bool uses_inner_coverage = ps && ps->info.ps.reads_fully_covered;

         pa_sc_conservative_rast |=
            S_028C4C_PREZ_AA_MASK_ENABLE(1) | S_028C4C_POSTZ_AA_MASK_ENABLE(1) | S_028C4C_CENTROID_SAMPLE_OVERRIDE(1);

         /* Inner coverage requires underestimate conservative rasterization. */
         if (d->vk.rs.conservative_mode == VK_CONSERVATIVE_RASTERIZATION_MODE_OVERESTIMATE_EXT &&
             !uses_inner_coverage) {
            pa_sc_conservative_rast |= S_028C4C_OVER_RAST_ENABLE(1) |
                                       S_028C4C_UNDER_RAST_SAMPLE_SELECT(pdev->info.gfx_level < GFX12) |
                                       S_028C4C_PBB_UNCERTAINTY_REGION_ENABLE(1);
         } else {
            pa_sc_conservative_rast |=
               S_028C4C_OVER_RAST_SAMPLE_SELECT(pdev->info.gfx_level < GFX12) | S_028C4C_UNDER_RAST_ENABLE(1);
         }

         /* Adjust MSAA state if conservative rasterization is enabled. */
         db_eqaa |= S_028804_OVERRASTERIZATION_AMOUNT(4);
         pa_sc_aa_config |= S_028BE0_AA_MASK_CENTROID_DTMN(1);

         /* GFX12 programs it in SPI_PS_INPUT_ENA.COVERAGE_TO_SHADER_SELECT */
         pa_sc_aa_config |= S_028BE0_COVERAGE_TO_SHADER_SELECT(pdev->info.gfx_level < GFX12 && uses_inner_coverage);
      } else {
         pa_sc_conservative_rast |= S_028C4C_NULL_SQUAD_AA_MASK_ENABLE(1);
      }
   }

   if (!d->sample_location.count || !d->vk.ms.sample_locations_enable) {
      max_sample_dist = radv_get_default_max_sample_dist(log_samples);
   } else {
      uint32_t num_samples = (uint32_t)d->sample_location.per_pixel;
      VkOffset2D sample_locs[4][8]; /* 8 is the max. sample count supported */

      /* Convert the user sample locations to hardware sample locations. */
      radv_convert_user_sample_locs(&d->sample_location, 0, 0, sample_locs[0]);
      radv_convert_user_sample_locs(&d->sample_location, 1, 0, sample_locs[1]);
      radv_convert_user_sample_locs(&d->sample_location, 0, 1, sample_locs[2]);
      radv_convert_user_sample_locs(&d->sample_location, 1, 1, sample_locs[3]);

      /* Compute the maximum sample distance from the specified locations. */
      for (unsigned i = 0; i < 4; ++i) {
         for (uint32_t j = 0; j < num_samples; j++) {
            VkOffset2D offset = sample_locs[i][j];
            max_sample_dist = MAX2(max_sample_dist, MAX2(abs(offset.x), abs(offset.y)));
         }
      }
   }

   if (msaa_enable) {
      unsigned z_samples = MAX2(render->ds_samples, rasterization_samples);
      unsigned ps_iter_samples = radv_get_ps_iter_samples(cmd_buffer);
      unsigned log_z_samples = util_logbase2(z_samples);
      unsigned log_ps_iter_samples = util_logbase2(ps_iter_samples);
      bool uses_underestimate = d->vk.rs.conservative_mode == VK_CONSERVATIVE_RASTERIZATION_MODE_UNDERESTIMATE_EXT;

      pa_sc_aa_config |=
         S_028BE0_MSAA_NUM_SAMPLES(uses_underestimate ? 0 : log_samples) | S_028BE0_MSAA_EXPOSED_SAMPLES(log_samples);

      if (pdev->info.gfx_level >= GFX12) {
         pa_sc_aa_config |= S_028BE0_PS_ITER_SAMPLES(log_ps_iter_samples);

         db_eqaa |= S_028078_MASK_EXPORT_NUM_SAMPLES(log_samples) | S_028078_ALPHA_TO_MASK_NUM_SAMPLES(log_samples);
      } else {
         pa_sc_aa_config |= S_028BE0_MAX_SAMPLE_DIST(max_sample_dist) |
                            S_028BE0_COVERED_CENTROID_IS_CENTER(pdev->info.gfx_level >= GFX10_3);

         db_eqaa |= S_028804_MAX_ANCHOR_SAMPLES(log_z_samples) | S_028804_PS_ITER_SAMPLES(log_ps_iter_samples) |
                    S_028804_MASK_EXPORT_NUM_SAMPLES(log_samples) | S_028804_ALPHA_TO_MASK_NUM_SAMPLES(log_samples);
      }

      if (line_rast_mode == VK_LINE_RASTERIZATION_MODE_RECTANGULAR_SMOOTH)
         db_eqaa |= S_028804_OVERRASTERIZATION_AMOUNT(log_samples);
   }

   if (instance->debug_flags & RADV_DEBUG_NO_ATOC_DITHERING) {
      db_alpha_to_mask = S_028B70_ALPHA_TO_MASK_OFFSET0(2) | S_028B70_ALPHA_TO_MASK_OFFSET1(2) |
                         S_028B70_ALPHA_TO_MASK_OFFSET2(2) | S_028B70_ALPHA_TO_MASK_OFFSET3(2) |
                         S_028B70_OFFSET_ROUND(0);
   } else {
      db_alpha_to_mask = S_028B70_ALPHA_TO_MASK_OFFSET0(3) | S_028B70_ALPHA_TO_MASK_OFFSET1(1) |
                         S_028B70_ALPHA_TO_MASK_OFFSET2(0) | S_028B70_ALPHA_TO_MASK_OFFSET3(2) |
                         S_028B70_OFFSET_ROUND(1);
   }

   db_alpha_to_mask |= S_028B70_ALPHA_TO_MASK_ENABLE(d->vk.ms.alpha_to_coverage_enable);

   if (pdev->info.gfx_level >= GFX12) {
      radeon_begin(cmd_buffer->cs);
      gfx12_begin_context_regs();
      gfx12_opt_set_context_reg2(R_028C38_PA_SC_AA_MASK_X0Y0_X1Y0, RADV_TRACKED_PA_SC_AA_MASK_X0Y0_X1Y0, sample_mask,
                                 sample_mask);
      gfx12_opt_set_context_reg(R_028BE0_PA_SC_AA_CONFIG, RADV_TRACKED_PA_SC_AA_CONFIG, pa_sc_aa_config);
      gfx12_opt_set_context_reg(
         R_028A48_PA_SC_MODE_CNTL_0, RADV_TRACKED_PA_SC_MODE_CNTL_0,
         S_028A48_ALTERNATE_RBS_PER_TILE(pdev->info.gfx_level >= GFX9) | S_028A48_VPORT_SCISSOR_ENABLE(1) |
            S_028A48_LINE_STIPPLE_ENABLE(d->vk.rs.line.stipple.enable) | S_028A48_MSAA_ENABLE(msaa_enable));
      gfx12_opt_set_context_reg(R_02807C_DB_ALPHA_TO_MASK, RADV_TRACKED_DB_ALPHA_TO_MASK, db_alpha_to_mask);
      gfx12_opt_set_context_reg(R_028C5C_PA_SC_SAMPLE_PROPERTIES, RADV_TRACKED_PA_SC_SAMPLE_PROPERTIES,
                                S_028C5C_MAX_SAMPLE_DIST(max_sample_dist));
      gfx12_opt_set_context_reg(R_028078_DB_EQAA, RADV_TRACKED_DB_EQAA, db_eqaa);
      gfx12_opt_set_context_reg(R_028C54_PA_SC_CONSERVATIVE_RASTERIZATION_CNTL,
                                RADV_TRACKED_PA_SC_CONSERVATIVE_RASTERIZATION_CNTL, pa_sc_conservative_rast);
      gfx12_end_context_regs();
      radeon_end();
   } else {
      radeon_begin(cmd_buffer->cs);
      radeon_opt_set_context_reg2(R_028C38_PA_SC_AA_MASK_X0Y0_X1Y0, RADV_TRACKED_PA_SC_AA_MASK_X0Y0_X1Y0, sample_mask,
                                  sample_mask);
      radeon_opt_set_context_reg(R_028BE0_PA_SC_AA_CONFIG, RADV_TRACKED_PA_SC_AA_CONFIG, pa_sc_aa_config);
      radeon_opt_set_context_reg(
         R_028A48_PA_SC_MODE_CNTL_0, RADV_TRACKED_PA_SC_MODE_CNTL_0,
         S_028A48_ALTERNATE_RBS_PER_TILE(pdev->info.gfx_level >= GFX9) | S_028A48_VPORT_SCISSOR_ENABLE(1) |
            S_028A48_LINE_STIPPLE_ENABLE(d->vk.rs.line.stipple.enable) | S_028A48_MSAA_ENABLE(msaa_enable));
      radeon_opt_set_context_reg(R_028B70_DB_ALPHA_TO_MASK, RADV_TRACKED_DB_ALPHA_TO_MASK, db_alpha_to_mask);
      radeon_opt_set_context_reg(R_028804_DB_EQAA, RADV_TRACKED_DB_EQAA, db_eqaa);

      if (pdev->info.gfx_level >= GFX9)
         radeon_opt_set_context_reg(R_028C4C_PA_SC_CONSERVATIVE_RASTERIZATION_CNTL,
                                    RADV_TRACKED_PA_SC_CONSERVATIVE_RASTERIZATION_CNTL, pa_sc_conservative_rast);
      radeon_end();
   }
}

static void
radv_emit_clip_rects_state(struct radv_cmd_buffer *cmd_buffer)
{
   const struct radv_device *device = radv_cmd_buffer_device(cmd_buffer);
   const struct radv_physical_device *pdev = radv_device_physical(device);
   const struct radv_dynamic_state *d = &cmd_buffer->state.dynamic;
   uint32_t cliprect_rule = 0;

   radeon_begin(cmd_buffer->cs);

   if (!d->vk.dr.enable) {
      cliprect_rule = 0xffff;
   } else {
      for (unsigned i = 0; i < (1u << MAX_DISCARD_RECTANGLES); ++i) {
         /* Interpret i as a bitmask, and then set the bit in
          * the mask if that combination of rectangles in which
          * the pixel is contained should pass the cliprect
          * test.
          */
         unsigned relevant_subset = i & ((1u << d->vk.dr.rectangle_count) - 1);

         if (d->vk.dr.mode == VK_DISCARD_RECTANGLE_MODE_INCLUSIVE_EXT && !relevant_subset)
            continue;

         if (d->vk.dr.mode == VK_DISCARD_RECTANGLE_MODE_EXCLUSIVE_EXT && relevant_subset)
            continue;

         cliprect_rule |= 1u << i;
      }

      radeon_set_context_reg_seq(R_028210_PA_SC_CLIPRECT_0_TL, d->vk.dr.rectangle_count * 2);
      for (unsigned i = 0; i < d->vk.dr.rectangle_count; ++i) {
         VkRect2D rect = d->vk.dr.rectangles[i];
         radeon_emit(S_028210_TL_X(rect.offset.x) | S_028210_TL_Y(rect.offset.y));
         radeon_emit(S_028214_BR_X(rect.offset.x + rect.extent.width) |
                     S_028214_BR_Y(rect.offset.y + rect.extent.height));
      }

      if (pdev->info.gfx_level >= GFX12) {
         radeon_set_context_reg_seq(R_028374_PA_SC_CLIPRECT_0_EXT, d->vk.dr.rectangle_count);
         for (unsigned i = 0; i < d->vk.dr.rectangle_count; ++i) {
            VkRect2D rect = d->vk.dr.rectangles[i];
            radeon_emit(S_028374_TL_X_EXT(rect.offset.x >> 15) | S_028374_TL_Y_EXT(rect.offset.y >> 15) |
                        S_028374_BR_X_EXT((rect.offset.x + rect.extent.width) >> 15) |
                        S_028374_BR_Y_EXT((rect.offset.y + rect.extent.height) >> 15));
         }
      }
   }

   radeon_set_context_reg(R_02820C_PA_SC_CLIPRECT_RULE, cliprect_rule);
   radeon_end();
}

static void
radv_validate_dynamic_states(struct radv_cmd_buffer *cmd_buffer, uint64_t dynamic_states)
{
   const struct radv_device *device = radv_cmd_buffer_device(cmd_buffer);
   const struct radv_physical_device *pdev = radv_device_physical(device);

   if (dynamic_states &
       (RADV_DYNAMIC_DEPTH_CLAMP_ENABLE | RADV_DYNAMIC_DEPTH_CLAMP_RANGE | RADV_DYNAMIC_DEPTH_CLIP_ENABLE)) {
      const bool depth_clip_enable = radv_get_depth_clip_enable(cmd_buffer);

      if (cmd_buffer->state.depth_clip_enable != depth_clip_enable) {
         cmd_buffer->state.depth_clip_enable = depth_clip_enable;
         cmd_buffer->state.dirty |= RADV_CMD_DIRTY_RASTER_STATE;
      }

      const enum radv_depth_clamp_mode depth_clamp_mode = radv_get_depth_clamp_mode(cmd_buffer);

      if (cmd_buffer->state.depth_clamp_mode != depth_clamp_mode) {
         cmd_buffer->state.depth_clamp_mode = depth_clamp_mode;
         cmd_buffer->state.dirty |= RADV_CMD_DIRTY_VIEWPORT_STATE;
      }

      if ((dynamic_states & RADV_DYNAMIC_DEPTH_CLAMP_RANGE) && depth_clamp_mode == RADV_DEPTH_CLAMP_MODE_USER_DEFINED)
         cmd_buffer->state.dirty |= RADV_CMD_DIRTY_VIEWPORT_STATE;
   }

   if (dynamic_states & RADV_DYNAMIC_PROVOKING_VERTEX_MODE)
      cmd_buffer->state.dirty |= RADV_CMD_DIRTY_NGG_STATE;

   if (dynamic_states & (RADV_DYNAMIC_CULL_MODE | RADV_DYNAMIC_FRONT_FACE | RADV_DYNAMIC_RASTERIZER_DISCARD_ENABLE |
                         RADV_DYNAMIC_VIEWPORT | RADV_DYNAMIC_VIEWPORT_WITH_COUNT |
                         RADV_DYNAMIC_CONSERVATIVE_RAST_MODE | RADV_DYNAMIC_SAMPLE_LOCATIONS_ENABLE))
      cmd_buffer->state.dirty |= RADV_CMD_DIRTY_NGGC_SETTINGS;

   if (dynamic_states & (RADV_DYNAMIC_VIEWPORT | RADV_DYNAMIC_VIEWPORT_WITH_COUNT | RADV_DYNAMIC_RASTERIZATION_SAMPLES))
      cmd_buffer->state.dirty |= RADV_CMD_DIRTY_NGGC_VIEWPORT;

   if (dynamic_states & RADV_DYNAMIC_PATCH_CONTROL_POINTS) {
      cmd_buffer->state.dirty |= RADV_CMD_DIRTY_TCS_TES_STATE;
      if (pdev->info.gfx_level < GFX12)
         cmd_buffer->state.dirty |= RADV_CMD_DIRTY_LS_HS_CONFIG;
   }

   if (dynamic_states &
       (RADV_DYNAMIC_DEPTH_TEST_ENABLE | RADV_DYNAMIC_DEPTH_WRITE_ENABLE | RADV_DYNAMIC_DEPTH_COMPARE_OP |
        RADV_DYNAMIC_DEPTH_BOUNDS_TEST_ENABLE | RADV_DYNAMIC_STENCIL_TEST_ENABLE | RADV_DYNAMIC_STENCIL_OP |
        RADV_DYNAMIC_DEPTH_BOUNDS | RADV_DYNAMIC_STENCIL_REFERENCE | RADV_DYNAMIC_STENCIL_WRITE_MASK |
        RADV_DYNAMIC_STENCIL_COMPARE_MASK))
      cmd_buffer->state.dirty |= RADV_CMD_DIRTY_DEPTH_STENCIL_STATE;

   if (dynamic_states &
       (RADV_DYNAMIC_LINE_WIDTH | RADV_DYNAMIC_LINE_STIPPLE | RADV_DYNAMIC_CULL_MODE | RADV_DYNAMIC_FRONT_FACE |
        RADV_DYNAMIC_DEPTH_BIAS_ENABLE | RADV_DYNAMIC_POLYGON_MODE | RADV_DYNAMIC_PROVOKING_VERTEX_MODE |
        RADV_DYNAMIC_RASTERIZER_DISCARD_ENABLE | RADV_DYNAMIC_DEPTH_CLIP_NEGATIVE_ONE_TO_ONE))
      cmd_buffer->state.dirty |= RADV_CMD_DIRTY_RASTER_STATE;

   if (dynamic_states &
       (RADV_DYNAMIC_LINE_STIPPLE_ENABLE | RADV_DYNAMIC_CONSERVATIVE_RAST_MODE | RADV_DYNAMIC_SAMPLE_LOCATIONS |
        RADV_DYNAMIC_SAMPLE_LOCATIONS_ENABLE | RADV_DYNAMIC_ALPHA_TO_COVERAGE_ENABLE | RADV_DYNAMIC_SAMPLE_MASK))
      cmd_buffer->state.dirty |= RADV_CMD_DIRTY_MSAA_STATE;

   if (dynamic_states &
       (RADV_DYNAMIC_DISCARD_RECTANGLE | RADV_DYNAMIC_DISCARD_RECTANGLE_ENABLE | RADV_DYNAMIC_DISCARD_RECTANGLE_MODE))
      cmd_buffer->state.dirty |= RADV_CMD_DIRTY_CLIP_RECTS_STATE;

   if (dynamic_states & (RADV_DYNAMIC_COLOR_WRITE_ENABLE | RADV_DYNAMIC_COLOR_WRITE_MASK | RADV_DYNAMIC_LOGIC_OP |
                         RADV_DYNAMIC_LOGIC_OP_ENABLE | RADV_DYNAMIC_COLOR_BLEND_ENABLE |
                         RADV_DYNAMIC_COLOR_BLEND_EQUATION | RADV_DYNAMIC_ALPHA_TO_COVERAGE_ENABLE))
      cmd_buffer->state.dirty |= RADV_CMD_DIRTY_CB_RENDER_STATE;

   if (dynamic_states &
       (RADV_DYNAMIC_VIEWPORT | RADV_DYNAMIC_VIEWPORT_WITH_COUNT | RADV_DYNAMIC_DEPTH_CLIP_NEGATIVE_ONE_TO_ONE))
      cmd_buffer->state.dirty |= RADV_CMD_DIRTY_VIEWPORT_STATE;

   if (dynamic_states & RADV_DYNAMIC_COLOR_WRITE_MASK)
      cmd_buffer->state.dirty |= RADV_CMD_DIRTY_BINNING_STATE;

   if (dynamic_states &
       (RADV_DYNAMIC_COLOR_WRITE_MASK | RADV_DYNAMIC_COLOR_BLEND_ENABLE | RADV_DYNAMIC_ATTACHMENT_FEEDBACK_LOOP_ENABLE |
        RADV_DYNAMIC_ALPHA_TO_COVERAGE_ENABLE | RADV_DYNAMIC_ALPHA_TO_ONE_ENABLE))
      cmd_buffer->state.dirty |= RADV_CMD_DIRTY_DB_SHADER_CONTROL;

   if (dynamic_states & RADV_DYNAMIC_FRAGMENT_SHADING_RATE)
      cmd_buffer->state.dirty |= RADV_CMD_DIRTY_FSR_STATE;

   if (dynamic_states & RADV_DYNAMIC_SAMPLE_LOCATIONS_ENABLE)
      cmd_buffer->state.dirty |= RADV_CMD_DIRTY_RAST_SAMPLES_STATE;

   if (dynamic_states & RADV_DYNAMIC_DEPTH_BIAS)
      cmd_buffer->state.dirty |= RADV_CMD_DIRTY_DEPTH_BIAS_STATE;

   if (dynamic_states & RADV_DYNAMIC_VERTEX_INPUT)
      cmd_buffer->state.dirty |= RADV_CMD_DIRTY_VS_PROLOG_STATE;

   if (dynamic_states & RADV_DYNAMIC_BLEND_CONSTANTS)
      cmd_buffer->state.dirty |= RADV_CMD_DIRTY_BLEND_CONSTANTS_STATE;

   if (dynamic_states & (RADV_DYNAMIC_SAMPLE_LOCATIONS | RADV_DYNAMIC_SAMPLE_LOCATIONS_ENABLE))
      cmd_buffer->state.dirty |= RADV_CMD_DIRTY_SAMPLE_LOCATIONS_STATE;

   if (dynamic_states & (RADV_DYNAMIC_SCISSOR | RADV_DYNAMIC_SCISSOR_WITH_COUNT | RADV_DYNAMIC_VIEWPORT |
                         RADV_DYNAMIC_VIEWPORT_WITH_COUNT) &&
       !pdev->info.has_gfx9_scissor_bug)
      cmd_buffer->state.dirty |= RADV_CMD_DIRTY_SCISSOR_STATE;

   if (dynamic_states & RADV_DYNAMIC_TESS_DOMAIN_ORIGIN)
      cmd_buffer->state.dirty |= RADV_CMD_DIRTY_TESS_DOMAIN_ORIGIN_STATE;

   if ((dynamic_states & RADV_DYNAMIC_PRIMITIVE_TOPOLOGY) ||
       (pdev->info.gfx_level >= GFX12 && dynamic_states & RADV_DYNAMIC_PATCH_CONTROL_POINTS))
      cmd_buffer->state.dirty |= RADV_CMD_DIRTY_VGT_PRIM_STATE;

   if (dynamic_states &
       (RADV_DYNAMIC_COLOR_WRITE_MASK | RADV_DYNAMIC_COLOR_BLEND_ENABLE | RADV_DYNAMIC_ALPHA_TO_COVERAGE_ENABLE |
        RADV_DYNAMIC_COLOR_BLEND_EQUATION | RADV_DYNAMIC_ALPHA_TO_ONE_ENABLE | RADV_DYNAMIC_COLOR_ATTACHMENT_MAP))
      cmd_buffer->state.dirty |= RADV_CMD_DIRTY_PS_EPILOG_SHADER;
}

static void
radv_emit_all_graphics_states(struct radv_cmd_buffer *cmd_buffer, const struct radv_draw_info *info)
{
   struct radv_device *device = radv_cmd_buffer_device(cmd_buffer);
   const struct radv_physical_device *pdev = radv_device_physical(device);

   const uint64_t dynamic_states = cmd_buffer->state.dirty_dynamic & radv_get_needed_dynamic_states(cmd_buffer);
   if (((cmd_buffer->state.dirty & (RADV_CMD_DIRTY_PIPELINE | RADV_CMD_DIRTY_GRAPHICS_SHADERS)) ||
        (dynamic_states & RADV_DYNAMIC_PATCH_CONTROL_POINTS))) {
      if (cmd_buffer->state.active_stages & VK_SHADER_STAGE_TESSELLATION_CONTROL_BIT) {
         const struct radv_shader *vs = radv_get_shader(cmd_buffer->state.shaders, MESA_SHADER_VERTEX);
         const struct radv_shader *tcs = cmd_buffer->state.shaders[MESA_SHADER_TESS_CTRL];
         const struct radv_dynamic_state *d = &cmd_buffer->state.dynamic;
         uint32_t tess_num_patches, tess_lds_size;

         radv_get_tess_wg_info(pdev, &tcs->info.tcs.io_info, tcs->info.tcs.tcs_vertices_out,
                               d->vk.ts.patch_control_points,
                               /* TODO: This should be only inputs in LDS (not VGPR inputs) to reduce LDS usage */
                               vs->info.vs.num_linked_outputs, &tess_num_patches, &tess_lds_size);

         if (cmd_buffer->state.tess_lds_size != tess_lds_size) {
            cmd_buffer->state.tess_lds_size = tess_lds_size;
            cmd_buffer->state.dirty |= RADV_CMD_DIRTY_TCS_TES_STATE;
         }

         if (cmd_buffer->state.tess_num_patches != tess_num_patches) {
            cmd_buffer->state.tess_num_patches = tess_num_patches;
            cmd_buffer->state.dirty |= RADV_CMD_DIRTY_LS_HS_CONFIG | RADV_CMD_DIRTY_TCS_TES_STATE;
         }
      }
   }

   if ((cmd_buffer->state.dirty & (RADV_CMD_DIRTY_PIPELINE | RADV_CMD_DIRTY_GRAPHICS_SHADERS)) ||
       (dynamic_states & (RADV_DYNAMIC_PRIMITIVE_TOPOLOGY | RADV_DYNAMIC_POLYGON_MODE |
                          RADV_DYNAMIC_LINE_RASTERIZATION_MODE | RADV_DYNAMIC_RASTERIZATION_SAMPLES))) {
      const uint32_t vgt_outprim_type = radv_get_vgt_outprim_type(cmd_buffer);

      if (cmd_buffer->state.vgt_outprim_type != vgt_outprim_type) {
         if (radv_vgt_outprim_is_point_or_line(cmd_buffer->state.vgt_outprim_type) !=
             radv_vgt_outprim_is_point_or_line(vgt_outprim_type))
            cmd_buffer->state.dirty |= RADV_CMD_DIRTY_GUARDBAND;

         cmd_buffer->state.vgt_outprim_type = vgt_outprim_type;
         cmd_buffer->state.dirty |= RADV_CMD_DIRTY_PS_STATE | RADV_CMD_DIRTY_NGG_STATE | RADV_CMD_DIRTY_NGGC_SETTINGS |
                                    RADV_CMD_DIRTY_VGT_PRIM_STATE;
      }

      const VkLineRasterizationModeEXT line_rast_mode = radv_get_line_mode(cmd_buffer);

      if (cmd_buffer->state.line_rast_mode != line_rast_mode) {
         cmd_buffer->state.line_rast_mode = line_rast_mode;
         cmd_buffer->state.dirty |= RADV_CMD_DIRTY_PS_STATE | RADV_CMD_DIRTY_RASTER_STATE | RADV_CMD_DIRTY_MSAA_STATE;
         if (pdev->info.gfx_level == GFX6)
            cmd_buffer->state.dirty |= RADV_CMD_DIRTY_DB_SHADER_CONTROL;
      }

      const uint32_t num_rast_samples = radv_get_rasterization_samples(cmd_buffer);

      if (cmd_buffer->state.num_rast_samples != num_rast_samples) {
         cmd_buffer->state.num_rast_samples = num_rast_samples;
         cmd_buffer->state.dirty |= RADV_CMD_DIRTY_BINNING_STATE | RADV_CMD_DIRTY_RAST_SAMPLES_STATE |
                                    RADV_CMD_DIRTY_PS_STATE | RADV_CMD_DIRTY_DB_SHADER_CONTROL |
                                    RADV_CMD_DIRTY_MSAA_STATE | RADV_CMD_DIRTY_NGGC_SETTINGS;
         if (pdev->info.gfx_level < GFX12)
            cmd_buffer->state.dirty |= RADV_CMD_DIRTY_OCCLUSION_QUERY;
      }
   }

   if (dynamic_states)
      radv_validate_dynamic_states(cmd_buffer, dynamic_states);

   if (cmd_buffer->state.dirty & RADV_CMD_DIRTY_PS_EPILOG_SHADER) {
      radv_bind_ps_epilog(cmd_buffer);
      cmd_buffer->state.dirty &= ~RADV_CMD_DIRTY_PS_EPILOG_SHADER;
   }

   /* Determine whether GFX9 late scissor workaround should be applied based on:
    * 1. radv_need_late_scissor_emission
    * 2. any dirty dynamic flags that may cause context rolls
    */
   const bool late_scissor_emission =
      pdev->info.has_gfx9_scissor_bug ? radv_need_late_scissor_emission(cmd_buffer, info) : false;

   cmd_buffer->state.dirty_dynamic &= ~dynamic_states;

   const bool gfx12_emit_hiz_wa_full =
      pdev->gfx12_hiz_wa == RADV_GFX12_HIZ_WA_FULL &&
      cmd_buffer->state.dirty & (RADV_CMD_DIRTY_FRAMEBUFFER | RADV_CMD_DIRTY_DEPTH_STENCIL_STATE);

   if (cmd_buffer->state.dirty & RADV_CMD_DIRTY_RBPLUS) {
      radv_emit_rbplus_state(cmd_buffer);
      cmd_buffer->state.dirty &= ~RADV_CMD_DIRTY_RBPLUS;
   }

   if (cmd_buffer->state.dirty & RADV_CMD_DIRTY_OCCLUSION_QUERY) {
      radv_emit_occlusion_query_state(cmd_buffer);
      cmd_buffer->state.dirty &= ~RADV_CMD_DIRTY_OCCLUSION_QUERY;
   }

   if (cmd_buffer->state.dirty & RADV_CMD_DIRTY_BINNING_STATE) {
      radv_emit_binning_state(cmd_buffer);
      cmd_buffer->state.dirty &= ~RADV_CMD_DIRTY_BINNING_STATE;
   }

   if (cmd_buffer->state.dirty & RADV_CMD_DIRTY_PIPELINE) {
      radv_emit_graphics_pipeline(cmd_buffer);
      cmd_buffer->state.dirty &= ~RADV_CMD_DIRTY_PIPELINE;
   } else if (cmd_buffer->state.dirty & RADV_CMD_DIRTY_GRAPHICS_SHADERS) {
      radv_emit_graphics_shaders(cmd_buffer);
      cmd_buffer->state.dirty &= ~RADV_CMD_DIRTY_GRAPHICS_SHADERS;
   }

   if (cmd_buffer->state.dirty & RADV_CMD_DIRTY_FRAGMENT_OUTPUT) {
      radv_emit_fragment_output_state(cmd_buffer);
      cmd_buffer->state.dirty &= ~RADV_CMD_DIRTY_FRAGMENT_OUTPUT;
   }

   if (cmd_buffer->state.dirty & RADV_CMD_DIRTY_FRAMEBUFFER) {
      radv_emit_framebuffer_state(cmd_buffer);
      cmd_buffer->state.dirty &= ~RADV_CMD_DIRTY_FRAMEBUFFER;
   }

   if (cmd_buffer->state.dirty & RADV_CMD_DIRTY_GUARDBAND) {
      radv_emit_guardband_state(cmd_buffer);
      cmd_buffer->state.dirty &= ~RADV_CMD_DIRTY_GUARDBAND;
   }

   if (cmd_buffer->state.dirty & RADV_CMD_DIRTY_DB_SHADER_CONTROL) {
      radv_emit_db_shader_control(cmd_buffer);
      cmd_buffer->state.dirty &= ~RADV_CMD_DIRTY_DB_SHADER_CONTROL;
   }

   if (info->indexed && info->indirect_va && cmd_buffer->state.dirty & RADV_CMD_DIRTY_INDEX_BUFFER) {
      radv_emit_index_buffer(cmd_buffer);
      cmd_buffer->state.dirty &= ~RADV_CMD_DIRTY_INDEX_BUFFER;
   }

   if (cmd_buffer->state.dirty & RADV_CMD_DIRTY_STREAMOUT_ENABLE) {
      radv_emit_streamout_enable_state(cmd_buffer);
      cmd_buffer->state.dirty &= ~RADV_CMD_DIRTY_STREAMOUT_ENABLE;
   }

   if (cmd_buffer->state.dirty & RADV_CMD_DIRTY_VS_PROLOG_STATE) {
      radv_emit_vs_prolog_state(cmd_buffer);
      cmd_buffer->state.dirty &= ~RADV_CMD_DIRTY_VS_PROLOG_STATE;
   }

   if (cmd_buffer->state.dirty & RADV_CMD_DIRTY_CLIP_RECTS_STATE) {
      radv_emit_clip_rects_state(cmd_buffer);
      cmd_buffer->state.dirty &= ~RADV_CMD_DIRTY_CLIP_RECTS_STATE;
   }

   if (cmd_buffer->state.dirty & RADV_CMD_DIRTY_VIEWPORT_STATE) {
      radv_emit_viewport_state(cmd_buffer);
      cmd_buffer->state.dirty &= ~RADV_CMD_DIRTY_VIEWPORT_STATE;
   }

   if (cmd_buffer->state.dirty & RADV_CMD_DIRTY_SCISSOR_STATE) {
      radv_emit_scissor_state(cmd_buffer);
      cmd_buffer->state.dirty &= ~RADV_CMD_DIRTY_SCISSOR_STATE;
   }

   if (cmd_buffer->state.dirty & RADV_CMD_DIRTY_VGT_PRIM_STATE) {
      radv_emit_vgt_prim_state(cmd_buffer);
      cmd_buffer->state.dirty &= ~RADV_CMD_DIRTY_VGT_PRIM_STATE;
   }

   if (cmd_buffer->state.dirty & RADV_CMD_DIRTY_LS_HS_CONFIG) {
      radv_emit_ls_hs_config(cmd_buffer);
      cmd_buffer->state.dirty &= ~RADV_CMD_DIRTY_LS_HS_CONFIG;
   }

   if (cmd_buffer->state.dirty & RADV_CMD_DIRTY_TESS_DOMAIN_ORIGIN_STATE) {
      radv_emit_tess_domain_origin_state(cmd_buffer);
      cmd_buffer->state.dirty &= ~RADV_CMD_DIRTY_TESS_DOMAIN_ORIGIN_STATE;
   }

   if (cmd_buffer->state.dirty & RADV_CMD_DIRTY_RASTER_STATE) {
      radv_emit_raster_state(cmd_buffer);
      cmd_buffer->state.dirty &= ~RADV_CMD_DIRTY_RASTER_STATE;
   }

   if (cmd_buffer->state.dirty & RADV_CMD_DIRTY_DEPTH_BIAS_STATE) {
      radv_emit_depth_bias_state(cmd_buffer);
      cmd_buffer->state.dirty &= ~RADV_CMD_DIRTY_DEPTH_BIAS_STATE;
   }

   if (cmd_buffer->state.dirty & RADV_CMD_DIRTY_DEPTH_STENCIL_STATE) {
      radv_emit_depth_stencil_state(cmd_buffer);
      cmd_buffer->state.dirty &= ~RADV_CMD_DIRTY_DEPTH_STENCIL_STATE;
   }

   if (cmd_buffer->state.dirty & RADV_CMD_DIRTY_BLEND_CONSTANTS_STATE) {
      radv_emit_blend_constants_state(cmd_buffer);
      cmd_buffer->state.dirty &= ~RADV_CMD_DIRTY_BLEND_CONSTANTS_STATE;
   }

   if (cmd_buffer->state.dirty & RADV_CMD_DIRTY_CB_RENDER_STATE) {
      radv_emit_cb_render_state(cmd_buffer);
      cmd_buffer->state.dirty &= ~RADV_CMD_DIRTY_CB_RENDER_STATE;
   }

   if (cmd_buffer->state.dirty & RADV_CMD_DIRTY_SAMPLE_LOCATIONS_STATE) {
      radv_emit_sample_locations_state(cmd_buffer);
      cmd_buffer->state.dirty &= ~RADV_CMD_DIRTY_SAMPLE_LOCATIONS_STATE;
   }

   if (cmd_buffer->state.dirty & RADV_CMD_DIRTY_MSAA_STATE) {
      radv_emit_msaa_state(cmd_buffer);
      cmd_buffer->state.dirty &= ~RADV_CMD_DIRTY_MSAA_STATE;
   }

   if (cmd_buffer->state.dirty & RADV_CMD_DIRTY_FSR_STATE) {
      radv_emit_fsr_state(cmd_buffer);
      cmd_buffer->state.dirty &= ~RADV_CMD_DIRTY_FSR_STATE;
   }

   if (cmd_buffer->state.dirty & RADV_CMD_DIRTY_RAST_SAMPLES_STATE) {
      radv_emit_rast_samples_state(cmd_buffer);
      cmd_buffer->state.dirty &= ~RADV_CMD_DIRTY_RAST_SAMPLES_STATE;
   }

   if (gfx12_emit_hiz_wa_full)
      radv_gfx12_emit_hiz_wa_full(cmd_buffer);

   radv_emit_shaders_state(cmd_buffer);

   radv_emit_draw_registers(cmd_buffer, info);

   if (late_scissor_emission) {
      radv_emit_scissor_state(cmd_buffer);
      cmd_buffer->cs->context_roll_without_scissor_emitted = false;
   }
}

static void
radv_bind_graphics_shaders(struct radv_cmd_buffer *cmd_buffer)
{
   struct radv_device *device = radv_cmd_buffer_device(cmd_buffer);
   const struct radv_physical_device *pdev = radv_device_physical(device);
   uint32_t push_constant_size = 0, dynamic_offset_count = 0;
   bool need_indirect_descriptors = false;
   bool need_push_constants_upload = false;

   for (unsigned s = 0; s <= MESA_SHADER_MESH; s++) {
      const struct radv_shader_object *shader_obj = cmd_buffer->state.shader_objs[s];
      struct radv_shader *shader = NULL;

      if (s == MESA_SHADER_COMPUTE)
         continue;

      if (!shader_obj) {
         radv_bind_shader(cmd_buffer, NULL, s);
         continue;
      }

      /* Select shader variants. */
      if (s == MESA_SHADER_VERTEX && (cmd_buffer->state.shader_objs[MESA_SHADER_TESS_CTRL] ||
                                      cmd_buffer->state.shader_objs[MESA_SHADER_GEOMETRY])) {
         if (cmd_buffer->state.shader_objs[MESA_SHADER_TESS_CTRL]) {
            shader = shader_obj->as_ls.shader;
         } else {
            shader = shader_obj->as_es.shader;
         }
      } else if (s == MESA_SHADER_TESS_EVAL && cmd_buffer->state.shader_objs[MESA_SHADER_GEOMETRY]) {
         shader = shader_obj->as_es.shader;
      } else {
         shader = shader_obj->shader;
      }

      radv_bind_shader(cmd_buffer, shader, s);
      if (!shader)
         continue;

      /* Compute push constants/indirect descriptors state. */
      need_indirect_descriptors |= radv_shader_need_indirect_descriptors(shader);
      need_push_constants_upload |= radv_shader_need_push_constants_upload(shader);
      push_constant_size += shader_obj->push_constant_size;
      dynamic_offset_count += shader_obj->dynamic_offset_count;
   }

   struct radv_shader *gs_copy_shader = cmd_buffer->state.shader_objs[MESA_SHADER_GEOMETRY]
                                           ? cmd_buffer->state.shader_objs[MESA_SHADER_GEOMETRY]->gs.copy_shader
                                           : NULL;

   radv_bind_gs_copy_shader(cmd_buffer, gs_copy_shader);

   /* Determine NGG GS info. */
   if (cmd_buffer->state.shaders[MESA_SHADER_GEOMETRY] &&
       cmd_buffer->state.shaders[MESA_SHADER_GEOMETRY]->info.is_ngg &&
       cmd_buffer->state.shaders[MESA_SHADER_GEOMETRY]->info.merged_shader_compiled_separately) {
      struct radv_shader *es = cmd_buffer->state.shaders[MESA_SHADER_TESS_EVAL]
                                  ? cmd_buffer->state.shaders[MESA_SHADER_TESS_EVAL]
                                  : cmd_buffer->state.shaders[MESA_SHADER_VERTEX];
      struct radv_shader *gs = cmd_buffer->state.shaders[MESA_SHADER_GEOMETRY];

      gfx10_ngg_set_esgs_ring_itemsize(device, &es->info, &gs->info, &gs->info.ngg_info);
      gfx10_get_ngg_info(device, &es->info, &gs->info, &gs->info.ngg_info);
      radv_precompute_registers_hw_ngg(device, &gs->config, &gs->info);
   }

   const struct radv_shader *ps = cmd_buffer->state.shaders[MESA_SHADER_FRAGMENT];
   if (ps && !ps->info.ps.has_epilog) {
      radv_bind_fragment_output_state(cmd_buffer, ps, NULL, 0);
   }

   /* Update push constants/indirect descriptors state. */
   struct radv_descriptor_state *descriptors_state =
      radv_get_descriptors_state(cmd_buffer, VK_PIPELINE_BIND_POINT_GRAPHICS);
   struct radv_push_constant_state *pc_state = &cmd_buffer->push_constant_state[VK_PIPELINE_BIND_POINT_GRAPHICS];

   descriptors_state->need_indirect_descriptors = need_indirect_descriptors;
   descriptors_state->dynamic_offset_count = dynamic_offset_count;
   pc_state->need_upload = need_push_constants_upload;
   pc_state->size = push_constant_size;

   if (pdev->info.gfx_level <= GFX9) {
      cmd_buffer->state.ia_multi_vgt_param = radv_compute_ia_multi_vgt_param(device, cmd_buffer->state.shaders);
   }
}

/* MUST inline this function to avoid massive perf loss in drawoverhead */
ALWAYS_INLINE static bool
radv_before_draw(struct radv_cmd_buffer *cmd_buffer, const struct radv_draw_info *info, uint32_t drawCount, bool dgc)
{
   struct radv_device *device = radv_cmd_buffer_device(cmd_buffer);
   const struct radv_physical_device *pdev = radv_device_physical(device);
   const bool has_prefetch = pdev->info.gfx_level >= GFX7;
   struct radv_cmd_stream *cs = cmd_buffer->cs;

   ASSERTED const unsigned cdw_max = radeon_check_space(device->ws, cs->b, 4096 + 128 * (drawCount - 1));

   if (likely(!info->indirect_va)) {
      /* GFX6-GFX7 treat instance_count==0 as instance_count==1. There is
       * no workaround for indirect draws, but we can at least skip
       * direct draws.
       */
      if (unlikely(!info->instance_count))
         return false;

      /* Handle count == 0. */
      if (unlikely(!info->count && !info->strmout_va))
         return false;
   }

   if (!info->indexed && pdev->info.gfx_level >= GFX7) {
      /* On GFX7 and later, non-indexed draws overwrite VGT_INDEX_TYPE,
       * so the state must be re-emitted before the next indexed
       * draw.
       */
      cmd_buffer->state.last_index_type = -1;
   }

   if (cmd_buffer->state.dirty & RADV_CMD_DIRTY_FBFETCH_OUTPUT) {
      radv_handle_fbfetch_output(cmd_buffer);
      cmd_buffer->state.dirty &= ~RADV_CMD_DIRTY_FBFETCH_OUTPUT;
   }

   if (cmd_buffer->state.dirty & RADV_CMD_DIRTY_GRAPHICS_SHADERS) {
      radv_bind_graphics_shaders(cmd_buffer);
   }

   /* This is the optimal packet order:
    * Set all states first, so that all SET packets are processed in parallel with previous draw
    * calls. Then flush caches and wait if needed. Then draw and prefetch at the end. It's better
    * to draw before prefetches because we want to start fetching indices before shaders. The idea
    * is to minimize the time when the CUs are idle.
    */
   radv_emit_all_graphics_states(cmd_buffer, info);
   radv_upload_graphics_shader_descriptors(cmd_buffer);

   if (pdev->info.gfx_level >= GFX12) {
      radv_gfx12_emit_buffered_regs(device, cs);
   }

   if (cmd_buffer->state.flush_bits)
      radv_emit_cache_flush(cmd_buffer);

   /* <-- CUs are idle here if shaders are synchronized. */

   if (has_prefetch) {
      /* Only prefetch the vertex shader and VBO descriptors in order to start the draw as soon as
       * possible.
       */
      radv_emit_graphics_prefetch(cmd_buffer, true);
   }

   if (device->sqtt.bo && !dgc)
      radv_describe_draw(cmd_buffer, info);
   if (likely(!info->indirect_va)) {
      struct radv_cmd_state *state = &cmd_buffer->state;
      assert(state->vtx_base_sgpr);
      if (state->last_num_instances != info->instance_count) {
         radeon_begin(cs);
         radeon_emit(PKT3(PKT3_NUM_INSTANCES, 0, false));
         radeon_emit(info->instance_count);
         radeon_end();

         state->last_num_instances = info->instance_count;
      }
   }
   assert(cs->b->cdw <= cdw_max);

   return true;
}

ALWAYS_INLINE static bool
radv_before_taskmesh_draw(struct radv_cmd_buffer *cmd_buffer, const struct radv_draw_info *info, uint32_t drawCount,
                          bool dgc)
{
   struct radv_device *device = radv_cmd_buffer_device(cmd_buffer);
   const struct radv_physical_device *pdev = radv_device_physical(device);
   struct radv_cmd_stream *cs = cmd_buffer->cs;

   /* For direct draws, this makes sure we don't draw anything.
    * For indirect draws, this is necessary to prevent a GPU hang (on MEC version < 100).
    */
   if (unlikely(!info->count))
      return false;

   if (cmd_buffer->state.dirty & RADV_CMD_DIRTY_GRAPHICS_SHADERS) {
      radv_bind_graphics_shaders(cmd_buffer);
   }

   struct radv_cmd_stream *ace_cs = cmd_buffer->gang.cs;
   struct radv_shader *task_shader = cmd_buffer->state.shaders[MESA_SHADER_TASK];

   assert(!task_shader || ace_cs);

   const VkShaderStageFlags stages =
      VK_SHADER_STAGE_MESH_BIT_EXT | VK_SHADER_STAGE_FRAGMENT_BIT | (task_shader ? VK_SHADER_STAGE_TASK_BIT_EXT : 0);
   const bool need_task_semaphore = task_shader && radv_flush_gang_leader_semaphore(cmd_buffer);

   ASSERTED const unsigned cdw_max = radeon_check_space(device->ws, cs->b, 4096 + 128 * (drawCount - 1));
   ASSERTED const unsigned ace_cdw_max =
      !ace_cs ? 0 : radeon_check_space(device->ws, ace_cs->b, 4096 + 128 * (drawCount - 1));

   radv_emit_all_graphics_states(cmd_buffer, info);

   struct radv_descriptor_state *descriptors_state =
      radv_get_descriptors_state(cmd_buffer, VK_PIPELINE_BIND_POINT_GRAPHICS);

   if (descriptors_state->dirty) {
      radv_flush_descriptors(cmd_buffer, stages, VK_PIPELINE_BIND_POINT_GRAPHICS);
      descriptors_state->dirty = 0;
   }

   if (descriptors_state->dirty_dynamic && descriptors_state->dynamic_offset_count) {
      radv_flush_dynamic_descriptors(cmd_buffer, stages, VK_PIPELINE_BIND_POINT_GRAPHICS);
      descriptors_state->dirty_dynamic = false;
   }

   const VkShaderStageFlags pc_stages = radv_must_flush_constants(cmd_buffer, stages, VK_PIPELINE_BIND_POINT_GRAPHICS);
   if (pc_stages)
      radv_flush_constants(cmd_buffer, pc_stages, VK_PIPELINE_BIND_POINT_GRAPHICS);

   if (pdev->info.gfx_level >= GFX12) {
      radv_gfx12_emit_buffered_regs(device, cs);

      if (task_shader)
         radv_gfx12_emit_buffered_regs(device, cmd_buffer->gang.cs);
   }

   if (cmd_buffer->state.flush_bits)
      radv_emit_cache_flush(cmd_buffer);

   if (task_shader) {
      radv_gang_cache_flush(cmd_buffer);

      if (need_task_semaphore) {
         radv_wait_gang_leader(cmd_buffer);
      }
   }

   if (device->sqtt.bo && !dgc)
      radv_describe_draw(cmd_buffer, info);
   if (likely(!info->indirect_va)) {
      struct radv_cmd_state *state = &cmd_buffer->state;
      if (unlikely(state->last_num_instances != 1)) {
         radeon_begin(cs);
         radeon_emit(PKT3(PKT3_NUM_INSTANCES, 0, false));
         radeon_emit(1);
         radeon_end();

         state->last_num_instances = 1;
      }
   }

   assert(cs->b->cdw <= cdw_max);
   assert(!ace_cs || ace_cs->b->cdw <= ace_cdw_max);

   cmd_buffer->state.last_index_type = -1;

   return true;
}

ALWAYS_INLINE static void
radv_after_draw(struct radv_cmd_buffer *cmd_buffer, bool dgc)
{
   struct radv_device *device = radv_cmd_buffer_device(cmd_buffer);
   const struct radv_physical_device *pdev = radv_device_physical(device);
   const struct radeon_info *gpu_info = &pdev->info;
   const bool has_prefetch = pdev->info.gfx_level >= GFX7;

   /* Start prefetches after the draw has been started. Both will run in parallel, but starting the
    * draw first is more important.
    */
   if (has_prefetch)
      radv_emit_graphics_prefetch(cmd_buffer, false);

   /* Workaround for a VGT hang when streamout is enabled.
    * It must be done after drawing.
    */
   if (radv_is_streamout_enabled(cmd_buffer) &&
       (gpu_info->family == CHIP_HAWAII || gpu_info->family == CHIP_TONGA || gpu_info->family == CHIP_FIJI)) {
      cmd_buffer->state.flush_bits |= RADV_CMD_FLAG_VGT_STREAMOUT_SYNC;
   }

   radv_cmd_buffer_after_draw(cmd_buffer, RADV_CMD_FLAG_PS_PARTIAL_FLUSH, dgc);
}

VKAPI_ATTR void VKAPI_CALL
radv_CmdDraw(VkCommandBuffer commandBuffer, uint32_t vertexCount, uint32_t instanceCount, uint32_t firstVertex,
             uint32_t firstInstance)
{
   VK_FROM_HANDLE(radv_cmd_buffer, cmd_buffer, commandBuffer);
   struct radv_draw_info info;

   info.count = vertexCount;
   info.instance_count = instanceCount;
   info.first_instance = firstInstance;
   info.strmout_va = 0;
   info.indirect_va = 0;
   info.indexed = false;

   if (!radv_before_draw(cmd_buffer, &info, 1, false))
      return;
   const VkMultiDrawInfoEXT minfo = {firstVertex, vertexCount};
   radv_emit_direct_draw_packets(cmd_buffer, &info, 1, &minfo, 0, 0);
   radv_after_draw(cmd_buffer, false);
}

VKAPI_ATTR void VKAPI_CALL
radv_CmdDrawMultiEXT(VkCommandBuffer commandBuffer, uint32_t drawCount, const VkMultiDrawInfoEXT *pVertexInfo,
                     uint32_t instanceCount, uint32_t firstInstance, uint32_t stride)
{
   VK_FROM_HANDLE(radv_cmd_buffer, cmd_buffer, commandBuffer);
   struct radv_draw_info info;

   if (!drawCount)
      return;

   info.count = pVertexInfo->vertexCount;
   info.instance_count = instanceCount;
   info.first_instance = firstInstance;
   info.strmout_va = 0;
   info.indirect_va = 0;
   info.indexed = false;

   if (!radv_before_draw(cmd_buffer, &info, drawCount, false))
      return;
   radv_emit_direct_draw_packets(cmd_buffer, &info, drawCount, pVertexInfo, 0, stride);
   radv_after_draw(cmd_buffer, false);
}

VKAPI_ATTR void VKAPI_CALL
radv_CmdDrawIndexed(VkCommandBuffer commandBuffer, uint32_t indexCount, uint32_t instanceCount, uint32_t firstIndex,
                    int32_t vertexOffset, uint32_t firstInstance)
{
   VK_FROM_HANDLE(radv_cmd_buffer, cmd_buffer, commandBuffer);
   struct radv_draw_info info;

   info.indexed = true;
   info.count = indexCount;
   info.instance_count = instanceCount;
   info.first_instance = firstInstance;
   info.strmout_va = 0;
   info.indirect_va = 0;

   if (!radv_before_draw(cmd_buffer, &info, 1, false))
      return;
   const VkMultiDrawIndexedInfoEXT minfo = {firstIndex, indexCount, vertexOffset};
   radv_emit_draw_packets_indexed(cmd_buffer, &info, 1, &minfo, 0, NULL);
   radv_after_draw(cmd_buffer, false);
}

VKAPI_ATTR void VKAPI_CALL
radv_CmdDrawMultiIndexedEXT(VkCommandBuffer commandBuffer, uint32_t drawCount,
                            const VkMultiDrawIndexedInfoEXT *pIndexInfo, uint32_t instanceCount, uint32_t firstInstance,
                            uint32_t stride, const int32_t *pVertexOffset)
{
   VK_FROM_HANDLE(radv_cmd_buffer, cmd_buffer, commandBuffer);
   struct radv_draw_info info;

   if (!drawCount)
      return;

   const VkMultiDrawIndexedInfoEXT *minfo = pIndexInfo;
   info.indexed = true;
   info.count = minfo->indexCount;
   info.instance_count = instanceCount;
   info.first_instance = firstInstance;
   info.strmout_va = 0;
   info.indirect_va = 0;

   if (!radv_before_draw(cmd_buffer, &info, drawCount, false))
      return;
   radv_emit_draw_packets_indexed(cmd_buffer, &info, drawCount, pIndexInfo, stride, pVertexOffset);
   radv_after_draw(cmd_buffer, false);
}

VKAPI_ATTR void VKAPI_CALL
radv_CmdDrawIndirect(VkCommandBuffer commandBuffer, VkBuffer _buffer, VkDeviceSize offset, uint32_t drawCount,
                     uint32_t stride)
{
   VK_FROM_HANDLE(radv_cmd_buffer, cmd_buffer, commandBuffer);
   VK_FROM_HANDLE(radv_buffer, buffer, _buffer);
   struct radv_device *device = radv_cmd_buffer_device(cmd_buffer);
   struct radv_cmd_stream *cs = cmd_buffer->cs;
   struct radv_draw_info info;

   info.count = drawCount;
   info.indirect_va = vk_buffer_address(&buffer->vk, offset);
   info.stride = stride;
   info.strmout_va = 0;
   info.count_va = 0;
   info.indexed = false;
   info.instance_count = 0;

   radv_cs_add_buffer(device->ws, cs->b, buffer->bo);

   if (!radv_before_draw(cmd_buffer, &info, 1, false))
      return;
   radv_emit_indirect_draw_packets(cmd_buffer, &info);
   radv_after_draw(cmd_buffer, false);
}

VKAPI_ATTR void VKAPI_CALL
radv_CmdDrawIndexedIndirect(VkCommandBuffer commandBuffer, VkBuffer _buffer, VkDeviceSize offset, uint32_t drawCount,
                            uint32_t stride)
{
   VK_FROM_HANDLE(radv_cmd_buffer, cmd_buffer, commandBuffer);
   VK_FROM_HANDLE(radv_buffer, buffer, _buffer);
   struct radv_device *device = radv_cmd_buffer_device(cmd_buffer);
   struct radv_cmd_stream *cs = cmd_buffer->cs;
   struct radv_draw_info info;

   info.indexed = true;
   info.count = drawCount;
   info.indirect_va = vk_buffer_address(&buffer->vk, offset);
   info.stride = stride;
   info.count_va = 0;
   info.strmout_va = 0;
   info.instance_count = 0;

   radv_cs_add_buffer(device->ws, cs->b, buffer->bo);

   if (!radv_before_draw(cmd_buffer, &info, 1, false))
      return;
   radv_emit_indirect_draw_packets(cmd_buffer, &info);
   radv_after_draw(cmd_buffer, false);
}

VKAPI_ATTR void VKAPI_CALL
radv_CmdDrawIndirectCount(VkCommandBuffer commandBuffer, VkBuffer _buffer, VkDeviceSize offset, VkBuffer _countBuffer,
                          VkDeviceSize countBufferOffset, uint32_t maxDrawCount, uint32_t stride)
{
   VK_FROM_HANDLE(radv_cmd_buffer, cmd_buffer, commandBuffer);
   VK_FROM_HANDLE(radv_buffer, buffer, _buffer);
   VK_FROM_HANDLE(radv_buffer, count_buffer, _countBuffer);
   struct radv_device *device = radv_cmd_buffer_device(cmd_buffer);
   struct radv_cmd_stream *cs = cmd_buffer->cs;
   struct radv_draw_info info;

   info.count = maxDrawCount;
   info.indirect_va = vk_buffer_address(&buffer->vk, offset);
   info.count_va = vk_buffer_address(&count_buffer->vk, countBufferOffset);
   info.stride = stride;
   info.strmout_va = 0;
   info.indexed = false;
   info.instance_count = 0;

   radv_cs_add_buffer(device->ws, cs->b, buffer->bo);
   radv_cs_add_buffer(device->ws, cs->b, count_buffer->bo);

   if (!radv_before_draw(cmd_buffer, &info, 1, false))
      return;
   radv_emit_indirect_draw_packets(cmd_buffer, &info);
   radv_after_draw(cmd_buffer, false);
}

VKAPI_ATTR void VKAPI_CALL
radv_CmdDrawIndexedIndirectCount(VkCommandBuffer commandBuffer, VkBuffer _buffer, VkDeviceSize offset,
                                 VkBuffer _countBuffer, VkDeviceSize countBufferOffset, uint32_t maxDrawCount,
                                 uint32_t stride)
{
   VK_FROM_HANDLE(radv_cmd_buffer, cmd_buffer, commandBuffer);
   VK_FROM_HANDLE(radv_buffer, buffer, _buffer);
   VK_FROM_HANDLE(radv_buffer, count_buffer, _countBuffer);
   struct radv_device *device = radv_cmd_buffer_device(cmd_buffer);
   struct radv_cmd_stream *cs = cmd_buffer->cs;
   struct radv_draw_info info;

   info.indexed = true;
   info.count = maxDrawCount;
   info.indirect_va = vk_buffer_address(&buffer->vk, offset);
   info.count_va = vk_buffer_address(&count_buffer->vk, countBufferOffset);
   info.stride = stride;
   info.strmout_va = 0;
   info.instance_count = 0;

   radv_cs_add_buffer(device->ws, cs->b, buffer->bo);
   radv_cs_add_buffer(device->ws, cs->b, count_buffer->bo);

   if (!radv_before_draw(cmd_buffer, &info, 1, false))
      return;
   radv_emit_indirect_draw_packets(cmd_buffer, &info);
   radv_after_draw(cmd_buffer, false);
}

VKAPI_ATTR void VKAPI_CALL
radv_CmdDrawMeshTasksEXT(VkCommandBuffer commandBuffer, uint32_t x, uint32_t y, uint32_t z)
{
   VK_FROM_HANDLE(radv_cmd_buffer, cmd_buffer, commandBuffer);
   struct radv_device *device = radv_cmd_buffer_device(cmd_buffer);
   struct radv_cmd_stream *cs = cmd_buffer->cs;
   struct radv_draw_info info;

   info.count = x * y * z;
   info.instance_count = 1;
   info.first_instance = 0;
   info.stride = 0;
   info.indexed = false;
   info.strmout_va = 0;
   info.count_va = 0;
   info.indirect_va = 0;

   if (!radv_before_taskmesh_draw(cmd_buffer, &info, 1, false))
      return;

   if (radv_cmdbuf_has_stage(cmd_buffer, MESA_SHADER_TASK)) {
      radv_emit_direct_taskmesh_draw_packets(device, &cmd_buffer->state, cs, cmd_buffer->gang.cs, x, y, z);
   } else {
      radv_emit_direct_mesh_draw_packet(cmd_buffer, x, y, z);
   }

   radv_after_draw(cmd_buffer, false);
}

VKAPI_ATTR void VKAPI_CALL
radv_CmdDrawMeshTasksIndirectEXT(VkCommandBuffer commandBuffer, VkBuffer _buffer, VkDeviceSize offset,
                                 uint32_t drawCount, uint32_t stride)
{
   VK_FROM_HANDLE(radv_cmd_buffer, cmd_buffer, commandBuffer);
   VK_FROM_HANDLE(radv_buffer, buffer, _buffer);
   struct radv_device *device = radv_cmd_buffer_device(cmd_buffer);
   struct radv_cmd_stream *cs = cmd_buffer->cs;
   struct radv_draw_info info;

   info.indirect_va = vk_buffer_address(&buffer->vk, offset);
   info.stride = stride;
   info.count = drawCount;
   info.strmout_va = 0;
   info.count_va = 0;
   info.indexed = false;
   info.instance_count = 0;

   radv_cs_add_buffer(device->ws, cs->b, buffer->bo);

   if (!radv_before_taskmesh_draw(cmd_buffer, &info, drawCount, false))
      return;

   if (radv_cmdbuf_has_stage(cmd_buffer, MESA_SHADER_TASK)) {
      radv_emit_indirect_taskmesh_draw_packets(device, &cmd_buffer->state, cs, cmd_buffer->gang.cs, &info, 0);
   } else {
      radv_emit_indirect_mesh_draw_packets(cmd_buffer, &info);
   }

   radv_after_draw(cmd_buffer, false);
}

VKAPI_ATTR void VKAPI_CALL
radv_CmdDrawMeshTasksIndirectCountEXT(VkCommandBuffer commandBuffer, VkBuffer _buffer, VkDeviceSize offset,
                                      VkBuffer _countBuffer, VkDeviceSize countBufferOffset, uint32_t maxDrawCount,
                                      uint32_t stride)
{

   VK_FROM_HANDLE(radv_cmd_buffer, cmd_buffer, commandBuffer);
   VK_FROM_HANDLE(radv_buffer, buffer, _buffer);
   VK_FROM_HANDLE(radv_buffer, count_buffer, _countBuffer);
   struct radv_device *device = radv_cmd_buffer_device(cmd_buffer);
   const struct radv_physical_device *pdev = radv_device_physical(device);
   struct radv_cmd_stream *cs = cmd_buffer->cs;
   struct radv_draw_info info;

   info.indirect_va = vk_buffer_address(&buffer->vk, offset);
   info.stride = stride;
   info.count = maxDrawCount;
   info.strmout_va = 0;
   info.count_va = vk_buffer_address(&count_buffer->vk, countBufferOffset);
   info.indexed = false;
   info.instance_count = 0;

   radv_cs_add_buffer(device->ws, cs->b, buffer->bo);
   radv_cs_add_buffer(device->ws, cs->b, count_buffer->bo);

   if (!radv_before_taskmesh_draw(cmd_buffer, &info, maxDrawCount, false))
      return;

   if (radv_cmdbuf_has_stage(cmd_buffer, MESA_SHADER_TASK)) {
      uint64_t workaround_cond_va = 0;

      if (pdev->info.has_taskmesh_indirect0_bug && info.count_va) {
         /* Allocate a 32-bit value for the MEC firmware bug workaround. */
         uint32_t workaround_cond_init = 0;
         uint32_t workaround_cond_off;

         if (!radv_cmd_buffer_upload_data(cmd_buffer, 4, &workaround_cond_init, &workaround_cond_off)) {
            vk_command_buffer_set_error(&cmd_buffer->vk, VK_ERROR_OUT_OF_HOST_MEMORY);
            return;
         }

         workaround_cond_va = radv_buffer_get_va(cmd_buffer->upload.upload_bo) + workaround_cond_off;
      }

      radv_emit_indirect_taskmesh_draw_packets(device, &cmd_buffer->state, cs, cmd_buffer->gang.cs, &info,
                                               workaround_cond_va);
   } else {
      radv_emit_indirect_mesh_draw_packets(cmd_buffer, &info);
   }

   radv_after_draw(cmd_buffer, false);
}

static void radv_before_dispatch(struct radv_cmd_buffer *cmd_buffer, struct radv_compute_pipeline *pipeline,
                                 VkPipelineBindPoint bind_point);
static void radv_after_dispatch(struct radv_cmd_buffer *cmd_buffer, VkPipelineBindPoint bind_point, bool dgc);

/* VK_EXT_device_generated_commands */
static void
radv_dgc_execute_ib(struct radv_cmd_buffer *cmd_buffer, const VkGeneratedCommandsInfoEXT *pGeneratedCommandsInfo)
{
   const struct radv_device *device = radv_cmd_buffer_device(cmd_buffer);
   const VkGeneratedCommandsPipelineInfoEXT *pipeline_info =
      vk_find_struct_const(pGeneratedCommandsInfo->pNext, GENERATED_COMMANDS_PIPELINE_INFO_EXT);
   const VkGeneratedCommandsShaderInfoEXT *eso_info =
      vk_find_struct_const(pGeneratedCommandsInfo->pNext, GENERATED_COMMANDS_SHADER_INFO_EXT);
   const struct radv_shader *task_shader = radv_dgc_get_shader(pipeline_info, eso_info, MESA_SHADER_TASK);
   const uint32_t cmdbuf_size = radv_get_indirect_main_cmdbuf_size(pGeneratedCommandsInfo);
   const uint64_t ib_va = pGeneratedCommandsInfo->preprocessAddress;
   const uint64_t main_ib_va = ib_va + radv_get_indirect_main_cmdbuf_offset(pGeneratedCommandsInfo);
   const uint64_t main_trailer_va = ib_va + radv_get_indirect_main_trailer_offset(pGeneratedCommandsInfo);
   struct radv_cmd_stream *cs = cmd_buffer->cs;

   radeon_check_space(device->ws, cs->b, 64);

   device->ws->cs_chain_dgc_ib(cs->b, main_ib_va, cmdbuf_size >> 2, main_trailer_va, cmd_buffer->state.predicating);

   if (task_shader) {
      const uint32_t ace_cmdbuf_size = radv_get_indirect_ace_cmdbuf_size(pGeneratedCommandsInfo);
      const uint64_t ace_ib_va = ib_va + radv_get_indirect_ace_cmdbuf_offset(pGeneratedCommandsInfo);
      const uint64_t ace_trailer_va = ib_va + radv_get_indirect_ace_trailer_offset(pGeneratedCommandsInfo);
      struct radv_cmd_stream *ace_cs = cmd_buffer->gang.cs;

      assert(ace_cs->b);
      device->ws->cs_chain_dgc_ib(ace_cs->b, ace_ib_va, ace_cmdbuf_size >> 2, ace_trailer_va,
                                  cmd_buffer->state.predicating);
   }
}

VKAPI_ATTR void VKAPI_CALL
radv_CmdExecuteGeneratedCommandsEXT(VkCommandBuffer commandBuffer, VkBool32 isPreprocessed,
                                    const VkGeneratedCommandsInfoEXT *pGeneratedCommandsInfo)
{
   VK_FROM_HANDLE(radv_indirect_command_layout, layout, pGeneratedCommandsInfo->indirectCommandsLayout);
   VK_FROM_HANDLE(radv_indirect_execution_set, ies, pGeneratedCommandsInfo->indirectExecutionSet);
   VK_FROM_HANDLE(radv_cmd_buffer, cmd_buffer, commandBuffer);
   const struct radv_device *device = radv_cmd_buffer_device(cmd_buffer);
   const bool use_predication = radv_use_dgc_predication(cmd_buffer, pGeneratedCommandsInfo);
   const bool compute = !!(layout->vk.dgc_info & BITFIELD_BIT(MESA_VK_DGC_DISPATCH));
   const bool rt = !!(layout->vk.dgc_info & BITFIELD_BIT(MESA_VK_DGC_RT));
   const VkGeneratedCommandsPipelineInfoEXT *pipeline_info =
      vk_find_struct_const(pGeneratedCommandsInfo->pNext, GENERATED_COMMANDS_PIPELINE_INFO_EXT);
   const VkGeneratedCommandsShaderInfoEXT *eso_info =
      vk_find_struct_const(pGeneratedCommandsInfo->pNext, GENERATED_COMMANDS_SHADER_INFO_EXT);
   struct radv_cmd_stream *cs = cmd_buffer->cs;

   if (ies) {
      radv_cs_add_buffer(device->ws, cs->b, ies->bo);

      cmd_buffer->compute_scratch_size_per_wave_needed =
         MAX2(cmd_buffer->compute_scratch_size_per_wave_needed, ies->compute_scratch_size_per_wave);
      cmd_buffer->compute_scratch_waves_wanted =
         MAX2(cmd_buffer->compute_scratch_waves_wanted, ies->compute_scratch_waves);
   }

   /* Secondary command buffers are banned. */
   assert(cmd_buffer->vk.level == VK_COMMAND_BUFFER_LEVEL_PRIMARY);

   if (use_predication) {
      const uint64_t va = pGeneratedCommandsInfo->sequenceCountAddress;
      radv_begin_conditional_rendering(cmd_buffer, va, true);
   }

   if (!(layout->vk.usage & VK_INDIRECT_COMMANDS_LAYOUT_USAGE_EXPLICIT_PREPROCESS_BIT_EXT)) {
      /* Suspend conditional rendering when the DGC execute is called on the compute queue to
       * generate a cmdbuf which will skips dispatches when necessary. This is because the compute
       * queue is missing IB2 which means it's not possible to skip the cmdbuf entirely. This
       * should also be suspended when task shaders are used because the DGC ACE IB would be
       * uninitialized otherwise.
       */
      const bool suspend_conditional_rendering =
         (cmd_buffer->qf == RADV_QUEUE_COMPUTE || radv_dgc_get_shader(pipeline_info, eso_info, MESA_SHADER_TASK));
      const bool old_predicating = cmd_buffer->state.predicating;

      if (suspend_conditional_rendering && cmd_buffer->state.predicating) {
         cmd_buffer->state.predicating = false;
      }

      radv_prepare_dgc(cmd_buffer, pGeneratedCommandsInfo, cmd_buffer, old_predicating);

      if (suspend_conditional_rendering) {
         cmd_buffer->state.predicating = old_predicating;
      }

      cmd_buffer->state.flush_bits |= RADV_CMD_FLAG_CS_PARTIAL_FLUSH | RADV_CMD_FLAG_INV_VCACHE | RADV_CMD_FLAG_INV_L2;

      /* Make sure the DGC ACE IB will wait for the DGC prepare shader before the execution
       * starts.
       */
      if (radv_dgc_get_shader(pipeline_info, eso_info, MESA_SHADER_TASK)) {
         radv_gang_barrier(cmd_buffer, VK_PIPELINE_STAGE_2_COMMAND_PREPROCESS_BIT_NV,
                           VK_PIPELINE_STAGE_2_DRAW_INDIRECT_BIT);
      }
   }

   if (rt) {
      struct radv_compute_pipeline *compute_pipeline = NULL;

      if (pipeline_info) {
         VK_FROM_HANDLE(radv_pipeline, pipeline, pipeline_info->pipeline);
         compute_pipeline = &radv_pipeline_to_ray_tracing(pipeline)->base;
      }

      radv_before_dispatch(cmd_buffer, compute_pipeline, VK_PIPELINE_BIND_POINT_RAY_TRACING_KHR);
   } else if (compute) {
      struct radv_compute_pipeline *compute_pipeline = NULL;

      if (pipeline_info) {
         VK_FROM_HANDLE(radv_pipeline, pipeline, pipeline_info->pipeline);
         compute_pipeline = radv_pipeline_to_compute(pipeline);
      }

      radv_before_dispatch(cmd_buffer, compute_pipeline, VK_PIPELINE_BIND_POINT_COMPUTE);
   } else {
      struct radv_draw_info info = {
         .count = pGeneratedCommandsInfo->maxSequenceCount,
         .indirect_va = (uintptr_t)&info,
         .indexed = !!(layout->vk.dgc_info & BITFIELD_BIT(MESA_VK_DGC_DRAW_INDEXED)),
      };

      if (layout->vk.dgc_info & BITFIELD_BIT(MESA_VK_DGC_DRAW_MESH)) {
         if (!radv_before_taskmesh_draw(cmd_buffer, &info, 1, true))
            return;
      } else {
         if (!radv_before_draw(cmd_buffer, &info, 1, true))
            return;
      }
   }

   if (!radv_cmd_buffer_uses_mec(cmd_buffer)) {
      radeon_check_space(device->ws, cs->b, 2);

      radeon_begin(cs);
      radeon_emit(PKT3(PKT3_PFP_SYNC_ME, 0, cmd_buffer->state.predicating));
      radeon_emit(0);
      radeon_end();
   }

   radv_dgc_execute_ib(cmd_buffer, pGeneratedCommandsInfo);

   if (rt) {
      cmd_buffer->push_constant_stages |= RADV_RT_STAGE_BITS;

      radv_after_dispatch(cmd_buffer, VK_PIPELINE_BIND_POINT_RAY_TRACING_KHR, true);
   } else if (compute) {
      cmd_buffer->push_constant_stages |= VK_SHADER_STAGE_COMPUTE_BIT;

      if (ies)
         radv_mark_descriptors_dirty(cmd_buffer, VK_PIPELINE_BIND_POINT_COMPUTE);

      radv_after_dispatch(cmd_buffer, VK_PIPELINE_BIND_POINT_COMPUTE, true);
   } else {
      if (layout->vk.dgc_info & BITFIELD_BIT(MESA_VK_DGC_IB)) {
         cmd_buffer->state.last_index_type = -1;
         cmd_buffer->state.dirty |= RADV_CMD_DIRTY_INDEX_BUFFER;
      }

      if (layout->vk.dgc_info & BITFIELD_BIT(MESA_VK_DGC_VB))
         cmd_buffer->state.dirty |= RADV_CMD_DIRTY_VERTEX_BUFFER;

      if (pipeline_info) {
         VK_FROM_HANDLE(radv_pipeline, pipeline, pipeline_info->pipeline);
         struct radv_graphics_pipeline *graphics_pipeline = radv_pipeline_to_graphics(pipeline);

         cmd_buffer->push_constant_stages |= graphics_pipeline->active_stages;
      } else {
         assert(eso_info);

         for (unsigned i = 0; i < eso_info->shaderCount; ++i) {
            VK_FROM_HANDLE(radv_shader_object, shader_object, eso_info->pShaders[i]);

            cmd_buffer->push_constant_stages |= mesa_to_vk_shader_stage(shader_object->stage);
         }
      }

      if (!(layout->vk.dgc_info & BITFIELD_BIT(MESA_VK_DGC_DRAW_INDEXED))) {
         /* Non-indexed draws overwrite VGT_INDEX_TYPE, so the state must be
          * re-emitted before the next indexed draw.
          */
         cmd_buffer->state.last_index_type = -1;
      }

      cmd_buffer->state.last_num_instances = -1;
      cmd_buffer->state.last_vertex_offset_valid = false;
      cmd_buffer->state.last_first_instance = -1;
      cmd_buffer->state.last_drawid = -1;

      radv_after_draw(cmd_buffer, true);
   }

   if (use_predication) {
      radv_end_conditional_rendering(cmd_buffer);
   }
}

static void
radv_save_dispatch_size(struct radv_cmd_buffer *cmd_buffer, uint64_t indirect_va)
{
   struct radv_device *device = radv_cmd_buffer_device(cmd_buffer);
   struct radv_cmd_stream *cs = cmd_buffer->cs;

   radeon_check_space(device->ws, cs->b, 18);

   uint64_t va = radv_buffer_get_va(device->trace_bo) + offsetof(struct radv_trace_data, indirect_dispatch);

   for (uint32_t i = 0; i < 3; i++) {
      ac_emit_cp_copy_data(cs->b, COPY_DATA_SRC_MEM, COPY_DATA_DST_MEM, indirect_va, va, AC_CP_COPY_DATA_WR_CONFIRM);

      indirect_va += 4;
      va += 4;
   }
}

static void
radv_emit_dispatch_packets(struct radv_cmd_buffer *cmd_buffer, const struct radv_shader *compute_shader,
                           const struct radv_dispatch_info *info)
{
   struct radv_device *device = radv_cmd_buffer_device(cmd_buffer);
   const struct radv_physical_device *pdev = radv_device_physical(device);
   unsigned dispatch_initiator = device->dispatch_initiator;
   struct radeon_winsys *ws = device->ws;
   bool predicating = cmd_buffer->state.predicating;
   struct radv_cmd_stream *cs = cmd_buffer->cs;
   const uint32_t grid_size_offset = radv_get_user_sgpr_loc(compute_shader, AC_UD_CS_GRID_SIZE);

   radv_describe_dispatch(cmd_buffer, info);

   ASSERTED unsigned cdw_max = radeon_check_space(ws, cs->b, 30);

   if (compute_shader->info.wave_size == 32) {
      assert(pdev->info.gfx_level >= GFX10);
      dispatch_initiator |= S_00B800_CS_W32_EN(1);
   }

   if (info->ordered)
      dispatch_initiator &= ~S_00B800_ORDER_MODE(1);

   if (info->indirect_va) {
      if (radv_device_fault_detection_enabled(device))
         radv_save_dispatch_size(cmd_buffer, info->indirect_va);

      if (info->unaligned) {
         radeon_begin(cs);
         radeon_set_sh_reg_seq(R_00B81C_COMPUTE_NUM_THREAD_X, 3);
         if (pdev->info.gfx_level >= GFX12) {
            radeon_emit(S_00B81C_NUM_THREAD_FULL_GFX12(compute_shader->info.cs.block_size[0]));
            radeon_emit(S_00B820_NUM_THREAD_FULL_GFX12(compute_shader->info.cs.block_size[1]));
         } else {
            radeon_emit(S_00B81C_NUM_THREAD_FULL_GFX6(compute_shader->info.cs.block_size[0]));
            radeon_emit(S_00B820_NUM_THREAD_FULL_GFX6(compute_shader->info.cs.block_size[1]));
         }
         radeon_emit(S_00B824_NUM_THREAD_FULL(compute_shader->info.cs.block_size[2]));
         radeon_end();

         dispatch_initiator |= S_00B800_USE_THREAD_DIMENSIONS(1);
      }

      /* Indirect CS does not support offsets in the API. Must program this in case there have been
       * preceding 1D RT dispatch or vkCmdDispatchBase. */
      dispatch_initiator |= S_00B800_FORCE_START_AT_000(1);

      if (grid_size_offset) {
         radeon_begin(cs);

         if (device->load_grid_size_from_user_sgpr) {
            assert(pdev->info.gfx_level >= GFX10_3);

            radeon_emit(PKT3(PKT3_LOAD_SH_REG_INDEX, 3, 0));
            radeon_emit(info->indirect_va);
            radeon_emit(info->indirect_va >> 32);
            radeon_emit((grid_size_offset - SI_SH_REG_OFFSET) >> 2);
            radeon_emit(3);
         } else {
            radeon_emit_64bit_pointer(grid_size_offset, info->indirect_va);
         }

         radeon_end();
      }

      if (radv_cmd_buffer_uses_mec(cmd_buffer)) {
         uint64_t indirect_va = info->indirect_va;
         const bool needs_align32_workaround = pdev->info.has_async_compute_align32_bug &&
                                               cmd_buffer->qf == RADV_QUEUE_COMPUTE &&
                                               !util_is_aligned(indirect_va, 32);
         const unsigned ace_predication_size =
            4 /* DISPATCH_INDIRECT */ + (needs_align32_workaround ? 6 * 3 /* 3x COPY_DATA */ : 0);

         radv_cs_emit_compute_predication(device, &cmd_buffer->state, cs, cmd_buffer->state.mec_inv_pred_va,
                                          &cmd_buffer->state.mec_inv_pred_emitted, ace_predication_size);

         if (needs_align32_workaround) {
            const uint64_t unaligned_va = indirect_va;
            UNUSED void *ptr;
            uint32_t offset;

            if (!radv_cmd_buffer_upload_alloc_aligned(cmd_buffer, sizeof(VkDispatchIndirectCommand), 32, &offset, &ptr))
               return;

            indirect_va = radv_buffer_get_va(cmd_buffer->upload.upload_bo) + offset;

            for (uint32_t i = 0; i < 3; i++) {
               const uint64_t src_va = unaligned_va + i * 4;
               const uint64_t dst_va = indirect_va + i * 4;

               ac_emit_cp_copy_data(cs->b, COPY_DATA_SRC_MEM, COPY_DATA_DST_MEM, src_va, dst_va,
                                    AC_CP_COPY_DATA_WR_CONFIRM);
            }
         }

         radeon_begin(cs);
         radeon_emit(PKT3(PKT3_DISPATCH_INDIRECT, 2, 0) | PKT3_SHADER_TYPE_S(1));
         radeon_emit(indirect_va);
         radeon_emit(indirect_va >> 32);
         radeon_emit(dispatch_initiator);
         radeon_end();
      } else {
         radv_emit_indirect_buffer(cs, info->indirect_va, true);

         if (cmd_buffer->qf == RADV_QUEUE_COMPUTE) {
            radv_cs_emit_compute_predication(device, &cmd_buffer->state, cs, cmd_buffer->state.mec_inv_pred_va,
                                             &cmd_buffer->state.mec_inv_pred_emitted, 3 /* PKT3_DISPATCH_INDIRECT */);
            predicating = false;
         }

         radeon_begin(cs);
         radeon_emit(PKT3(PKT3_DISPATCH_INDIRECT, 1, predicating) | PKT3_SHADER_TYPE_S(1));
         radeon_emit(0);
         radeon_emit(dispatch_initiator);
         radeon_end();
      }
   } else {
      const unsigned *cs_block_size = compute_shader->info.cs.block_size;
      unsigned blocks[3] = {info->blocks[0], info->blocks[1], info->blocks[2]};
      unsigned offsets[3] = {info->offsets[0], info->offsets[1], info->offsets[2]};

      if (info->unaligned) {
         unsigned remainder[3];

         /* If aligned, these should be an entire block size,
          * not 0.
          */
         remainder[0] = blocks[0] + cs_block_size[0] - ALIGN_NPOT(blocks[0], cs_block_size[0]);
         remainder[1] = blocks[1] + cs_block_size[1] - ALIGN_NPOT(blocks[1], cs_block_size[1]);
         remainder[2] = blocks[2] + cs_block_size[2] - ALIGN_NPOT(blocks[2], cs_block_size[2]);

         blocks[0] = DIV_ROUND_UP(blocks[0], cs_block_size[0]);
         blocks[1] = DIV_ROUND_UP(blocks[1], cs_block_size[1]);
         blocks[2] = DIV_ROUND_UP(blocks[2], cs_block_size[2]);

         for (unsigned i = 0; i < 3; ++i) {
            assert(offsets[i] % cs_block_size[i] == 0);
            offsets[i] /= cs_block_size[i];
         }

         radeon_begin(cs);
         radeon_set_sh_reg_seq(R_00B81C_COMPUTE_NUM_THREAD_X, 3);
         if (pdev->info.gfx_level >= GFX12) {
            radeon_emit(S_00B81C_NUM_THREAD_FULL_GFX12(cs_block_size[0]) | S_00B81C_NUM_THREAD_PARTIAL(remainder[0]));
            radeon_emit(S_00B820_NUM_THREAD_FULL_GFX12(cs_block_size[1]) | S_00B820_NUM_THREAD_PARTIAL(remainder[1]));
         } else {
            radeon_emit(S_00B81C_NUM_THREAD_FULL_GFX6(cs_block_size[0]) | S_00B81C_NUM_THREAD_PARTIAL(remainder[0]));
            radeon_emit(S_00B820_NUM_THREAD_FULL_GFX6(cs_block_size[1]) | S_00B820_NUM_THREAD_PARTIAL(remainder[1]));
         }
         radeon_emit(S_00B824_NUM_THREAD_FULL(cs_block_size[2]) | S_00B824_NUM_THREAD_PARTIAL(remainder[2]));
         radeon_end();

         dispatch_initiator |= S_00B800_PARTIAL_TG_EN(1);
      }

      if (grid_size_offset) {
         if (device->load_grid_size_from_user_sgpr) {
            radeon_begin(cs);
            radeon_set_sh_reg_seq(grid_size_offset, 3);
            radeon_emit(blocks[0]);
            radeon_emit(blocks[1]);
            radeon_emit(blocks[2]);
            radeon_end();
         } else {
            uint32_t offset;
            if (!radv_cmd_buffer_upload_data(cmd_buffer, 12, blocks, &offset))
               return;

            uint64_t va = radv_buffer_get_va(cmd_buffer->upload.upload_bo) + offset;

            radeon_begin(cs);
            radeon_emit_64bit_pointer(grid_size_offset, va);
            radeon_end();
         }
      }

      if (offsets[0] || offsets[1] || offsets[2]) {
         radeon_begin(cs);
         radeon_set_sh_reg_seq(R_00B810_COMPUTE_START_X, 3);
         radeon_emit(offsets[0]);
         radeon_emit(offsets[1]);
         radeon_emit(offsets[2]);
         radeon_end();

         /* The blocks in the packet are not counts but end values. */
         for (unsigned i = 0; i < 3; ++i)
            blocks[i] += offsets[i];
      } else {
         dispatch_initiator |= S_00B800_FORCE_START_AT_000(1);
      }

      if (cmd_buffer->qf == RADV_QUEUE_COMPUTE) {
         radv_cs_emit_compute_predication(device, &cmd_buffer->state, cs, cmd_buffer->state.mec_inv_pred_va,
                                          &cmd_buffer->state.mec_inv_pred_emitted, 5 /* DISPATCH_DIRECT size */);
         predicating = false;
      }

      if (pdev->info.has_async_compute_threadgroup_bug && cmd_buffer->qf == RADV_QUEUE_COMPUTE) {
         for (unsigned i = 0; i < 3; i++) {
            if (info->unaligned) {
               /* info->blocks is already in thread dimensions for unaligned dispatches. */
               blocks[i] = info->blocks[i];
            } else {
               /* Force the async compute dispatch to be in "thread" dim mode to workaround a hw bug. */
               blocks[i] *= cs_block_size[i];
            }

            dispatch_initiator |= S_00B800_USE_THREAD_DIMENSIONS(1);
         }
      }

      radeon_begin(cs);
      radeon_emit(PKT3(PKT3_DISPATCH_DIRECT, 3, predicating) | PKT3_SHADER_TYPE_S(1));
      radeon_emit(blocks[0]);
      radeon_emit(blocks[1]);
      radeon_emit(blocks[2]);
      radeon_emit(dispatch_initiator);
      radeon_end();
   }

   assert(cs->b->cdw <= cdw_max);
}

static void
radv_upload_compute_shader_descriptors(struct radv_cmd_buffer *cmd_buffer, VkPipelineBindPoint bind_point)
{
   struct radv_descriptor_state *descriptors_state = radv_get_descriptors_state(cmd_buffer, bind_point);

   if (descriptors_state->dirty) {
      radv_flush_descriptors(cmd_buffer, VK_SHADER_STAGE_COMPUTE_BIT, bind_point);
      descriptors_state->dirty = 0;
   }

   if (descriptors_state->dirty_dynamic && descriptors_state->dynamic_offset_count) {
      radv_flush_dynamic_descriptors(cmd_buffer, VK_SHADER_STAGE_COMPUTE_BIT, bind_point);
      descriptors_state->dirty_dynamic = false;
   }

   const VkShaderStageFlags stages =
      bind_point == VK_PIPELINE_BIND_POINT_RAY_TRACING_KHR ? RADV_RT_STAGE_BITS : VK_SHADER_STAGE_COMPUTE_BIT;
   const VkShaderStageFlags pc_stages = radv_must_flush_constants(cmd_buffer, stages, bind_point);
   if (pc_stages)
      radv_flush_constants(cmd_buffer, pc_stages, bind_point);
}

static void
radv_emit_rt_stack_size(struct radv_cmd_buffer *cmd_buffer)
{
   struct radv_device *device = radv_cmd_buffer_device(cmd_buffer);
   const struct radv_physical_device *pdev = radv_device_physical(device);
   const struct radv_shader *rt_prolog = cmd_buffer->state.rt_prolog;
   struct radv_cmd_stream *cs = cmd_buffer->cs;
   unsigned rsrc2 = rt_prolog->config.rsrc2;

   /* Reserve scratch for stacks manually since it is not handled by the compute path. */
   uint32_t scratch_bytes_per_wave = rt_prolog->config.scratch_bytes_per_wave;
   const uint32_t wave_size = rt_prolog->info.wave_size;

   scratch_bytes_per_wave +=
      align(cmd_buffer->state.rt_stack_size * wave_size, pdev->info.scratch_wavesize_granularity);

   cmd_buffer->compute_scratch_size_per_wave_needed =
      MAX2(cmd_buffer->compute_scratch_size_per_wave_needed, scratch_bytes_per_wave);

   if (cmd_buffer->state.rt_stack_size)
      rsrc2 |= S_00B12C_SCRATCH_EN(1);

   radeon_check_space(device->ws, cs->b, 3);

   radeon_begin(cs);
   if (pdev->info.gfx_level >= GFX12) {
      gfx12_push_sh_reg(rt_prolog->info.regs.pgm_rsrc2, rsrc2);
   } else {
      radeon_set_sh_reg(rt_prolog->info.regs.pgm_rsrc2, rsrc2);
   }
   radeon_end();
}

static void
radv_before_dispatch(struct radv_cmd_buffer *cmd_buffer, struct radv_compute_pipeline *pipeline,
                     VkPipelineBindPoint bind_point)
{
   struct radv_device *device = radv_cmd_buffer_device(cmd_buffer);
   const struct radv_physical_device *pdev = radv_device_physical(device);
   const bool pipeline_is_dirty = pipeline != cmd_buffer->state.emitted_compute_pipeline;
   struct radv_shader *compute_shader = bind_point == VK_PIPELINE_BIND_POINT_COMPUTE
                                           ? cmd_buffer->state.shaders[MESA_SHADER_COMPUTE]
                                           : cmd_buffer->state.rt_prolog;

   if (compute_shader->info.cs.regalloc_hang_bug)
      cmd_buffer->state.flush_bits |= RADV_CMD_FLAG_PS_PARTIAL_FLUSH | RADV_CMD_FLAG_CS_PARTIAL_FLUSH;

   /* Use the optimal packet order similar to draws. */
   if (pipeline)
      radv_emit_compute_pipeline(cmd_buffer, pipeline);
   if (bind_point == VK_PIPELINE_BIND_POINT_RAY_TRACING_KHR)
      radv_emit_rt_stack_size(cmd_buffer);

   radv_upload_compute_shader_descriptors(cmd_buffer, bind_point);

   if (pdev->info.gfx_level >= GFX12)
      radv_gfx12_emit_buffered_regs(device, cmd_buffer->cs);

   radv_emit_cache_flush(cmd_buffer);

   /* <-- CUs are idle here if shaders are synchronized. */

   if (pipeline_is_dirty) {
      /* Raytracing uses compute shaders but has separate bind points and pipelines.
       * So if we set compute userdata & shader registers we should dirty the raytracing
       * ones and the other way around.
       *
       * We only need to do this when the pipeline is dirty because when we switch between
       * the two we always need to switch pipelines.
       */
      if (bind_point == VK_PIPELINE_BIND_POINT_COMPUTE) {
         radv_mark_descriptors_dirty(cmd_buffer, VK_PIPELINE_BIND_POINT_RAY_TRACING_KHR);
         cmd_buffer->push_constant_stages |= RADV_RT_STAGE_BITS;
      } else {
         assert(bind_point == VK_PIPELINE_BIND_POINT_RAY_TRACING_KHR);
         radv_mark_descriptors_dirty(cmd_buffer, VK_PIPELINE_BIND_POINT_COMPUTE);
         cmd_buffer->push_constant_stages |= VK_SHADER_STAGE_COMPUTE_BIT;
      }
   }

   if (pdev->info.gfx_level >= GFX12)
      radv_gfx12_emit_buffered_regs(device, cmd_buffer->cs);
}

static void
radv_after_dispatch(struct radv_cmd_buffer *cmd_buffer, VkPipelineBindPoint bind_point, bool dgc)
{
   struct radv_device *device = radv_cmd_buffer_device(cmd_buffer);
   const struct radv_physical_device *pdev = radv_device_physical(device);
   struct radv_shader *compute_shader = bind_point == VK_PIPELINE_BIND_POINT_COMPUTE
                                           ? cmd_buffer->state.shaders[MESA_SHADER_COMPUTE]
                                           : cmd_buffer->state.rt_prolog;
   const bool has_prefetch = pdev->info.gfx_level >= GFX7;

   /* Start prefetches after the dispatch has been started. Both will run in parallel, but
    * starting the dispatch first is more important.
    */
   if (has_prefetch) {
      if (bind_point == VK_PIPELINE_BIND_POINT_COMPUTE) {
         radv_emit_compute_prefetch(cmd_buffer);
      } else {
         radv_emit_ray_tracing_prefetch(cmd_buffer);
      }
   }

   if (compute_shader->info.cs.regalloc_hang_bug)
      cmd_buffer->state.flush_bits |= RADV_CMD_FLAG_CS_PARTIAL_FLUSH;

   radv_cmd_buffer_after_draw(cmd_buffer, RADV_CMD_FLAG_CS_PARTIAL_FLUSH, dgc);
}

static void
radv_dispatch(struct radv_cmd_buffer *cmd_buffer, const struct radv_dispatch_info *info,
              struct radv_compute_pipeline *pipeline, const struct radv_shader *shader, VkPipelineBindPoint bind_point)
{
   radv_before_dispatch(cmd_buffer, pipeline, bind_point);
   radv_emit_dispatch_packets(cmd_buffer, shader, info);
   radv_after_dispatch(cmd_buffer, bind_point, false);
}

void
radv_compute_dispatch(struct radv_cmd_buffer *cmd_buffer, const struct radv_dispatch_info *info)
{
   struct radv_compute_pipeline *pipeline = cmd_buffer->state.compute_pipeline;
   const struct radv_shader *shader = cmd_buffer->state.shaders[MESA_SHADER_COMPUTE];

   radv_dispatch(cmd_buffer, info, pipeline, shader, VK_PIPELINE_BIND_POINT_COMPUTE);
}

static void
radv_rt_dispatch(struct radv_cmd_buffer *cmd_buffer, const struct radv_dispatch_info *info)
{
   struct radv_compute_pipeline *pipeline = &cmd_buffer->state.rt_pipeline->base;
   const struct radv_shader *shader = cmd_buffer->state.rt_prolog;

   radv_dispatch(cmd_buffer, info, pipeline, shader, VK_PIPELINE_BIND_POINT_RAY_TRACING_KHR);
}

VKAPI_ATTR void VKAPI_CALL
radv_CmdDispatchBase(VkCommandBuffer commandBuffer, uint32_t base_x, uint32_t base_y, uint32_t base_z, uint32_t x,
                     uint32_t y, uint32_t z)
{
   VK_FROM_HANDLE(radv_cmd_buffer, cmd_buffer, commandBuffer);
   struct radv_dispatch_info info = {0};

   info.blocks[0] = x;
   info.blocks[1] = y;
   info.blocks[2] = z;

   info.offsets[0] = base_x;
   info.offsets[1] = base_y;
   info.offsets[2] = base_z;
   radv_compute_dispatch(cmd_buffer, &info);
}

VKAPI_ATTR void VKAPI_CALL
radv_CmdDispatchIndirect(VkCommandBuffer commandBuffer, VkBuffer _buffer, VkDeviceSize offset)
{
   VK_FROM_HANDLE(radv_cmd_buffer, cmd_buffer, commandBuffer);
   VK_FROM_HANDLE(radv_buffer, buffer, _buffer);
   struct radv_device *device = radv_cmd_buffer_device(cmd_buffer);
   struct radv_dispatch_info info = {.indirect_va = vk_buffer_address(&buffer->vk, offset)};
   struct radv_cmd_stream *cs = cmd_buffer->cs;

   radv_cs_add_buffer(device->ws, cs->b, buffer->bo);

   radv_compute_dispatch(cmd_buffer, &info);
}

void
radv_unaligned_dispatch(struct radv_cmd_buffer *cmd_buffer, uint32_t x, uint32_t y, uint32_t z)
{
   struct radv_dispatch_info info = {0};

   info.blocks[0] = x;
   info.blocks[1] = y;
   info.blocks[2] = z;
   info.unaligned = 1;

   radv_compute_dispatch(cmd_buffer, &info);
}

static void
radv_trace_trace_rays(struct radv_cmd_buffer *cmd_buffer, const VkTraceRaysIndirectCommand2KHR *cmd,
                      uint64_t indirect_va)
{
   if (!cmd || indirect_va)
      return;

   struct radv_rra_ray_history_data *data = malloc(sizeof(struct radv_rra_ray_history_data));
   if (!data)
      return;

   struct radv_device *device = radv_cmd_buffer_device(cmd_buffer);
   uint32_t width = DIV_ROUND_UP(cmd->width, device->rra_trace.ray_history_resolution_scale);
   uint32_t height = DIV_ROUND_UP(cmd->height, device->rra_trace.ray_history_resolution_scale);
   uint32_t depth = DIV_ROUND_UP(cmd->depth, device->rra_trace.ray_history_resolution_scale);

   struct radv_rra_ray_history_counter counter = {
      .dispatch_size = {width, height, depth},
      .hit_shader_count = cmd->hitShaderBindingTableSize / cmd->hitShaderBindingTableStride,
      .miss_shader_count = cmd->missShaderBindingTableSize / cmd->missShaderBindingTableStride,
      .shader_count = cmd_buffer->state.rt_pipeline->stage_count,
      .pipeline_api_hash = cmd_buffer->state.rt_pipeline->base.base.pipeline_hash,
      .mode = 1,
      .stride = sizeof(uint32_t),
      .data_size = 0,
      .ray_id_begin = 0,
      .ray_id_end = 0xFFFFFFFF,
      .pipeline_type = RADV_RRA_PIPELINE_RAY_TRACING,
   };

   struct radv_rra_ray_history_dispatch_size dispatch_size = {
      .size = {width, height, depth},
   };

   struct radv_rra_ray_history_traversal_flags traversal_flags = {0};

   data->metadata = (struct radv_rra_ray_history_metadata){
      .counter_info.type = RADV_RRA_COUNTER_INFO,
      .counter_info.size = sizeof(struct radv_rra_ray_history_counter),
      .counter = counter,

      .dispatch_size_info.type = RADV_RRA_DISPATCH_SIZE,
      .dispatch_size_info.size = sizeof(struct radv_rra_ray_history_dispatch_size),
      .dispatch_size = dispatch_size,

      .traversal_flags_info.type = RADV_RRA_TRAVERSAL_FLAGS,
      .traversal_flags_info.size = sizeof(struct radv_rra_ray_history_traversal_flags),
      .traversal_flags = traversal_flags,
   };

   uint32_t dispatch_index = util_dynarray_num_elements(&cmd_buffer->ray_history, struct radv_rra_ray_history_data *)
                             << 16;

   util_dynarray_append(&cmd_buffer->ray_history, struct radv_rra_ray_history_data *, data);

   cmd_buffer->state.flush_bits |= RADV_CMD_FLAG_INV_SCACHE | RADV_CMD_FLAG_CS_PARTIAL_FLUSH |
                                   radv_src_access_flush(cmd_buffer, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                                                         VK_ACCESS_2_SHADER_WRITE_BIT, 0, NULL, NULL) |
                                   radv_dst_access_flush(cmd_buffer, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                                                         VK_ACCESS_2_SHADER_READ_BIT, 0, NULL, NULL);

   radv_update_memory_cp(cmd_buffer,
                         device->rra_trace.ray_history_addr + offsetof(struct radv_ray_history_header, dispatch_index),
                         &dispatch_index, sizeof(dispatch_index));
}

enum radv_rt_mode {
   radv_rt_mode_direct,
   radv_rt_mode_indirect,
   radv_rt_mode_indirect2,
};

static void
radv_upload_trace_rays_params(struct radv_cmd_buffer *cmd_buffer, VkTraceRaysIndirectCommand2KHR *tables,
                              enum radv_rt_mode mode, uint64_t *launch_size_va, uint64_t *sbt_va)
{
   uint32_t upload_size = mode == radv_rt_mode_direct ? sizeof(VkTraceRaysIndirectCommand2KHR)
                                                      : offsetof(VkTraceRaysIndirectCommand2KHR, width);

   uint32_t offset;
   if (!radv_cmd_buffer_upload_data(cmd_buffer, upload_size, tables, &offset))
      return;

   uint64_t upload_va = radv_buffer_get_va(cmd_buffer->upload.upload_bo) + offset;

   if (mode == radv_rt_mode_direct)
      *launch_size_va = upload_va + offsetof(VkTraceRaysIndirectCommand2KHR, width);
   if (sbt_va)
      *sbt_va = upload_va;
}

static void
radv_trace_rays(struct radv_cmd_buffer *cmd_buffer, VkTraceRaysIndirectCommand2KHR *tables, uint64_t indirect_va,
                enum radv_rt_mode mode)
{
   struct radv_device *device = radv_cmd_buffer_device(cmd_buffer);
   const struct radv_physical_device *pdev = radv_device_physical(device);
   const struct radv_instance *instance = radv_physical_device_instance(pdev);
   struct radv_cmd_stream *cs = cmd_buffer->cs;

   if (instance->debug_flags & RADV_DEBUG_NO_RT)
      return;

   radv_suspend_conditional_rendering(cmd_buffer);

   if (unlikely(device->rra_trace.ray_history_buffer))
      radv_trace_trace_rays(cmd_buffer, tables, indirect_va);

   struct radv_shader *rt_prolog = cmd_buffer->state.rt_prolog;

   /* Since the workgroup size is 8x4 (or 8x8), 1D dispatches can only fill 8 threads per wave at most. To increase
    * occupancy, it's beneficial to convert to a 2D dispatch in these cases. */
   if (tables && tables->height == 1 && tables->width >= cmd_buffer->state.rt_prolog->info.cs.block_size[0])
      tables->height = ACO_RT_CONVERTED_2D_LAUNCH_SIZE;

   struct radv_dispatch_info info = {0};
   info.unaligned = true;

   uint64_t launch_size_va = 0;
   uint64_t sbt_va = 0;

   if (mode != radv_rt_mode_indirect2) {
      launch_size_va = indirect_va;
      radv_upload_trace_rays_params(cmd_buffer, tables, mode, &launch_size_va, &sbt_va);
   } else {
      launch_size_va = indirect_va + offsetof(VkTraceRaysIndirectCommand2KHR, width);
      sbt_va = indirect_va;
   }

   uint32_t remaining_ray_count = 0;

   if (mode == radv_rt_mode_direct) {
      info.blocks[0] = tables->width;
      info.blocks[1] = tables->height;
      info.blocks[2] = tables->depth;

      if (tables->height == ACO_RT_CONVERTED_2D_LAUNCH_SIZE) {
         /* We need the ray count for the 2D dispatch to be a multiple of the y block size for the division to work, and
          * a multiple of the x block size because the invocation offset must be a multiple of the block size when
          * dispatching the remaining rays. Fortunately, the x block size is itself a multiple of the y block size, so
          * we only need to ensure that the ray count is a multiple of the x block size. */
         remaining_ray_count = tables->width % rt_prolog->info.cs.block_size[0];

         uint32_t ray_count = tables->width - remaining_ray_count;
         info.blocks[0] = ray_count / rt_prolog->info.cs.block_size[1];
         info.blocks[1] = rt_prolog->info.cs.block_size[1];
      }
   } else
      info.indirect_va = launch_size_va;

   ASSERTED unsigned cdw_max = radeon_check_space(device->ws, cs->b, 15);

   const uint32_t sbt_descriptors_offset = radv_get_user_sgpr_loc(rt_prolog, AC_UD_CS_SBT_DESCRIPTORS);
   if (sbt_descriptors_offset) {
      radeon_begin(cs);
      if (pdev->info.gfx_level >= GFX12) {
         gfx12_push_64bit_pointer(sbt_descriptors_offset, sbt_va);
      } else {
         radeon_emit_64bit_pointer(sbt_descriptors_offset, sbt_va);
      }
      radeon_end();
   }

   const uint32_t ray_launch_size_addr_offset = radv_get_user_sgpr_loc(rt_prolog, AC_UD_CS_RAY_LAUNCH_SIZE_ADDR);
   if (ray_launch_size_addr_offset) {
      radeon_begin(cs);
      if (pdev->info.gfx_level >= GFX12) {
         gfx12_push_64bit_pointer(ray_launch_size_addr_offset, launch_size_va);
      } else {
         radeon_emit_64bit_pointer(ray_launch_size_addr_offset, launch_size_va);
      }
      radeon_end();
   }

   assert(cs->b->cdw <= cdw_max);

   radv_rt_dispatch(cmd_buffer, &info);

   if (remaining_ray_count) {
      info.blocks[0] = remaining_ray_count;
      info.blocks[1] = 1;
      info.offsets[0] = tables->width - remaining_ray_count;

      /* Reset the ray launch size so the prolog doesn't think this is a converted dispatch */
      tables->height = 1;
      radv_upload_trace_rays_params(cmd_buffer, tables, mode, &launch_size_va, NULL);
      if (ray_launch_size_addr_offset) {
         radeon_begin(cs);
         if (pdev->info.gfx_level >= GFX12) {
            gfx12_push_64bit_pointer(ray_launch_size_addr_offset, launch_size_va);
         } else {
            radeon_emit_64bit_pointer(ray_launch_size_addr_offset, launch_size_va);
         }
         radeon_end();
      }

      radv_rt_dispatch(cmd_buffer, &info);
   }

   radv_resume_conditional_rendering(cmd_buffer);
}

VKAPI_ATTR void VKAPI_CALL
radv_CmdTraceRaysKHR(VkCommandBuffer commandBuffer, const VkStridedDeviceAddressRegionKHR *pRaygenShaderBindingTable,
                     const VkStridedDeviceAddressRegionKHR *pMissShaderBindingTable,
                     const VkStridedDeviceAddressRegionKHR *pHitShaderBindingTable,
                     const VkStridedDeviceAddressRegionKHR *pCallableShaderBindingTable, uint32_t width,
                     uint32_t height, uint32_t depth)
{
   VK_FROM_HANDLE(radv_cmd_buffer, cmd_buffer, commandBuffer);

   VkTraceRaysIndirectCommand2KHR tables = {
      .raygenShaderRecordAddress = pRaygenShaderBindingTable->deviceAddress,
      .raygenShaderRecordSize = pRaygenShaderBindingTable->size,
      .missShaderBindingTableAddress = pMissShaderBindingTable->deviceAddress,
      .missShaderBindingTableSize = pMissShaderBindingTable->size,
      .missShaderBindingTableStride = pMissShaderBindingTable->stride,
      .hitShaderBindingTableAddress = pHitShaderBindingTable->deviceAddress,
      .hitShaderBindingTableSize = pHitShaderBindingTable->size,
      .hitShaderBindingTableStride = pHitShaderBindingTable->stride,
      .callableShaderBindingTableAddress = pCallableShaderBindingTable->deviceAddress,
      .callableShaderBindingTableSize = pCallableShaderBindingTable->size,
      .callableShaderBindingTableStride = pCallableShaderBindingTable->stride,
      .width = width,
      .height = height,
      .depth = depth,
   };

   radv_trace_rays(cmd_buffer, &tables, 0, radv_rt_mode_direct);
}

VKAPI_ATTR void VKAPI_CALL
radv_CmdTraceRaysIndirectKHR(VkCommandBuffer commandBuffer,
                             const VkStridedDeviceAddressRegionKHR *pRaygenShaderBindingTable,
                             const VkStridedDeviceAddressRegionKHR *pMissShaderBindingTable,
                             const VkStridedDeviceAddressRegionKHR *pHitShaderBindingTable,
                             const VkStridedDeviceAddressRegionKHR *pCallableShaderBindingTable,
                             VkDeviceAddress indirectDeviceAddress)
{
   VK_FROM_HANDLE(radv_cmd_buffer, cmd_buffer, commandBuffer);
   struct radv_device *device = radv_cmd_buffer_device(cmd_buffer);

   assert(device->use_global_bo_list);

   VkTraceRaysIndirectCommand2KHR tables = {
      .raygenShaderRecordAddress = pRaygenShaderBindingTable->deviceAddress,
      .raygenShaderRecordSize = pRaygenShaderBindingTable->size,
      .missShaderBindingTableAddress = pMissShaderBindingTable->deviceAddress,
      .missShaderBindingTableSize = pMissShaderBindingTable->size,
      .missShaderBindingTableStride = pMissShaderBindingTable->stride,
      .hitShaderBindingTableAddress = pHitShaderBindingTable->deviceAddress,
      .hitShaderBindingTableSize = pHitShaderBindingTable->size,
      .hitShaderBindingTableStride = pHitShaderBindingTable->stride,
      .callableShaderBindingTableAddress = pCallableShaderBindingTable->deviceAddress,
      .callableShaderBindingTableSize = pCallableShaderBindingTable->size,
      .callableShaderBindingTableStride = pCallableShaderBindingTable->stride,
   };

   radv_trace_rays(cmd_buffer, &tables, indirectDeviceAddress, radv_rt_mode_indirect);
}

VKAPI_ATTR void VKAPI_CALL
radv_CmdTraceRaysIndirect2KHR(VkCommandBuffer commandBuffer, VkDeviceAddress indirectDeviceAddress)
{
   VK_FROM_HANDLE(radv_cmd_buffer, cmd_buffer, commandBuffer);
   struct radv_device *device = radv_cmd_buffer_device(cmd_buffer);

   assert(device->use_global_bo_list);

   radv_trace_rays(cmd_buffer, NULL, indirectDeviceAddress, radv_rt_mode_indirect2);
}

VKAPI_ATTR void VKAPI_CALL
radv_CmdSetRayTracingPipelineStackSizeKHR(VkCommandBuffer commandBuffer, uint32_t size)
{
   VK_FROM_HANDLE(radv_cmd_buffer, cmd_buffer, commandBuffer);
   cmd_buffer->state.rt_stack_size = size;
}

/*
 * For HTILE we have the following interesting clear words:
 *   0xfffff30f: Uncompressed, full depth range, for depth+stencil HTILE
 *   0xfffc000f: Uncompressed, full depth range, for depth only HTILE.
 *   0xfffffff0: Clear depth to 1.0
 *   0x00000000: Clear depth to 0.0
 */
static void
radv_initialize_htile(struct radv_cmd_buffer *cmd_buffer, struct radv_image *image,
                      const VkImageSubresourceRange *range)
{
   struct radv_device *device = radv_cmd_buffer_device(cmd_buffer);
   struct radv_cmd_state *state = &cmd_buffer->state;
   uint32_t htile_value = radv_get_htile_initial_value(device, image);
   VkClearDepthStencilValue value = {0};
   struct radv_barrier_data barrier = {0};

   barrier.layout_transitions.init_mask_ram = 1;
   radv_describe_layout_transition(cmd_buffer, &barrier);

   /* Transitioning from LAYOUT_UNDEFINED layout not everyone is consistent
    * in considering previous rendering work for WAW hazards. */
   state->flush_bits |= radv_src_access_flush(cmd_buffer, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                                              VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT, 0, image, range);

   if (image->planes[0].surface.has_stencil &&
       !(range->aspectMask == (VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT))) {
      /* Flush caches before performing a separate aspect initialization because it's a
       * read-modify-write operation.
       */
      state->flush_bits |= radv_dst_access_flush(cmd_buffer, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                                                 VK_ACCESS_2_SHADER_READ_BIT, 0, image, range);
   }

   state->flush_bits |= radv_clear_htile(cmd_buffer, image, range, htile_value, false);

   radv_set_ds_clear_metadata(cmd_buffer, image, range, value, range->aspectMask);

   if (radv_tc_compat_htile_enabled(image, range->baseMipLevel) && (range->aspectMask & VK_IMAGE_ASPECT_DEPTH_BIT)) {
      /* Initialize the TC-compat metada value to 0 because by
       * default DB_Z_INFO.RANGE_PRECISION is set to 1, and we only
       * need have to conditionally update its value when performing
       * a fast depth clear.
       */
      radv_set_tc_compat_zrange_metadata(cmd_buffer, image, range, 0);
   }
}

static void
radv_initialize_hiz(struct radv_cmd_buffer *cmd_buffer, struct radv_image *image, const VkImageSubresourceRange *range)
{
   struct radv_cmd_state *state = &cmd_buffer->state;
   struct radv_barrier_data barrier = {0};

   if (cmd_buffer->qf == RADV_QUEUE_TRANSFER)
      return;

   barrier.layout_transitions.init_mask_ram = 1;
   radv_describe_layout_transition(cmd_buffer, &barrier);

   /* Transitioning from LAYOUT_UNDEFINED layout not everyone is consistent
    * in considering previous rendering work for WAW hazards. */
   state->flush_bits |= radv_src_access_flush(cmd_buffer, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                                              VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT, 0, image, range);

   radv_clear_hiz(cmd_buffer, image, range, radv_gfx12_get_hiz_initial_value());

   /* Allow to enable HiZ for this range because all layers are handled in the barrier. */
   const bool enable_hiz =
      range->baseArrayLayer == 0 && vk_image_subresource_layer_count(&image->vk, range) == image->vk.array_layers;

   radv_update_hiz_metadata(cmd_buffer, image, range, enable_hiz);
}

static void
radv_handle_depth_image_transition(struct radv_cmd_buffer *cmd_buffer, struct radv_image *image,
                                   VkImageLayout src_layout, VkImageLayout dst_layout, unsigned src_queue_mask,
                                   unsigned dst_queue_mask, const VkImageSubresourceRange *range,
                                   struct radv_sample_locations_state *sample_locs)
{
   struct radv_device *device = radv_cmd_buffer_device(cmd_buffer);
   const struct radv_physical_device *pdev = radv_device_physical(device);

   if (pdev->info.gfx_level >= GFX12) {
      if (!image->hiz_valid_offset)
         return;

      if (src_layout == VK_IMAGE_LAYOUT_UNDEFINED || src_layout == VK_IMAGE_LAYOUT_ZERO_INITIALIZED_EXT) {
         radv_initialize_hiz(cmd_buffer, image, range);
      }
   } else {
      if (!radv_htile_enabled(image, range->baseMipLevel))
         return;

      if (src_layout == VK_IMAGE_LAYOUT_UNDEFINED || src_layout == VK_IMAGE_LAYOUT_ZERO_INITIALIZED_EXT) {
         radv_initialize_htile(cmd_buffer, image, range);
      } else if (radv_layout_is_htile_compressed(device, image, range->baseMipLevel, src_layout, src_queue_mask) &&
                 !radv_layout_is_htile_compressed(device, image, range->baseMipLevel, dst_layout, dst_queue_mask)) {
         cmd_buffer->state.flush_bits |= RADV_CMD_FLAG_FLUSH_AND_INV_DB | RADV_CMD_FLAG_FLUSH_AND_INV_DB_META;

         radv_expand_depth_stencil(cmd_buffer, image, range, sample_locs);

         cmd_buffer->state.flush_bits |= RADV_CMD_FLAG_FLUSH_AND_INV_DB | RADV_CMD_FLAG_FLUSH_AND_INV_DB_META;
      }
   }
}

static uint32_t
radv_init_cmask(struct radv_cmd_buffer *cmd_buffer, struct radv_image *image, const VkImageSubresourceRange *range,
                uint32_t value)
{
   struct radv_barrier_data barrier = {0};

   barrier.layout_transitions.init_mask_ram = 1;
   radv_describe_layout_transition(cmd_buffer, &barrier);

   return radv_clear_cmask(cmd_buffer, image, range, value);
}

uint32_t
radv_init_fmask(struct radv_cmd_buffer *cmd_buffer, struct radv_image *image, const VkImageSubresourceRange *range)
{
   static const uint32_t fmask_clear_values[4] = {0x00000000, 0x02020202, 0xE4E4E4E4, 0x76543210};
   uint32_t log2_samples = util_logbase2(image->vk.samples);
   uint32_t value = fmask_clear_values[log2_samples];
   struct radv_barrier_data barrier = {0};

   barrier.layout_transitions.init_mask_ram = 1;
   radv_describe_layout_transition(cmd_buffer, &barrier);

   return radv_clear_fmask(cmd_buffer, image, range, value);
}

uint32_t
radv_init_dcc(struct radv_cmd_buffer *cmd_buffer, struct radv_image *image, const VkImageSubresourceRange *range,
              uint32_t value)
{
   struct radv_device *device = radv_cmd_buffer_device(cmd_buffer);
   const struct radv_physical_device *pdev = radv_device_physical(device);
   struct radv_barrier_data barrier = {0};
   uint32_t flush_bits = 0;
   unsigned size = 0;

   barrier.layout_transitions.init_mask_ram = 1;
   radv_describe_layout_transition(cmd_buffer, &barrier);

   flush_bits |= radv_clear_dcc(cmd_buffer, image, range, value);

   if (pdev->info.gfx_level == GFX8) {
      /* When DCC is enabled with mipmaps, some levels might not
       * support fast clears and we have to initialize them as "fully
       * expanded".
       */
      /* Compute the size of all fast clearable DCC levels. */
      for (unsigned i = 0; i < image->planes[0].surface.num_meta_levels; i++) {
         struct legacy_surf_dcc_level *dcc_level = &image->planes[0].surface.u.legacy.color.dcc_level[i];
         unsigned dcc_fast_clear_size = dcc_level->dcc_slice_fast_clear_size * image->vk.array_layers;

         if (!dcc_fast_clear_size)
            break;

         size = dcc_level->dcc_offset + dcc_fast_clear_size;
      }

      /* Initialize the mipmap levels without DCC. */
      if (size != image->planes[0].surface.meta_size) {
         flush_bits |= radv_fill_image(cmd_buffer, image, image->planes[0].surface.meta_offset + size,
                                       image->planes[0].surface.meta_size - size, 0xffffffff);
      }
   }

   return flush_bits;
}

/**
 * Initialize DCC/FMASK/CMASK metadata for a color image.
 */
static void
radv_init_color_image_metadata(struct radv_cmd_buffer *cmd_buffer, struct radv_image *image, VkImageLayout src_layout,
                               VkImageLayout dst_layout, unsigned src_queue_mask, unsigned dst_queue_mask,
                               const VkImageSubresourceRange *range)
{
   struct radv_device *device = radv_cmd_buffer_device(cmd_buffer);
   uint32_t flush_bits = 0;

   /* Transitioning from LAYOUT_UNDEFINED layout not everyone is
    * consistent in considering previous rendering work for WAW hazards.
    */
   cmd_buffer->state.flush_bits |= radv_src_access_flush(cmd_buffer, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                                                         VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT, 0, image, range);

   if (radv_image_has_cmask(image)) {
      static const uint32_t cmask_clear_values[4] = {0xffffffff, 0xdddddddd, 0xeeeeeeee, 0xffffffff};
      uint32_t log2_samples = util_logbase2(image->vk.samples);

      flush_bits |= radv_init_cmask(cmd_buffer, image, range, cmask_clear_values[log2_samples]);
   }

   if (radv_image_has_fmask(image)) {
      flush_bits |= radv_init_fmask(cmd_buffer, image, range);
   }

   if (radv_dcc_enabled(image, range->baseMipLevel)) {
      uint32_t value = 0xffffffffu; /* Fully expanded mode. */

      if (radv_layout_dcc_compressed(device, image, range->baseMipLevel, dst_layout, dst_queue_mask)) {
         value = 0u;
      }

      flush_bits |= radv_init_dcc(cmd_buffer, image, range, value);
   }

   if (radv_image_has_cmask(image) || radv_dcc_enabled(image, range->baseMipLevel)) {
      radv_update_fce_metadata(cmd_buffer, image, range, false);

      uint32_t color_values[2] = {0};
      radv_set_color_clear_metadata(cmd_buffer, image, range, color_values);
   }

   cmd_buffer->state.flush_bits |= flush_bits;
}

static void
radv_retile_transition(struct radv_cmd_buffer *cmd_buffer, struct radv_image *image, VkImageLayout src_layout,
                       VkImageLayout dst_layout, unsigned dst_queue_mask)
{
   /* If the image is read-only, we don't have to retile DCC because it can't change. */
   if (!(image->vk.usage & RADV_IMAGE_USAGE_WRITE_BITS))
      return;

   if (src_layout != VK_IMAGE_LAYOUT_PRESENT_SRC_KHR &&
       (dst_layout == VK_IMAGE_LAYOUT_PRESENT_SRC_KHR || (dst_queue_mask & (1u << RADV_QUEUE_FOREIGN))))
      radv_retile_dcc(cmd_buffer, image);
}

static bool
radv_image_need_retile(const struct radv_cmd_buffer *cmd_buffer, const struct radv_image *image)
{
   return cmd_buffer->qf != RADV_QUEUE_TRANSFER && image->planes[0].surface.display_dcc_offset &&
          image->planes[0].surface.display_dcc_offset != image->planes[0].surface.meta_offset;
}

/**
 * Handle color image transitions for DCC/FMASK/CMASK.
 */
static void
radv_handle_color_image_transition(struct radv_cmd_buffer *cmd_buffer, struct radv_image *image,
                                   VkImageLayout src_layout, VkImageLayout dst_layout, unsigned src_queue_mask,
                                   unsigned dst_queue_mask, const VkImageSubresourceRange *range)
{
   struct radv_device *device = radv_cmd_buffer_device(cmd_buffer);
   bool needs_dcc_decompress = false, needs_dcc_retile = false;
   bool needs_fce = false, needs_fmask_decompress = false, needs_fmask_color_expand = false;

   if (!radv_image_has_cmask(image) && !radv_image_has_fmask(image) && !radv_dcc_enabled(image, range->baseMipLevel))
      return;

   if (src_layout == VK_IMAGE_LAYOUT_UNDEFINED || src_layout == VK_IMAGE_LAYOUT_ZERO_INITIALIZED_EXT) {
      radv_init_color_image_metadata(cmd_buffer, image, src_layout, dst_layout, src_queue_mask, dst_queue_mask, range);

      if (radv_image_need_retile(cmd_buffer, image))
         radv_retile_transition(cmd_buffer, image, src_layout, dst_layout, dst_queue_mask);
      return;
   }

   if (radv_dcc_enabled(image, range->baseMipLevel)) {
      if (src_layout == VK_IMAGE_LAYOUT_PREINITIALIZED) {
         cmd_buffer->state.flush_bits |= radv_init_dcc(cmd_buffer, image, range, 0xffffffffu);
      } else if (radv_layout_dcc_compressed(device, image, range->baseMipLevel, src_layout, src_queue_mask) &&
                 !radv_layout_dcc_compressed(device, image, range->baseMipLevel, dst_layout, dst_queue_mask)) {
         needs_dcc_decompress = true;
      }

      if (radv_image_need_retile(cmd_buffer, image))
         needs_dcc_retile = true;
   }

   if (radv_layout_can_fast_clear(device, image, range->baseMipLevel, src_layout, src_queue_mask) &&
       !radv_layout_can_fast_clear(device, image, range->baseMipLevel, dst_layout, dst_queue_mask)) {
      /* FCE is only required for color images that don't support comp-to-single fast clears. */
      if (!image->support_comp_to_single)
         needs_fce = true;

      /* FMASK_DECOMPRESS is only required for color images that don't support TC-compatible CMASK. */
      if (radv_image_has_fmask(image) && !image->tc_compatible_cmask)
         needs_fmask_decompress = true;
   }

   const enum radv_fmask_compression src_fmask_comp =
      radv_layout_fmask_compression(device, image, src_layout, src_queue_mask);
   const enum radv_fmask_compression dst_fmask_comp =
      radv_layout_fmask_compression(device, image, dst_layout, dst_queue_mask);

   if (src_fmask_comp > dst_fmask_comp) {
      if (src_fmask_comp == RADV_FMASK_COMPRESSION_FULL) {
         if (radv_dcc_enabled(image, range->baseMipLevel) && !radv_image_use_dcc_image_stores(device, image)) {
            /* A DCC decompress is required before expanding FMASK when DCC stores aren't supported to
             * avoid being in a state where DCC is compressed and the main surface is uncompressed.
             */
            needs_dcc_decompress = true;
         } else {
            /* FMASK_DECOMPRESS is always required before expanding FMASK. */
            needs_fmask_decompress = true;
         }
      }

      if (dst_fmask_comp == RADV_FMASK_COMPRESSION_NONE)
         needs_fmask_color_expand = true;
   }

   if (needs_dcc_decompress) {
      radv_decompress_dcc(cmd_buffer, image, range);
   } else if (needs_fmask_decompress) {
      /* MSAA images with DCC and CMASK might have been fast-cleared and might require a FCE but
       * FMASK_DECOMPRESS can't eliminate DCC fast clears. Only GFX10 is affected because it has few
       * restrictions related to comp-to-single.
       */
      const bool needs_dcc_fce =
         radv_image_has_dcc(image) && radv_image_has_cmask(image) && !image->support_comp_to_single;

      if (needs_dcc_fce)
         radv_fast_clear_eliminate(cmd_buffer, image, range);

      radv_fmask_decompress(cmd_buffer, image, range);
   } else if (needs_fce) {
      radv_fast_clear_eliminate(cmd_buffer, image, range);
   }

   if (needs_fmask_color_expand)
      radv_fmask_color_expand(cmd_buffer, image, range);

   if (needs_dcc_retile)
      radv_retile_transition(cmd_buffer, image, src_layout, dst_layout, dst_queue_mask);
}

static void
radv_handle_image_transition(struct radv_cmd_buffer *cmd_buffer, struct radv_image *image, VkImageLayout src_layout,
                             VkImageLayout dst_layout, uint32_t src_family_index, uint32_t dst_family_index,
                             const VkImageSubresourceRange *range, struct radv_sample_locations_state *sample_locs)
{
   struct radv_device *device = radv_cmd_buffer_device(cmd_buffer);
   const struct radv_physical_device *pdev = radv_device_physical(device);
   enum radv_queue_family src_qf = vk_queue_to_radv(pdev, src_family_index);
   enum radv_queue_family dst_qf = vk_queue_to_radv(pdev, dst_family_index);
   if (image->exclusive && src_family_index != dst_family_index) {
      /* This is an acquire or a release operation and there will be
       * a corresponding release/acquire. Do the transition in the
       * most flexible queue. */

      assert(src_qf == cmd_buffer->qf || dst_qf == cmd_buffer->qf);

      if (src_family_index == VK_QUEUE_FAMILY_EXTERNAL || src_family_index == VK_QUEUE_FAMILY_FOREIGN_EXT)
         return;

      if (cmd_buffer->qf == RADV_QUEUE_TRANSFER)
         return;

      if (cmd_buffer->qf == RADV_QUEUE_COMPUTE && (src_qf == RADV_QUEUE_GENERAL || dst_qf == RADV_QUEUE_GENERAL))
         return;
   }

   unsigned src_queue_mask = radv_image_queue_family_mask(image, src_qf, cmd_buffer->qf);
   unsigned dst_queue_mask = radv_image_queue_family_mask(image, dst_qf, cmd_buffer->qf);

   if (src_layout == dst_layout && src_queue_mask == dst_queue_mask)
      return;

   if (image->vk.aspects & VK_IMAGE_ASPECT_DEPTH_BIT) {
      radv_handle_depth_image_transition(cmd_buffer, image, src_layout, dst_layout, src_queue_mask, dst_queue_mask,
                                         range, sample_locs);
   } else {
      radv_handle_color_image_transition(cmd_buffer, image, src_layout, dst_layout, src_queue_mask, dst_queue_mask,
                                         range);
   }
}

static void
radv_cp_dma_wait_for_stages(struct radv_cmd_buffer *cmd_buffer, VkPipelineStageFlags2 stage_mask)
{
   /* Make sure CP DMA is idle because the driver might have performed a DMA operation for copying a
    * buffer (or a MSAA image using FMASK). Note that updating a buffer is considered a clear
    * operation but it might also use a CP DMA copy in some rare situations. Other operations using
    * a CP DMA clear are implicitly synchronized (see CP_DMA_SYNC).
    */
   if (stage_mask &
       (VK_PIPELINE_STAGE_2_COPY_BIT | VK_PIPELINE_STAGE_2_CLEAR_BIT | VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT |
        VK_PIPELINE_STAGE_2_BOTTOM_OF_PIPE_BIT | VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT))
      radv_cp_dma_wait_for_idle(cmd_buffer);
}

void
radv_emit_cache_flush(struct radv_cmd_buffer *cmd_buffer)
{
   struct radv_device *device = radv_cmd_buffer_device(cmd_buffer);
   const struct radv_physical_device *pdev = radv_device_physical(device);
   bool is_compute = cmd_buffer->qf == RADV_QUEUE_COMPUTE;
   struct radv_cmd_stream *cs = cmd_buffer->cs;

   if (is_compute)
      cmd_buffer->state.flush_bits &=
         ~(RADV_CMD_FLAG_FLUSH_AND_INV_CB | RADV_CMD_FLAG_FLUSH_AND_INV_CB_META | RADV_CMD_FLAG_FLUSH_AND_INV_DB |
           RADV_CMD_FLAG_FLUSH_AND_INV_DB_META | RADV_CMD_FLAG_INV_L2_METADATA | RADV_CMD_FLAG_PS_PARTIAL_FLUSH |
           RADV_CMD_FLAG_VS_PARTIAL_FLUSH | RADV_CMD_FLAG_VGT_FLUSH | RADV_CMD_FLAG_START_PIPELINE_STATS |
           RADV_CMD_FLAG_STOP_PIPELINE_STATS);

   if (!cmd_buffer->state.flush_bits) {
      radv_describe_barrier_end_delayed(cmd_buffer);
      return;
   }

   radv_cs_emit_cache_flush(device->ws, cs, pdev->info.gfx_level, &cmd_buffer->gfx9_fence_idx,
                            cmd_buffer->gfx9_fence_va, cmd_buffer->state.flush_bits, &cmd_buffer->state.sqtt_flush_bits,
                            cmd_buffer->gfx9_eop_bug_va);

   if (radv_device_fault_detection_enabled(device))
      radv_cmd_buffer_trace_emit(cmd_buffer);

   if (cmd_buffer->state.flush_bits & RADV_CMD_FLAG_INV_L2)
      cmd_buffer->state.rb_noncoherent_dirty = false;

   /* Clear the caches that have been flushed to avoid syncing too much
    * when there is some pending active queries.
    */
   cmd_buffer->active_query_flush_bits &= ~cmd_buffer->state.flush_bits;

   cmd_buffer->state.flush_bits = 0;

   /* If the driver used a compute shader for resetting a query pool, it
    * should be finished at this point.
    */
   cmd_buffer->pending_reset_query = false;

   radv_describe_barrier_end_delayed(cmd_buffer);
}

static enum radv_cmd_flush_bits
radv_get_src_access_flush(struct radv_cmd_buffer *cmd_buffer, VkPipelineStageFlags2 src_stage_mask,
                          VkAccessFlags2 src_access_mask, const struct radv_image *image,
                          const VkImageSubresourceRange *range, const void *pNext)
{
   const VkMemoryBarrierAccessFlags3KHR *barrier3 = vk_find_struct_const(pNext, MEMORY_BARRIER_ACCESS_FLAGS_3_KHR);
   const VkAccessFlags3KHR src3_flags = barrier3 ? barrier3->srcAccessMask3 : 0;

   return radv_src_access_flush(cmd_buffer, src_stage_mask, src_access_mask, src3_flags, image, range);
}

static enum radv_cmd_flush_bits
radv_get_dst_access_flush(struct radv_cmd_buffer *cmd_buffer, VkPipelineStageFlags2 dst_stage_mask,
                          VkAccessFlags2 dst_access_mask, const struct radv_image *image,
                          const VkImageSubresourceRange *range, const void *pNext)
{
   const VkMemoryBarrierAccessFlags3KHR *barrier3 = vk_find_struct_const(pNext, MEMORY_BARRIER_ACCESS_FLAGS_3_KHR);
   const VkAccessFlags3KHR dst3_flags = barrier3 ? barrier3->dstAccessMask3 : 0;

   return radv_dst_access_flush(cmd_buffer, dst_stage_mask, dst_access_mask, dst3_flags, image, range);
}

static void
radv_barrier(struct radv_cmd_buffer *cmd_buffer, uint32_t dep_count, const VkDependencyInfo *dep_infos,
             enum rgp_barrier_reason reason)
{
   struct radv_device *device = radv_cmd_buffer_device(cmd_buffer);
   struct radv_cmd_stream *cs = cmd_buffer->cs;
   enum radv_cmd_flush_bits src_flush_bits = 0;
   enum radv_cmd_flush_bits dst_flush_bits = 0;
   VkPipelineStageFlags2 src_stage_mask = 0;
   VkPipelineStageFlags2 dst_stage_mask = 0;
   bool has_image_transitions = false;

   if (cmd_buffer->state.render.active)
      radv_mark_noncoherent_rb(cmd_buffer);

   radv_describe_barrier_start(cmd_buffer, reason);

   for (uint32_t dep_idx = 0; dep_idx < dep_count; dep_idx++) {
      const VkDependencyInfo *dep_info = &dep_infos[dep_idx];

      for (uint32_t i = 0; i < dep_info->memoryBarrierCount; i++) {
         const VkMemoryBarrier2 *barrier = &dep_info->pMemoryBarriers[i];
         src_stage_mask |= barrier->srcStageMask;
         src_flush_bits |= radv_get_src_access_flush(cmd_buffer, barrier->srcStageMask, barrier->srcAccessMask, NULL,
                                                     NULL, barrier->pNext);
         dst_stage_mask |= barrier->dstStageMask;
         dst_flush_bits |= radv_get_dst_access_flush(cmd_buffer, barrier->dstStageMask, barrier->dstAccessMask, NULL,
                                                     NULL, barrier->pNext);
      }

      for (uint32_t i = 0; i < dep_info->bufferMemoryBarrierCount; i++) {
         const VkBufferMemoryBarrier2 *barrier = &dep_info->pBufferMemoryBarriers[i];
         src_stage_mask |= barrier->srcStageMask;
         src_flush_bits |= radv_get_src_access_flush(cmd_buffer, barrier->srcStageMask, barrier->srcAccessMask, NULL,
                                                     NULL, barrier->pNext);
         dst_stage_mask |= barrier->dstStageMask;
         dst_flush_bits |= radv_get_dst_access_flush(cmd_buffer, barrier->dstStageMask, barrier->dstAccessMask, NULL,
                                                     NULL, barrier->pNext);
      }

      for (uint32_t i = 0; i < dep_info->imageMemoryBarrierCount; i++) {
         const VkImageMemoryBarrier2 *barrier = &dep_info->pImageMemoryBarriers[i];
         VK_FROM_HANDLE(radv_image, image, barrier->image);

         src_stage_mask |= barrier->srcStageMask;
         src_flush_bits |= radv_get_src_access_flush(cmd_buffer, barrier->srcStageMask, barrier->srcAccessMask, image,
                                                     &barrier->subresourceRange, barrier->pNext);
         dst_stage_mask |= barrier->dstStageMask;
         dst_flush_bits |= radv_get_dst_access_flush(cmd_buffer, barrier->dstStageMask, barrier->dstAccessMask, image,
                                                     &barrier->subresourceRange, barrier->pNext);
      }

      has_image_transitions |= dep_info->imageMemoryBarrierCount > 0;
   }

   /* Only optimize BOTTOM_OF_PIPE/NONE as dst when there is no image layout transitions because it might
    * need to synchronize.
    */
   if (has_image_transitions ||
       (dst_stage_mask != VK_PIPELINE_STAGE_2_BOTTOM_OF_PIPE_BIT && dst_stage_mask != VK_PIPELINE_STAGE_2_NONE))
      radv_stage_flush(cmd_buffer, src_stage_mask);
   cmd_buffer->state.flush_bits |= src_flush_bits;

   radv_gang_barrier(cmd_buffer, src_stage_mask, 0);

   for (uint32_t dep_idx = 0; dep_idx < dep_count; dep_idx++) {
      const VkDependencyInfo *dep_info = &dep_infos[dep_idx];

      for (uint32_t i = 0; i < dep_info->imageMemoryBarrierCount; i++) {
         VK_FROM_HANDLE(radv_image, image, dep_info->pImageMemoryBarriers[i].image);

         const struct VkSampleLocationsInfoEXT *sample_locs_info =
            vk_find_struct_const(dep_info->pImageMemoryBarriers[i].pNext, SAMPLE_LOCATIONS_INFO_EXT);
         struct radv_sample_locations_state sample_locations;

         if (sample_locs_info) {
            assert(image->vk.create_flags & VK_IMAGE_CREATE_SAMPLE_LOCATIONS_COMPATIBLE_DEPTH_BIT_EXT);
            sample_locations.per_pixel = sample_locs_info->sampleLocationsPerPixel;
            sample_locations.grid_size = sample_locs_info->sampleLocationGridSize;
            sample_locations.count = sample_locs_info->sampleLocationsCount;
            typed_memcpy(&sample_locations.locations[0], sample_locs_info->pSampleLocations,
                         sample_locs_info->sampleLocationsCount);
         }

         uint32_t src_qf_index = dep_info->pImageMemoryBarriers[i].srcQueueFamilyIndex;
         uint32_t dst_qf_index = dep_info->pImageMemoryBarriers[i].dstQueueFamilyIndex;

         /* The src and dst queue family indices may contrain arbitrary values
          * that should be ignored if they are equal. For example, see
          * VUID-VkBufferMemoryBarrier-buffer-09095 (Vulkan spec 1.4.313).
          *
          *   If buffer was created with a sharing mode of
          *   VK_SHARING_MODE_EXCLUSIVE, and srcQueueFamilyIndex and
          *   dstQueueFamilyIndex are not equal, srcQueueFamilyIndex must be
          *   VK_QUEUE_FAMILY_EXTERNAL, VK_QUEUE_FAMILY_FOREIGN_EXT, or a valid
          *   queue family
          */
         if (src_qf_index == dst_qf_index) {
            src_qf_index = VK_QUEUE_FAMILY_IGNORED;
            dst_qf_index = VK_QUEUE_FAMILY_IGNORED;
         }

         radv_handle_image_transition(cmd_buffer, image, dep_info->pImageMemoryBarriers[i].oldLayout,
                                      dep_info->pImageMemoryBarriers[i].newLayout, src_qf_index, dst_qf_index,
                                      &dep_info->pImageMemoryBarriers[i].subresourceRange,
                                      sample_locs_info ? &sample_locations : NULL);
      }
   }

   radv_gang_barrier(cmd_buffer, 0, dst_stage_mask);

   if (cmd_buffer->qf == RADV_QUEUE_TRANSFER) {
      /* SDMA NOP packet waits for all pending SDMA operations to complete.
       * Note that GFX9+ is supposed to have RAW dependency tracking, but it's buggy
       * so we can't rely on it fow now.
       */
      radv_sdma_emit_nop(device, cs);
   } else {
      const bool is_gfx_or_ace = cmd_buffer->qf == RADV_QUEUE_GENERAL || cmd_buffer->qf == RADV_QUEUE_COMPUTE;
      if (is_gfx_or_ace)
         radv_cp_dma_wait_for_stages(cmd_buffer, src_stage_mask);
   }

   cmd_buffer->state.flush_bits |= dst_flush_bits;

   radv_describe_barrier_end(cmd_buffer);
}

VKAPI_ATTR void VKAPI_CALL
radv_CmdPipelineBarrier2(VkCommandBuffer commandBuffer, const VkDependencyInfo *pDependencyInfo)
{
   VK_FROM_HANDLE(radv_cmd_buffer, cmd_buffer, commandBuffer);
   enum rgp_barrier_reason barrier_reason;

   if (cmd_buffer->vk.runtime_rp_barrier) {
      barrier_reason = RGP_BARRIER_EXTERNAL_RENDER_PASS_SYNC;
   } else {
      barrier_reason = RGP_BARRIER_EXTERNAL_CMD_PIPELINE_BARRIER;
   }

   radv_barrier(cmd_buffer, 1, pDependencyInfo, barrier_reason);
}

static void
write_event(struct radv_cmd_buffer *cmd_buffer, struct radv_event *event, VkPipelineStageFlags2 stageMask,
            unsigned value)
{
   struct radv_device *device = radv_cmd_buffer_device(cmd_buffer);
   const struct radv_physical_device *pdev = radv_device_physical(device);
   struct radv_cmd_stream *cs = cmd_buffer->cs;
   uint64_t va = radv_buffer_get_va(event->bo);

   radv_cs_add_buffer(device->ws, cs->b, event->bo);

   if (cmd_buffer->qf == RADV_QUEUE_VIDEO_DEC || cmd_buffer->qf == RADV_QUEUE_VIDEO_ENC) {
      radv_vcn_write_memory(cmd_buffer, va, value);
      return;
   }

   radv_emit_cache_flush(cmd_buffer);

   ASSERTED unsigned cdw_max = radeon_check_space(device->ws, cs->b, 28);

   if (stageMask & (VK_PIPELINE_STAGE_2_COPY_BIT | VK_PIPELINE_STAGE_2_RESOLVE_BIT | VK_PIPELINE_STAGE_2_BLIT_BIT |
                    VK_PIPELINE_STAGE_2_CLEAR_BIT)) {
      /* Be conservative for now. */
      stageMask |= VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT;
   }

   /* Flags that only require a top-of-pipe event. */
   VkPipelineStageFlags2 top_of_pipe_flags = VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT;

   /* Flags that only require a post-index-fetch event. */
   VkPipelineStageFlags2 post_index_fetch_flags =
      top_of_pipe_flags | VK_PIPELINE_STAGE_2_DRAW_INDIRECT_BIT | VK_PIPELINE_STAGE_2_VERTEX_INPUT_BIT;

   /* Flags that only require signaling post PS. */
   VkPipelineStageFlags2 post_ps_flags =
      post_index_fetch_flags | VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT |
      VK_PIPELINE_STAGE_2_TESSELLATION_CONTROL_SHADER_BIT | VK_PIPELINE_STAGE_2_TESSELLATION_EVALUATION_SHADER_BIT |
      VK_PIPELINE_STAGE_2_GEOMETRY_SHADER_BIT | VK_PIPELINE_STAGE_2_MESH_SHADER_BIT_EXT |
      VK_PIPELINE_STAGE_2_TRANSFORM_FEEDBACK_BIT_EXT | VK_PIPELINE_STAGE_2_PRE_RASTERIZATION_SHADERS_BIT |
      VK_PIPELINE_STAGE_2_FRAGMENT_SHADING_RATE_ATTACHMENT_BIT_KHR | VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT |
      VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;

   /* Flags that only require signaling post CS. */
   VkPipelineStageFlags2 post_cs_flags = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;

   radv_cp_dma_wait_for_stages(cmd_buffer, stageMask);

   if (!(stageMask & ~top_of_pipe_flags) && cmd_buffer->qf != RADV_QUEUE_COMPUTE) {
      /* Just need to sync the PFP engine. */
      radv_write_data(cmd_buffer, V_370_PFP, va, 1, &value, false);
   } else if (!(stageMask & ~post_index_fetch_flags)) {
      /* Sync ME because PFP reads index and indirect buffers. */
      radv_write_data(cmd_buffer, V_370_ME, va, 1, &value, false);
   } else {
      unsigned event_type;

      if (!(stageMask & ~post_ps_flags)) {
         /* Sync previous fragment shaders. */
         event_type = V_028A90_PS_DONE;
         assert(cmd_buffer->qf == RADV_QUEUE_GENERAL);
      } else if (!(stageMask & ~post_cs_flags)) {
         /* Sync previous compute shaders. */
         event_type = V_028A90_CS_DONE;
      } else {
         /* Otherwise, sync all prior GPU work. */
         event_type = V_028A90_BOTTOM_OF_PIPE_TS;
      }

      radv_cs_emit_write_event_eop(cs, pdev->info.gfx_level, event_type, 0, EOP_DST_SEL_MEM, EOP_DATA_SEL_VALUE_32BIT,
                                   va, value, cmd_buffer->gfx9_eop_bug_va);
   }

   assert(cs->b->cdw <= cdw_max);
}

VKAPI_ATTR void VKAPI_CALL
radv_CmdSetEvent2(VkCommandBuffer commandBuffer, VkEvent _event, const VkDependencyInfo *pDependencyInfo)
{
   VK_FROM_HANDLE(radv_cmd_buffer, cmd_buffer, commandBuffer);
   VK_FROM_HANDLE(radv_event, event, _event);
   VkPipelineStageFlags2 src_stage_mask = 0;

   for (uint32_t i = 0; i < pDependencyInfo->memoryBarrierCount; i++)
      src_stage_mask |= pDependencyInfo->pMemoryBarriers[i].srcStageMask;
   for (uint32_t i = 0; i < pDependencyInfo->bufferMemoryBarrierCount; i++)
      src_stage_mask |= pDependencyInfo->pBufferMemoryBarriers[i].srcStageMask;
   for (uint32_t i = 0; i < pDependencyInfo->imageMemoryBarrierCount; i++)
      src_stage_mask |= pDependencyInfo->pImageMemoryBarriers[i].srcStageMask;

   write_event(cmd_buffer, event, src_stage_mask, 1);
}

VKAPI_ATTR void VKAPI_CALL
radv_CmdResetEvent2(VkCommandBuffer commandBuffer, VkEvent _event, VkPipelineStageFlags2 stageMask)
{
   VK_FROM_HANDLE(radv_cmd_buffer, cmd_buffer, commandBuffer);
   VK_FROM_HANDLE(radv_event, event, _event);

   write_event(cmd_buffer, event, stageMask, 0);
}

VKAPI_ATTR void VKAPI_CALL
radv_CmdWaitEvents2(VkCommandBuffer commandBuffer, uint32_t eventCount, const VkEvent *pEvents,
                    const VkDependencyInfo *pDependencyInfos)
{
   VK_FROM_HANDLE(radv_cmd_buffer, cmd_buffer, commandBuffer);
   struct radv_device *device = radv_cmd_buffer_device(cmd_buffer);
   struct radv_cmd_stream *cs = cmd_buffer->cs;

   if (cmd_buffer->qf == RADV_QUEUE_VIDEO_DEC || cmd_buffer->qf == RADV_QUEUE_VIDEO_ENC)
      return;

   for (unsigned i = 0; i < eventCount; ++i) {
      VK_FROM_HANDLE(radv_event, event, pEvents[i]);
      uint64_t va = radv_buffer_get_va(event->bo);

      radv_cs_add_buffer(device->ws, cs->b, event->bo);

      ASSERTED unsigned cdw_max = radeon_check_space(device->ws, cs->b, 7);

      radv_cp_wait_mem(cs, WAIT_REG_MEM_EQUAL, va, 1, 0xffffffff);
      assert(cs->b->cdw <= cdw_max);
   }

   radv_barrier(cmd_buffer, eventCount, pDependencyInfos, RGP_BARRIER_EXTERNAL_CMD_WAIT_EVENTS);
}

void
radv_emit_set_predication_state(struct radv_cmd_buffer *cmd_buffer, bool draw_visible, unsigned pred_op, uint64_t va)
{
   struct radv_device *device = radv_cmd_buffer_device(cmd_buffer);
   const struct radv_physical_device *pdev = radv_device_physical(device);
   struct radv_cmd_stream *cs = cmd_buffer->cs;
   uint32_t op = 0;

   radeon_check_space(device->ws, cs->b, 4);

   if (va) {
      assert(pred_op == PREDICATION_OP_BOOL32 || pred_op == PREDICATION_OP_BOOL64);

      op = PRED_OP(pred_op);

      /* PREDICATION_DRAW_VISIBLE means that if the 32-bit value is
       * zero, all rendering commands are discarded. Otherwise, they
       * are discarded if the value is non zero.
       */
      op |= draw_visible ? PREDICATION_DRAW_VISIBLE : PREDICATION_DRAW_NOT_VISIBLE;
   }

   radeon_begin(cs);
   if (pdev->info.gfx_level >= GFX9) {
      radeon_emit(PKT3(PKT3_SET_PREDICATION, 2, 0));
      radeon_emit(op);
      radeon_emit(va);
      radeon_emit(va >> 32);
   } else {
      radeon_emit(PKT3(PKT3_SET_PREDICATION, 1, 0));
      radeon_emit(va);
      radeon_emit(op | ((va >> 32) & 0xFF));
   }
   radeon_end();
}

void
radv_begin_conditional_rendering(struct radv_cmd_buffer *cmd_buffer, uint64_t va, bool draw_visible)
{
   struct radv_device *device = radv_cmd_buffer_device(cmd_buffer);
   const struct radv_physical_device *pdev = radv_device_physical(device);
   struct radv_cmd_stream *cs = cmd_buffer->cs;
   unsigned pred_op = PREDICATION_OP_BOOL32;
   uint64_t emulated_va = 0;

   radv_emit_cache_flush(cmd_buffer);

   if (cmd_buffer->qf == RADV_QUEUE_GENERAL) {
      if (pdev->info.has_32bit_predication) {
         radv_emit_set_predication_state(cmd_buffer, draw_visible, pred_op, va);
      } else {
         uint64_t pred_value = 0;
         unsigned pred_offset;

         /* From the Vulkan spec 1.1.107:
          *
          * "If the 32-bit value at offset in buffer memory is zero,
          *  then the rendering commands are discarded, otherwise they
          *  are executed as normal. If the value of the predicate in
          *  buffer memory changes while conditional rendering is
          *  active, the rendering commands may be discarded in an
          *  implementation-dependent way. Some implementations may
          *  latch the value of the predicate upon beginning conditional
          *  rendering while others may read it before every rendering
          *  command."
          *
          * But, the AMD hardware treats the predicate as a 64-bit
          * value which means we need a workaround in the driver.
          * Luckily, it's not required to support if the value changes
          * when predication is active.
          *
          * The workaround is as follows:
          * 1) allocate a 64-value in the upload BO and initialize it
          *    to 0
          * 2) copy the 32-bit predicate value to the upload BO
          * 3) use the new allocated VA address for predication
          *
          * Based on the conditionalrender demo, it's faster to do the
          * COPY_DATA in ME  (+ sync PFP) instead of PFP.
          */
         if (!radv_cmd_buffer_upload_data(cmd_buffer, 8, &pred_value, &pred_offset)) {
            vk_command_buffer_set_error(&cmd_buffer->vk, VK_ERROR_OUT_OF_HOST_MEMORY);
            return;
         }

         emulated_va = radv_buffer_get_va(cmd_buffer->upload.upload_bo) + pred_offset;

         radeon_check_space(device->ws, cs->b, 8);

         ac_emit_cp_copy_data(cs->b, COPY_DATA_SRC_MEM, COPY_DATA_DST_MEM, va, emulated_va, AC_CP_COPY_DATA_WR_CONFIRM);

         ac_emit_cp_pfp_sync_me(cs->b);

         pred_op = PREDICATION_OP_BOOL64;

         radv_emit_set_predication_state(cmd_buffer, draw_visible, pred_op, emulated_va);
      }
   } else {
      /* Compute queue doesn't support predication and it's emulated elsewhere. */
   }

   /* Store conditional rendering user info. */
   cmd_buffer->state.predicating = true;
   cmd_buffer->state.predication_type = draw_visible;
   cmd_buffer->state.predication_op = pred_op;
   cmd_buffer->state.user_predication_va = va;
   cmd_buffer->state.emulated_predication_va = emulated_va;
   cmd_buffer->state.mec_inv_pred_emitted = false;
}

void
radv_end_conditional_rendering(struct radv_cmd_buffer *cmd_buffer)
{
   if (cmd_buffer->qf == RADV_QUEUE_GENERAL) {
      radv_emit_set_predication_state(cmd_buffer, false, 0, 0);
   } else {
      /* Compute queue doesn't support predication, no need to emit anything here. */
   }

   /* Reset conditional rendering user info. */
   cmd_buffer->state.predicating = false;
   cmd_buffer->state.predication_type = -1;
   cmd_buffer->state.predication_op = 0;
   cmd_buffer->state.user_predication_va = 0;
   cmd_buffer->state.emulated_predication_va = 0;
   cmd_buffer->state.mec_inv_pred_emitted = false;
}

/* VK_EXT_conditional_rendering */
VKAPI_ATTR void VKAPI_CALL
radv_CmdBeginConditionalRenderingEXT(VkCommandBuffer commandBuffer,
                                     const VkConditionalRenderingBeginInfoEXT *pConditionalRenderingBegin)
{
   VK_FROM_HANDLE(radv_cmd_buffer, cmd_buffer, commandBuffer);
   VK_FROM_HANDLE(radv_buffer, buffer, pConditionalRenderingBegin->buffer);
   struct radv_device *device = radv_cmd_buffer_device(cmd_buffer);
   struct radv_cmd_stream *cs = cmd_buffer->cs;
   bool draw_visible = true;
   uint64_t va;

   va = vk_buffer_address(&buffer->vk, pConditionalRenderingBegin->offset);

   radv_cs_add_buffer(device->ws, cs->b, buffer->bo);

   /* By default, if the 32-bit value at offset in buffer memory is zero,
    * then the rendering commands are discarded, otherwise they are
    * executed as normal. If the inverted flag is set, all commands are
    * discarded if the value is non zero.
    */
   if (pConditionalRenderingBegin->flags & VK_CONDITIONAL_RENDERING_INVERTED_BIT_EXT) {
      draw_visible = false;
   }

   radv_begin_conditional_rendering(cmd_buffer, va, draw_visible);
}

VKAPI_ATTR void VKAPI_CALL
radv_CmdEndConditionalRenderingEXT(VkCommandBuffer commandBuffer)
{
   VK_FROM_HANDLE(radv_cmd_buffer, cmd_buffer, commandBuffer);

   radv_end_conditional_rendering(cmd_buffer);
}

/* VK_EXT_transform_feedback */
VKAPI_ATTR void VKAPI_CALL
radv_CmdBindTransformFeedbackBuffersEXT(VkCommandBuffer commandBuffer, uint32_t firstBinding, uint32_t bindingCount,
                                        const VkBuffer *pBuffers, const VkDeviceSize *pOffsets,
                                        const VkDeviceSize *pSizes)
{
   VK_FROM_HANDLE(radv_cmd_buffer, cmd_buffer, commandBuffer);
   struct radv_device *device = radv_cmd_buffer_device(cmd_buffer);
   struct radv_streamout_binding *sb = cmd_buffer->streamout_bindings;
   struct radv_cmd_stream *cs = cmd_buffer->cs;
   uint8_t enabled_mask = 0;

   assert(firstBinding + bindingCount <= MAX_SO_BUFFERS);
   for (uint32_t i = 0; i < bindingCount; i++) {
      VK_FROM_HANDLE(radv_buffer, buffer, pBuffers[i]);
      uint32_t idx = firstBinding + i;

      sb[idx].va = vk_buffer_address(&buffer->vk, pOffsets[i]);

      if (!pSizes || pSizes[i] == VK_WHOLE_SIZE) {
         sb[idx].size = buffer->vk.size - pOffsets[i];
      } else {
         sb[idx].size = pSizes[i];
      }

      radv_cs_add_buffer(device->ws, cs->b, buffer->bo);

      enabled_mask |= 1 << idx;
   }

   cmd_buffer->state.streamout.enabled_mask |= enabled_mask;

   cmd_buffer->state.dirty |= RADV_CMD_DIRTY_STREAMOUT_BUFFER;
}

static void
radv_set_streamout_enable(struct radv_cmd_buffer *cmd_buffer, bool enable)
{
   struct radv_device *device = radv_cmd_buffer_device(cmd_buffer);
   const struct radv_physical_device *pdev = radv_device_physical(device);
   struct radv_streamout_state *so = &cmd_buffer->state.streamout;
   bool old_streamout_enabled = radv_is_streamout_enabled(cmd_buffer);
   uint32_t old_hw_enabled_mask = so->hw_enabled_mask;

   so->streamout_enabled = enable;

   so->hw_enabled_mask =
      so->enabled_mask | (so->enabled_mask << 4) | (so->enabled_mask << 8) | (so->enabled_mask << 12);

   if (!pdev->use_ngg_streamout && ((old_streamout_enabled != radv_is_streamout_enabled(cmd_buffer)) ||
                                    (old_hw_enabled_mask != so->hw_enabled_mask)))
      cmd_buffer->state.dirty |= RADV_CMD_DIRTY_STREAMOUT_ENABLE;

   if (pdev->use_ngg_streamout) {
      /* Re-emit streamout desciptors because with NGG streamout, a buffer size of 0 acts like a
       * disable bit and this is needed when streamout needs to be ignored in shaders.
       */
      cmd_buffer->state.dirty |= RADV_CMD_DIRTY_SHADER_QUERY | RADV_CMD_DIRTY_STREAMOUT_BUFFER;
   }
}

static void
radv_flush_vgt_streamout(struct radv_cmd_buffer *cmd_buffer)
{
   struct radv_device *device = radv_cmd_buffer_device(cmd_buffer);
   const struct radv_physical_device *pdev = radv_device_physical(device);
   struct radv_cmd_stream *cs = cmd_buffer->cs;
   unsigned reg_strmout_cntl;

   ASSERTED unsigned cdw_max = radeon_check_space(device->ws, cs->b, 14);

   radeon_begin(cs);

   /* The register is at different places on different ASICs. */
   if (pdev->info.gfx_level >= GFX9) {
      reg_strmout_cntl = R_0300FC_CP_STRMOUT_CNTL;
      radeon_emit(PKT3(PKT3_WRITE_DATA, 3, 0));
      radeon_emit(S_370_DST_SEL(V_370_MEM_MAPPED_REGISTER) | S_370_ENGINE_SEL(V_370_ME));
      radeon_emit(R_0300FC_CP_STRMOUT_CNTL >> 2);
      radeon_emit(0);
      radeon_emit(0);
   } else if (pdev->info.gfx_level >= GFX7) {
      reg_strmout_cntl = R_0300FC_CP_STRMOUT_CNTL;
      radeon_set_uconfig_reg(reg_strmout_cntl, 0);
   } else {
      reg_strmout_cntl = R_0084FC_CP_STRMOUT_CNTL;
      radeon_set_config_reg(reg_strmout_cntl, 0);
   }

   radeon_event_write(V_028A90_SO_VGTSTREAMOUT_FLUSH);

   radeon_emit(PKT3(PKT3_WAIT_REG_MEM, 5, 0));
   radeon_emit(WAIT_REG_MEM_EQUAL);    /* wait until the register is equal to the reference value */
   radeon_emit(reg_strmout_cntl >> 2); /* register */
   radeon_emit(0);
   radeon_emit(S_0084FC_OFFSET_UPDATE_DONE(1)); /* reference value */
   radeon_emit(S_0084FC_OFFSET_UPDATE_DONE(1)); /* mask */
   radeon_emit(4);                              /* poll interval */

   radeon_end();
   assert(cs->b->cdw <= cdw_max);
}

static void
radv_init_streamout_state(struct radv_cmd_buffer *cmd_buffer)
{
   const struct radv_device *device = radv_cmd_buffer_device(cmd_buffer);
   const struct radv_physical_device *pdev = radv_device_physical(device);
   struct radv_streamout_state *so = &cmd_buffer->state.streamout;
   unsigned offset;
   void *ptr;

   assert(pdev->info.gfx_level >= GFX12);

   /* The layout is:
    *    struct {
    *       struct {
    *          uint32_t ordered_id; // equal for all buffers
    *          uint32_t dwords_written;
    *       } buffer[4];
    *    };
    *
    * The buffer must be initialized to 0 and the address must be aligned to 64
    * because it's faster when the atomic doesn't straddle a 64B block boundary.
    */
   if (!radv_cmd_buffer_upload_alloc_aligned(cmd_buffer, MAX_SO_BUFFERS * 8, 64, &offset, &ptr))
      return;

   memset(ptr, 0, MAX_SO_BUFFERS * 8);

   so->state_va = radv_buffer_get_va(cmd_buffer->upload.upload_bo);
   so->state_va += offset;

   /* GE must be idle when GE_GS_ORDERED_ID is written. */
   cmd_buffer->state.flush_bits |= RADV_CMD_FLAG_VS_PARTIAL_FLUSH;
   radv_emit_cache_flush(cmd_buffer);

   /* Reset the ordered ID for the next GS workgroup to 0 because it must be
    * equal to the 4 ordered IDs in the layout.
    */
   radeon_begin(cmd_buffer->cs);
   radeon_set_uconfig_reg(R_0309B0_GE_GS_ORDERED_ID_BASE, 0);
   radeon_end();
}

VKAPI_ATTR void VKAPI_CALL
radv_CmdBeginTransformFeedbackEXT(VkCommandBuffer commandBuffer, uint32_t firstCounterBuffer,
                                  uint32_t counterBufferCount, const VkBuffer *pCounterBuffers,
                                  const VkDeviceSize *pCounterBufferOffsets)
{
   VK_FROM_HANDLE(radv_cmd_buffer, cmd_buffer, commandBuffer);
   struct radv_device *device = radv_cmd_buffer_device(cmd_buffer);
   const struct radv_physical_device *pdev = radv_device_physical(device);
   const enum amd_ip_type ring = radv_queue_family_to_ring(pdev, cmd_buffer->qf);
   struct radv_streamout_binding *sb = cmd_buffer->streamout_bindings;
   struct radv_streamout_state *so = &cmd_buffer->state.streamout;
   struct radv_cmd_stream *cs = cmd_buffer->cs;

   assert(firstCounterBuffer + counterBufferCount <= MAX_SO_BUFFERS);

   if (pdev->info.gfx_level >= GFX12)
      radv_init_streamout_state(cmd_buffer);
   else if (!pdev->use_ngg_streamout)
      radv_flush_vgt_streamout(cmd_buffer);

   ASSERTED unsigned cdw_max = radeon_check_space(device->ws, cs->b, MAX_SO_BUFFERS * 10);

   u_foreach_bit (i, so->enabled_mask) {
      int32_t counter_buffer_idx = i - firstCounterBuffer;
      if (counter_buffer_idx >= 0 && counter_buffer_idx >= counterBufferCount)
         counter_buffer_idx = -1;

      bool append = counter_buffer_idx >= 0 && pCounterBuffers && pCounterBuffers[counter_buffer_idx];
      uint64_t va = 0;

      if (append) {
         VK_FROM_HANDLE(radv_buffer, buffer, pCounterBuffers[counter_buffer_idx]);
         uint64_t counter_buffer_offset = 0;

         if (pCounterBufferOffsets)
            counter_buffer_offset = pCounterBufferOffsets[counter_buffer_idx];

         va += vk_buffer_address(&buffer->vk, counter_buffer_offset);

         radv_cs_add_buffer(device->ws, cs->b, buffer->bo);
      }

      if (pdev->info.gfx_level >= GFX12) {
         if (append) {
            ac_emit_cp_copy_data(cs->b, COPY_DATA_SRC_MEM, COPY_DATA_DST_MEM, va, so->state_va + i * 8 + 4,
                                 AC_CP_COPY_DATA_WR_CONFIRM);
         }
      } else if (pdev->use_ngg_streamout) {
         if (append) {
            ac_emit_cp_copy_data(cs->b, COPY_DATA_SRC_MEM, COPY_DATA_REG, va,
                                 (R_031088_GDS_STRMOUT_DWORDS_WRITTEN_0 >> 2) + i, AC_CP_COPY_DATA_WR_CONFIRM);
         } else {
            /* The PKT3 CAM bit workaround seems needed for initializing this GDS register to zero. */
            radeon_begin(cs);
            radeon_set_uconfig_perfctr_reg(pdev->info.gfx_level, ring, R_031088_GDS_STRMOUT_DWORDS_WRITTEN_0 + i * 4,
                                           0);
            radeon_end();
         }
      } else {
         radeon_begin(cs);

         /* AMD GCN binds streamout buffers as shader resources.
          * VGT only counts primitives and tells the shader through
          * SGPRs what to do.
          */
         radeon_set_context_reg(R_028AD0_VGT_STRMOUT_BUFFER_SIZE_0 + 16 * i, sb[i].size >> 2);

         cmd_buffer->cs->context_roll_without_scissor_emitted = true;

         if (append) {
            radeon_emit(PKT3(PKT3_STRMOUT_BUFFER_UPDATE, 4, 0));
            radeon_emit(STRMOUT_SELECT_BUFFER(i) | STRMOUT_DATA_TYPE(1) | /* offset in bytes */
                        STRMOUT_OFFSET_SOURCE(STRMOUT_OFFSET_FROM_MEM));  /* control */
            radeon_emit(0);                                               /* unused */
            radeon_emit(0);                                               /* unused */
            radeon_emit(va);                                              /* src address lo */
            radeon_emit(va >> 32);                                        /* src address hi */
         } else {
            /* Start from the beginning. */
            radeon_emit(PKT3(PKT3_STRMOUT_BUFFER_UPDATE, 4, 0));
            radeon_emit(STRMOUT_SELECT_BUFFER(i) | STRMOUT_DATA_TYPE(1) |   /* offset in bytes */
                        STRMOUT_OFFSET_SOURCE(STRMOUT_OFFSET_FROM_PACKET)); /* control */
            radeon_emit(0);                                                 /* unused */
            radeon_emit(0);                                                 /* unused */
            radeon_emit(0);                                                 /* unused */
            radeon_emit(0);                                                 /* unused */
         }

         radeon_end();
      }
   }

   assert(cs->b->cdw <= cdw_max);

   radv_set_streamout_enable(cmd_buffer, true);

   if (!pdev->use_ngg_streamout)
      cmd_buffer->state.dirty |= RADV_CMD_DIRTY_STREAMOUT_ENABLE;
}

VKAPI_ATTR void VKAPI_CALL
radv_CmdEndTransformFeedbackEXT(VkCommandBuffer commandBuffer, uint32_t firstCounterBuffer, uint32_t counterBufferCount,
                                const VkBuffer *pCounterBuffers, const VkDeviceSize *pCounterBufferOffsets)
{
   VK_FROM_HANDLE(radv_cmd_buffer, cmd_buffer, commandBuffer);
   struct radv_device *device = radv_cmd_buffer_device(cmd_buffer);
   const struct radv_physical_device *pdev = radv_device_physical(device);
   struct radv_streamout_state *so = &cmd_buffer->state.streamout;
   struct radv_cmd_stream *cs = cmd_buffer->cs;

   assert(firstCounterBuffer + counterBufferCount <= MAX_SO_BUFFERS);

   if (pdev->use_ngg_streamout) {
      /* Wait for streamout to finish before copying back the number of bytes
       * written.
       */
      cmd_buffer->state.flush_bits |= RADV_CMD_FLAG_VS_PARTIAL_FLUSH;
      if (pdev->info.cp_sdma_ge_use_system_memory_scope)
         cmd_buffer->state.flush_bits |= RADV_CMD_FLAG_INV_L2;

      radv_emit_cache_flush(cmd_buffer);
   } else {
      radv_flush_vgt_streamout(cmd_buffer);
   }

   ASSERTED unsigned cdw_max = radeon_check_space(device->ws, cs->b, MAX_SO_BUFFERS * 12);

   u_foreach_bit (i, so->enabled_mask) {
      int32_t counter_buffer_idx = i - firstCounterBuffer;
      if (counter_buffer_idx >= 0 && counter_buffer_idx >= counterBufferCount)
         counter_buffer_idx = -1;

      bool append = counter_buffer_idx >= 0 && pCounterBuffers && pCounterBuffers[counter_buffer_idx];
      uint64_t va = 0;

      if (append) {
         VK_FROM_HANDLE(radv_buffer, buffer, pCounterBuffers[counter_buffer_idx]);
         uint64_t counter_buffer_offset = 0;

         if (pCounterBufferOffsets)
            counter_buffer_offset = pCounterBufferOffsets[counter_buffer_idx];

         va += vk_buffer_address(&buffer->vk, counter_buffer_offset);

         radv_cs_add_buffer(device->ws, cs->b, buffer->bo);
      }

      if (pdev->info.gfx_level >= GFX12) {
         if (append) {
            ac_emit_cp_copy_data(cs->b, COPY_DATA_SRC_MEM, COPY_DATA_DST_MEM, so->state_va + i * 8 + 4, va,
                                 AC_CP_COPY_DATA_WR_CONFIRM);
         }
      } else if (pdev->use_ngg_streamout) {
         if (append) {
            ac_emit_cp_copy_data(cs->b, COPY_DATA_REG, COPY_DATA_DST_MEM,
                                 (R_031088_GDS_STRMOUT_DWORDS_WRITTEN_0 >> 2) + i, va, AC_CP_COPY_DATA_WR_CONFIRM);
         }
      } else {
         radeon_begin(cs);

         if (append) {
            radeon_emit(PKT3(PKT3_STRMOUT_BUFFER_UPDATE, 4, 0));
            radeon_emit(STRMOUT_SELECT_BUFFER(i) | STRMOUT_DATA_TYPE(1) | /* offset in bytes */
                        STRMOUT_OFFSET_SOURCE(STRMOUT_OFFSET_NONE) | STRMOUT_STORE_BUFFER_FILLED_SIZE); /* control */
            radeon_emit(va);       /* dst address lo */
            radeon_emit(va >> 32); /* dst address hi */
            radeon_emit(0);        /* unused */
            radeon_emit(0);        /* unused */
         }

         /* Deactivate transform feedback by zeroing the buffer size.
          * The counters (primitives generated, primitives emitted) may
          * be enabled even if there is not buffer bound. This ensures
          * that the primitives-emitted query won't increment.
          */
         radeon_set_context_reg(R_028AD0_VGT_STRMOUT_BUFFER_SIZE_0 + 16 * i, 0);

         radeon_end();

         cmd_buffer->cs->context_roll_without_scissor_emitted = true;
      }
   }

   assert(cs->b->cdw <= cdw_max);

   radv_set_streamout_enable(cmd_buffer, false);
}

static void
radv_emit_strmout_buffer(struct radv_cmd_buffer *cmd_buffer, const struct radv_draw_info *draw_info,
                         uint32_t counter_offset)
{
   struct radv_device *device = radv_cmd_buffer_device(cmd_buffer);
   const struct radv_physical_device *pdev = radv_device_physical(device);
   const enum amd_gfx_level gfx_level = pdev->info.gfx_level;
   struct radv_cmd_stream *cs = cmd_buffer->cs;

   if (pdev->info.gfx_level >= GFX12) {
      radeon_begin(cs);
      gfx12_begin_context_regs();
      gfx12_set_context_reg(R_028B30_VGT_STRMOUT_DRAW_OPAQUE_VERTEX_STRIDE, draw_info->stride);
      gfx12_set_context_reg(R_028B28_VGT_STRMOUT_DRAW_OPAQUE_OFFSET, counter_offset);
      gfx12_end_context_regs();
      radeon_end();
   } else {
      radeon_begin(cs);
      radeon_set_context_reg(R_028B30_VGT_STRMOUT_DRAW_OPAQUE_VERTEX_STRIDE, draw_info->stride);
      radeon_set_context_reg(R_028B28_VGT_STRMOUT_DRAW_OPAQUE_OFFSET, counter_offset);
      radeon_end();
   }

   if (gfx_level >= GFX10) {
      /* Emitting a COPY_DATA packet should be enough because RADV doesn't support preemption
       * (shadow memory) but for unknown reasons, it can lead to GPU hangs on GFX10+.
       */
      ac_emit_cp_pfp_sync_me(cs->b);

      radeon_begin(cs);
      radeon_emit(PKT3(PKT3_LOAD_CONTEXT_REG_INDEX, 3, 0));
      radeon_emit(draw_info->strmout_va);
      radeon_emit(draw_info->strmout_va >> 32);
      radeon_emit((R_028B2C_VGT_STRMOUT_DRAW_OPAQUE_BUFFER_FILLED_SIZE - SI_CONTEXT_REG_OFFSET) >> 2);
      radeon_emit(1); /* 1 DWORD */
      radeon_end();
   } else {
      ac_emit_cp_copy_data(cs->b, COPY_DATA_SRC_MEM, COPY_DATA_REG, draw_info->strmout_va,
                           R_028B2C_VGT_STRMOUT_DRAW_OPAQUE_BUFFER_FILLED_SIZE >> 2, AC_CP_COPY_DATA_WR_CONFIRM);
   }
}

VKAPI_ATTR void VKAPI_CALL
radv_CmdDrawIndirectByteCountEXT(VkCommandBuffer commandBuffer, uint32_t instanceCount, uint32_t firstInstance,
                                 VkBuffer _counterBuffer, VkDeviceSize counterBufferOffset, uint32_t counterOffset,
                                 uint32_t vertexStride)
{
   VK_FROM_HANDLE(radv_cmd_buffer, cmd_buffer, commandBuffer);
   VK_FROM_HANDLE(radv_buffer, counterBuffer, _counterBuffer);
   struct radv_device *device = radv_cmd_buffer_device(cmd_buffer);
   const struct radv_physical_device *pdev = radv_device_physical(device);
   struct radv_cmd_stream *cs = cmd_buffer->cs;
   struct radv_draw_info info;

   info.count = 0;
   info.instance_count = instanceCount;
   info.first_instance = firstInstance;
   info.strmout_va = vk_buffer_address(&counterBuffer->vk, counterBufferOffset);
   info.stride = vertexStride;
   info.indexed = false;
   info.indirect_va = 0;

   radv_cs_add_buffer(device->ws, cs->b, counterBuffer->bo);

   if (!radv_before_draw(cmd_buffer, &info, 1, false))
      return;
   struct VkMultiDrawInfoEXT minfo = {0, 0};
   radv_emit_strmout_buffer(cmd_buffer, &info, counterOffset);
   radv_emit_direct_draw_packets(cmd_buffer, &info, 1, &minfo, S_0287F0_USE_OPAQUE(1), 0);

   if (pdev->info.gfx_level == GFX12) {
      /* DrawTransformFeedback requires 3 SQ_NON_EVENTs after the packet. */
      radeon_begin(cs);
      radeon_event_write(V_028A90_SQ_NON_EVENT);
      radeon_event_write(V_028A90_SQ_NON_EVENT);
      radeon_event_write(V_028A90_SQ_NON_EVENT);
      radeon_end();
   }

   radv_after_draw(cmd_buffer, false);
}

/* VK_AMD_buffer_marker */
VKAPI_ATTR void VKAPI_CALL
radv_CmdWriteBufferMarker2AMD(VkCommandBuffer commandBuffer, VkPipelineStageFlags2 stage, VkBuffer dstBuffer,
                              VkDeviceSize dstOffset, uint32_t marker)
{
   VK_FROM_HANDLE(radv_cmd_buffer, cmd_buffer, commandBuffer);
   VK_FROM_HANDLE(radv_buffer, buffer, dstBuffer);
   struct radv_device *device = radv_cmd_buffer_device(cmd_buffer);
   const struct radv_physical_device *pdev = radv_device_physical(device);
   struct radv_cmd_stream *cs = cmd_buffer->cs;
   const uint64_t va = vk_buffer_address(&buffer->vk, dstOffset);

   radv_cs_add_buffer(device->ws, cs->b, buffer->bo);

   if (cmd_buffer->qf == RADV_QUEUE_TRANSFER) {
      radeon_check_space(device->ws, cs->b, 4);
      radv_sdma_emit_fence(cs, va, marker);
      return;
   }

   radv_emit_cache_flush(cmd_buffer);

   ASSERTED unsigned cdw_max = radeon_check_space(device->ws, cs->b, 12);

   if (!(stage & ~VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT)) {
      ac_emit_cp_copy_data(cs->b, COPY_DATA_IMM, COPY_DATA_DST_MEM, marker, va, AC_CP_COPY_DATA_WR_CONFIRM);
   } else {
      radv_cs_emit_write_event_eop(cs, pdev->info.gfx_level, V_028A90_BOTTOM_OF_PIPE_TS, 0, EOP_DST_SEL_MEM,
                                   EOP_DATA_SEL_VALUE_32BIT, va, marker, cmd_buffer->gfx9_eop_bug_va);
   }

   assert(cs->b->cdw <= cdw_max);
}

/* VK_EXT_descriptor_buffer */
VKAPI_ATTR void VKAPI_CALL
radv_CmdBindDescriptorBuffersEXT(VkCommandBuffer commandBuffer, uint32_t bufferCount,
                                 const VkDescriptorBufferBindingInfoEXT *pBindingInfos)
{
   VK_FROM_HANDLE(radv_cmd_buffer, cmd_buffer, commandBuffer);

   for (uint32_t i = 0; i < bufferCount; i++) {
      cmd_buffer->descriptor_buffers[i] = pBindingInfos[i].address;
   }
}

static void
radv_set_descriptor_buffer_offsets(struct radv_cmd_buffer *cmd_buffer,
                                   const VkSetDescriptorBufferOffsetsInfoEXT *pSetDescriptorBufferOffsetsInfo,
                                   VkPipelineBindPoint bind_point)
{
   struct radv_descriptor_state *descriptors_state = radv_get_descriptors_state(cmd_buffer, bind_point);

   for (unsigned i = 0; i < pSetDescriptorBufferOffsetsInfo->setCount; i++) {
      const uint32_t buffer_idx = pSetDescriptorBufferOffsetsInfo->pBufferIndices[i];
      const uint64_t offset = pSetDescriptorBufferOffsetsInfo->pOffsets[i];
      unsigned idx = i + pSetDescriptorBufferOffsetsInfo->firstSet;

      descriptors_state->descriptor_buffers[idx] = cmd_buffer->descriptor_buffers[buffer_idx] + offset;

      radv_set_descriptor_set(cmd_buffer, bind_point, NULL, idx);
   }
}

VKAPI_ATTR void VKAPI_CALL
radv_CmdSetDescriptorBufferOffsets2EXT(VkCommandBuffer commandBuffer,
                                       const VkSetDescriptorBufferOffsetsInfoEXT *pSetDescriptorBufferOffsetsInfo)
{
   VK_FROM_HANDLE(radv_cmd_buffer, cmd_buffer, commandBuffer);

   if (pSetDescriptorBufferOffsetsInfo->stageFlags & VK_SHADER_STAGE_COMPUTE_BIT) {
      radv_set_descriptor_buffer_offsets(cmd_buffer, pSetDescriptorBufferOffsetsInfo, VK_PIPELINE_BIND_POINT_COMPUTE);
   }

   if (pSetDescriptorBufferOffsetsInfo->stageFlags & RADV_GRAPHICS_STAGE_BITS) {
      radv_set_descriptor_buffer_offsets(cmd_buffer, pSetDescriptorBufferOffsetsInfo, VK_PIPELINE_BIND_POINT_GRAPHICS);
   }

   if (pSetDescriptorBufferOffsetsInfo->stageFlags & RADV_RT_STAGE_BITS) {
      radv_set_descriptor_buffer_offsets(cmd_buffer, pSetDescriptorBufferOffsetsInfo,
                                         VK_PIPELINE_BIND_POINT_RAY_TRACING_KHR);
   }
}

VKAPI_ATTR void VKAPI_CALL
radv_CmdBindDescriptorBufferEmbeddedSamplers2EXT(
   VkCommandBuffer commandBuffer,
   const VkBindDescriptorBufferEmbeddedSamplersInfoEXT *pBindDescriptorBufferEmbeddedSamplersInfo)
{
   /* This is a no-op because embedded samplers are inlined at compile time. */
}

/* VK_EXT_shader_object */
static void
radv_reset_pipeline_state(struct radv_cmd_buffer *cmd_buffer, VkPipelineBindPoint pipelineBindPoint)
{
   switch (pipelineBindPoint) {
   case VK_PIPELINE_BIND_POINT_COMPUTE:
      if (cmd_buffer->state.compute_pipeline) {
         radv_bind_shader(cmd_buffer, NULL, MESA_SHADER_COMPUTE);
         cmd_buffer->state.compute_pipeline = NULL;
      }
      if (cmd_buffer->state.emitted_compute_pipeline) {
         cmd_buffer->state.emitted_compute_pipeline = NULL;
      }
      break;
   case VK_PIPELINE_BIND_POINT_GRAPHICS:
      if (cmd_buffer->state.graphics_pipeline) {
         radv_foreach_stage (s, cmd_buffer->state.graphics_pipeline->active_stages) {
            radv_bind_shader(cmd_buffer, NULL, s);
         }
         cmd_buffer->state.graphics_pipeline = NULL;

         cmd_buffer->state.gs_copy_shader = NULL;
         cmd_buffer->state.last_vgt_shader = NULL;
         cmd_buffer->state.emitted_vs_prolog = NULL;
         cmd_buffer->state.ms.sample_shading_enable = false;
         cmd_buffer->state.ms.min_sample_shading = 1.0f;
         cmd_buffer->state.uses_out_of_order_rast = false;
         cmd_buffer->state.uses_vrs_attachment = false;
      }
      if (cmd_buffer->state.emitted_graphics_pipeline) {
         radv_bind_custom_blend_mode(cmd_buffer, 0);

         if (cmd_buffer->state.db_render_control) {
            cmd_buffer->state.db_render_control = 0;
            cmd_buffer->state.dirty |= RADV_CMD_DIRTY_FRAMEBUFFER;
         }

         if (cmd_buffer->state.spi_shader_col_format || cmd_buffer->state.spi_shader_z_format ||
             cmd_buffer->state.cb_shader_mask) {
            cmd_buffer->state.spi_shader_col_format = 0;
            cmd_buffer->state.spi_shader_z_format = 0;
            cmd_buffer->state.cb_shader_mask = 0;
            cmd_buffer->state.dirty |= RADV_CMD_DIRTY_FRAGMENT_OUTPUT;
         }

         cmd_buffer->state.uses_vrs = false;
         cmd_buffer->state.uses_vrs_coarse_shading = false;

         cmd_buffer->state.emitted_graphics_pipeline = NULL;
      }
      break;
   default:
      break;
   }

   cmd_buffer->state.dirty &= ~RADV_CMD_DIRTY_PIPELINE;
}

static void
radv_bind_compute_shader(struct radv_cmd_buffer *cmd_buffer, struct radv_shader_object *shader_obj)
{
   struct radv_shader *shader = shader_obj ? shader_obj->shader : NULL;
   struct radv_device *device = radv_cmd_buffer_device(cmd_buffer);
   const struct radv_physical_device *pdev = radv_device_physical(device);
   struct radv_cmd_stream *cs = cmd_buffer->cs;

   radv_bind_shader(cmd_buffer, shader, MESA_SHADER_COMPUTE);

   if (!shader_obj)
      return;

   ASSERTED const unsigned cdw_max = radeon_check_space(device->ws, cs->b, 128);

   radv_emit_compute_shader(pdev, cs, shader);

   /* Update push constants/indirect descriptors state. */
   struct radv_descriptor_state *descriptors_state =
      radv_get_descriptors_state(cmd_buffer, VK_PIPELINE_BIND_POINT_COMPUTE);
   struct radv_push_constant_state *pc_state = &cmd_buffer->push_constant_state[VK_PIPELINE_BIND_POINT_COMPUTE];

   descriptors_state->need_indirect_descriptors = radv_shader_need_indirect_descriptors(shader);
   descriptors_state->dynamic_offset_count = shader_obj->dynamic_offset_count;
   pc_state->need_upload = radv_shader_need_push_constants_upload(shader);
   pc_state->size = shader_obj->push_constant_size;

   assert(cs->b->cdw <= cdw_max);
}

VKAPI_ATTR void VKAPI_CALL
radv_CmdBindShadersEXT(VkCommandBuffer commandBuffer, uint32_t stageCount, const VkShaderStageFlagBits *pStages,
                       const VkShaderEXT *pShaders)
{
   VK_FROM_HANDLE(radv_cmd_buffer, cmd_buffer, commandBuffer);
   VkShaderStageFlagBits bound_stages = 0;

   for (uint32_t i = 0; i < stageCount; i++) {
      const mesa_shader_stage stage = vk_to_mesa_shader_stage(pStages[i]);

      if (!pShaders) {
         cmd_buffer->state.shader_objs[stage] = NULL;
         continue;
      }

      VK_FROM_HANDLE(radv_shader_object, shader_obj, pShaders[i]);

      cmd_buffer->state.shader_objs[stage] = shader_obj;

      bound_stages |= pStages[i];
   }

   if (bound_stages & VK_SHADER_STAGE_COMPUTE_BIT) {
      radv_reset_pipeline_state(cmd_buffer, VK_PIPELINE_BIND_POINT_COMPUTE);
      radv_mark_descriptors_dirty(cmd_buffer, VK_PIPELINE_BIND_POINT_COMPUTE);

      radv_bind_compute_shader(cmd_buffer, cmd_buffer->state.shader_objs[MESA_SHADER_COMPUTE]);
   }

   if (bound_stages & RADV_GRAPHICS_STAGE_BITS) {
      radv_reset_pipeline_state(cmd_buffer, VK_PIPELINE_BIND_POINT_GRAPHICS);
      radv_mark_descriptors_dirty(cmd_buffer, VK_PIPELINE_BIND_POINT_GRAPHICS);

      /* Graphics shaders are handled at draw time because of shader variants. */
   }

   cmd_buffer->state.dirty |= RADV_CMD_DIRTY_GRAPHICS_SHADERS;
}

VKAPI_ATTR void VKAPI_CALL
radv_CmdSetCoverageModulationModeNV(VkCommandBuffer commandBuffer, VkCoverageModulationModeNV coverageModulationMode)
{
   UNREACHABLE("Not supported by RADV.");
}

VKAPI_ATTR void VKAPI_CALL
radv_CmdSetCoverageModulationTableEnableNV(VkCommandBuffer commandBuffer, VkBool32 coverageModulationTableEnable)
{
   UNREACHABLE("Not supported by RADV.");
}

VKAPI_ATTR void VKAPI_CALL
radv_CmdSetCoverageModulationTableNV(VkCommandBuffer commandBuffer, uint32_t coverageModulationTableCount,
                                     const float *pCoverageModulationTable)
{
   UNREACHABLE("Not supported by RADV.");
}

VKAPI_ATTR void VKAPI_CALL
radv_CmdSetCoverageReductionModeNV(VkCommandBuffer commandBuffer, VkCoverageReductionModeNV coverageReductionMode)
{
   UNREACHABLE("Not supported by RADV.");
}

VKAPI_ATTR void VKAPI_CALL
radv_CmdSetCoverageToColorEnableNV(VkCommandBuffer commandBuffer, VkBool32 coverageToColorEnable)
{
   UNREACHABLE("Not supported by RADV.");
}

VKAPI_ATTR void VKAPI_CALL
radv_CmdSetCoverageToColorLocationNV(VkCommandBuffer commandBuffer, uint32_t coverageToColorLocation)
{
   UNREACHABLE("Not supported by RADV.");
}

VKAPI_ATTR void VKAPI_CALL
radv_CmdSetRepresentativeFragmentTestEnableNV(VkCommandBuffer commandBuffer, VkBool32 representativeFragmentTestEnable)
{
   UNREACHABLE("Not supported by RADV.");
}

VKAPI_ATTR void VKAPI_CALL
radv_CmdSetShadingRateImageEnableNV(VkCommandBuffer commandBuffer, VkBool32 shadingRateImageEnable)
{
   UNREACHABLE("Not supported by RADV.");
}

VKAPI_ATTR void VKAPI_CALL
radv_CmdSetViewportSwizzleNV(VkCommandBuffer commandBuffer, uint32_t firstViewport, uint32_t viewportCount,
                             const VkViewportSwizzleNV *pViewportSwizzles)
{
   UNREACHABLE("Not supported by RADV.");
}

VKAPI_ATTR void VKAPI_CALL
radv_CmdSetViewportWScalingEnableNV(VkCommandBuffer commandBuffer, VkBool32 viewportWScalingEnable)
{
   UNREACHABLE("Not supported by RADV.");
}

VKAPI_ATTR void VKAPI_CALL
radv_CmdSetDepthClampRangeEXT(VkCommandBuffer commandBuffer, VkDepthClampModeEXT depthClampMode,
                              const VkDepthClampRangeEXT *pDepthClampRange)
{
   VK_FROM_HANDLE(radv_cmd_buffer, cmd_buffer, commandBuffer);
   radv_cmd_set_depth_clamp_range(cmd_buffer, depthClampMode, pDepthClampRange);
}
