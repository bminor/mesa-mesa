/*
 * Copyright © 2022 Imagination Technologies Ltd.
 *
 * based in part on anv driver which is:
 * Copyright © 2015 Intel Corporation
 *
 * based in part on radv driver which is:
 * Copyright © 2016 Red Hat.
 * Copyright © 2016 Bas Nieuwenhuizen
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the next
 * paragraph) shall be included in all copies or substantial portions of the
 * Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */

#ifndef PVR_PRIVATE_H
#define PVR_PRIVATE_H

#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <vulkan/vulkan.h>

#include "compiler/shader_enums.h"
#include "hwdef/rogue_hw_defs.h"
#include "pco/pco.h"
#include "pco/pco_data.h"
#include "pvr_border.h"
#include "pvr_clear.h"
#include "pvr_common.h"
#include "pvr_csb.h"
#include "pvr_device_info.h"
#include "pvr_entrypoints.h"
#include "pvr_hw_pass.h"
#include "pvr_job_render.h"
#include "pvr_limits.h"
#include "pvr_pds.h"
#include "pvr_usc.h"
#include "pvr_spm.h"
#include "pvr_types.h"
#include "pvr_winsys.h"
#include "util/bitscan.h"
#include "util/format/u_format.h"
#include "util/log.h"
#include "util/macros.h"
#include "util/simple_mtx.h"
#include "util/u_dynarray.h"
#include "util/u_math.h"
#include "vk_command_buffer.h"
#include "vk_enum_to_str.h"
#include "vk_graphics_state.h"
#include "vk_log.h"
#include "vk_sync.h"
#include "wsi_common.h"

#ifdef HAVE_VALGRIND
#   include <valgrind/valgrind.h>
#   include <valgrind/memcheck.h>
#   define VG(x) x
#else
#   define VG(x) ((void)0)
#endif

struct pvr_bo;
struct pvr_buffer;
struct pvr_compute_pipeline;
struct pvr_device;
struct pvr_graphics_pipeline;
struct pvr_physical_device;

struct pvr_vertex_binding {
   struct pvr_buffer *buffer;
   VkDeviceSize offset;
   VkDeviceSize size;
};

#define PVR_TRANSFER_MAX_SOURCES 10U
#define PVR_TRANSFER_MAX_CUSTOM_MAPPINGS 6U

/** A surface describes a source or destination for a transfer operation. */
struct pvr_transfer_cmd_surface {
   pvr_dev_addr_t dev_addr;

   /* Memory address for extra U/V planes. */
   pvr_dev_addr_t uv_address[2];

   /* Surface width in texels. */
   uint32_t width;

   /* Surface height in texels. */
   uint32_t height;

   uint32_t depth;

   /* Z position in a 3D tecture. 0.0f <= z_position <= depth. */
   float z_position;

   /* Stride in texels. */
   uint32_t stride;

   VkFormat vk_format;

   enum pvr_memlayout mem_layout;

   uint32_t sample_count;
};

struct pvr_rect_mapping {
   VkRect2D src_rect;
   VkRect2D dst_rect;
   bool flip_x;
   bool flip_y;
};

struct pvr_transfer_cmd_source {
   struct pvr_transfer_cmd_surface surface;

   uint32_t mapping_count;
   struct pvr_rect_mapping mappings[PVR_TRANSFER_MAX_CUSTOM_MAPPINGS];

   /* In the case of a simple 1:1 copy, this setting does not affect the output
    * but will affect performance. Use clamp to edge when possible.
    */
   /* This is of type enum ROGUE_TEXSTATE_ADDRMODE. */
   int addr_mode;

   /* Source filtering method. */
   enum pvr_filter filter;

   /* MSAA resolve operation. */
   enum pvr_resolve_op resolve_op;
};

struct pvr_transfer_cmd {
   /* Node to link this cmd into the transfer_cmds list in
    * pvr_sub_cmd::transfer structure.
    */
   struct list_head link;

   uint32_t flags;

   uint32_t source_count;

   struct pvr_transfer_cmd_source sources[PVR_TRANSFER_MAX_SOURCES];

   union fi clear_color[4];

   struct pvr_transfer_cmd_surface dst;

   VkRect2D scissor;

   /* Pointer to cmd buffer this transfer cmd belongs to. This is mainly used
    * to link buffer objects allocated during job submission into
    * cmd_buffer::bo_list head.
    */
   struct pvr_cmd_buffer *cmd_buffer;

   /* Deferred RTA clears are allocated from pvr_cmd_buffer->deferred_clears and
    * cannot be freed directly.
    */
   bool is_deferred_clear;
};

struct pvr_sub_cmd_gfx {
   const struct pvr_framebuffer *framebuffer;

   struct pvr_render_job job;

   struct pvr_suballoc_bo *depth_bias_bo;
   struct pvr_suballoc_bo *scissor_bo;

   /* Tracking how the loaded depth/stencil values are being used. */
   enum pvr_depth_stencil_usage depth_usage;
   enum pvr_depth_stencil_usage stencil_usage;

   /* Tracking whether the subcommand modifies depth/stencil. */
   bool modifies_depth;
   bool modifies_stencil;

   /* Store the render to a scratch buffer. */
   bool barrier_store;
   /* Load the render (stored with a `barrier_store`) as a background to the
    * current render.
    */
   bool barrier_load;

   const struct pvr_query_pool *query_pool;
   struct util_dynarray sec_query_indices;

   /* Control stream builder object */
   struct pvr_csb control_stream;

   struct pvr_bo *multiview_ctrl_stream;
   uint32_t multiview_ctrl_stream_stride;

   /* Required iff pvr_sub_cmd_gfx_requires_split_submit() returns true. */
   struct pvr_bo *terminate_ctrl_stream;

   uint32_t hw_render_idx;

   uint32_t max_tiles_in_flight;

   bool empty_cmd;

   /* True if any fragment shader used in this sub command uses atomic
    * operations.
    */
   bool frag_uses_atomic_ops;

   bool disable_compute_overlap;

   /* True if any fragment shader used in this sub command has side
    * effects.
    */
   bool frag_has_side_effects;

   /* True if any vertex shader used in this sub command contains both
    * texture reads and texture writes.
    */
   bool vertex_uses_texture_rw;

   /* True if any fragment shader used in this sub command contains
    * both texture reads and texture writes.
    */
   bool frag_uses_texture_rw;

   bool has_query;

   bool wait_on_previous_transfer;

   bool has_depth_feedback;

   uint32_t view_mask;
   bool multiview_enabled;
};

struct pvr_sub_cmd_compute {
   /* Control stream builder object. */
   struct pvr_csb control_stream;

   uint32_t num_shared_regs;

   /* True if any shader used in this sub command uses atomic
    * operations.
    */
   bool uses_atomic_ops;

   bool uses_barrier;

   bool pds_sw_barrier_requires_clearing;
};

struct pvr_sub_cmd_transfer {
   bool serialize_with_frag;

   /* Pointer to the actual transfer command list, allowing primary and
    * secondary sub-commands to share the same list.
    */
   struct list_head *transfer_cmds;

   /* List of pvr_transfer_cmd type structures. Do not access the list
    * directly, but always use the transfer_cmds pointer above.
    */
   struct list_head transfer_cmds_priv;
};

struct pvr_sub_cmd_event {
   enum pvr_event_type type;

   union {
      struct pvr_sub_cmd_event_set_reset {
         struct pvr_event *event;
         /* Stages to wait for until the event is set or reset. */
         uint32_t wait_for_stage_mask;
      } set_reset;

      struct pvr_sub_cmd_event_wait {
         uint32_t count;
         /* Events to wait for before resuming. */
         struct pvr_event **events;
         /* Stages to wait at. */
         uint32_t *wait_at_stage_masks;
      } wait;

      struct pvr_sub_cmd_event_barrier {
         /* Stages to wait for. */
         uint32_t wait_for_stage_mask;
         /* Stages to wait at. */
         uint32_t wait_at_stage_mask;
      } barrier;
   };
};

struct pvr_sub_cmd {
   /* This links the subcommand in pvr_cmd_buffer:sub_cmds list. */
   struct list_head link;

   enum pvr_sub_cmd_type type;

   /* True if the sub_cmd is owned by this command buffer. False if taken from
    * a secondary command buffer, in that case we are not supposed to free any
    * resources associated with the sub_cmd.
    */
   bool owned;

   union {
      struct pvr_sub_cmd_gfx gfx;
      struct pvr_sub_cmd_compute compute;
      struct pvr_sub_cmd_transfer transfer;
      struct pvr_sub_cmd_event event;
   };
};

struct pvr_render_pass_info {
   const struct pvr_render_pass *pass;
   struct pvr_framebuffer *framebuffer;

   struct pvr_image_view **attachments;

   uint32_t subpass_idx;
   uint32_t current_hw_subpass;

   VkRect2D render_area;

   uint32_t clear_value_count;
   VkClearValue *clear_values;

   VkPipelineBindPoint pipeline_bind_point;

   bool process_empty_tiles;
   bool enable_bg_tag;
   uint32_t isp_userpass;
};

struct pvr_ppp_state {
   uint32_t header;

   struct {
      /* TODO: Can we get rid of the "control" field? */
      struct ROGUE_TA_STATE_ISPCTL control_struct;
      uint32_t control;

      uint32_t front_a;
      uint32_t front_b;
      uint32_t back_a;
      uint32_t back_b;
   } isp;

   struct pvr_ppp_dbsc {
      uint16_t scissor_index;
      uint16_t depthbias_index;
   } depthbias_scissor_indices;

   struct {
      uint32_t pixel_shader_base;
      uint32_t texture_uniform_code_base;
      uint32_t size_info1;
      uint32_t size_info2;
      uint32_t varying_base;
      uint32_t texture_state_data_base;
      uint32_t uniform_state_data_base;
   } pds;

   struct {
      uint32_t word0;
      uint32_t word1;
   } region_clipping;

   struct {
      uint32_t a0;
      uint32_t m0;
      uint32_t a1;
      uint32_t m1;
      uint32_t a2;
      uint32_t m2;
   } viewports[PVR_MAX_VIEWPORTS];

   uint32_t viewport_count;

   uint32_t output_selects;

   uint32_t varying_word[2];

   uint32_t ppp_control;
};

/* Represents a control stream related command that is deferred for execution in
 * a secondary command buffer.
 */
struct pvr_deferred_cs_command {
   enum pvr_deferred_cs_command_type type;
   union {
      struct {
         struct pvr_ppp_dbsc state;

         uint32_t *vdm_state;
      } dbsc;

      struct {
         struct pvr_ppp_dbsc state;

         struct pvr_suballoc_bo *ppp_cs_bo;
         uint32_t patch_offset;
      } dbsc2;
   };
};

struct pvr_cmd_buffer_draw_state {
   uint32_t base_instance;
   uint32_t base_vertex;
   bool draw_indirect;
   bool draw_indexed;
};

struct pvr_push_constants {
   uint8_t data[PVR_MAX_PUSH_CONSTANTS_SIZE];
   unsigned bytes_updated;
   pvr_dev_addr_t dev_addr;
   bool dirty;
};

struct pvr_cmd_buffer_state {
   /* Pipeline binding. */
   const struct pvr_graphics_pipeline *gfx_pipeline;

   const struct pvr_compute_pipeline *compute_pipeline;

   struct pvr_render_pass_info render_pass_info;

   struct pvr_sub_cmd *current_sub_cmd;

   struct pvr_ppp_state ppp_state;

   struct ROGUE_TA_STATE_HEADER emit_header;

   struct pvr_vertex_binding vertex_bindings[PVR_MAX_VERTEX_INPUT_BINDINGS];

   struct {
      struct pvr_buffer *buffer;
      VkDeviceSize offset;
      VkIndexType type;
   } index_buffer_binding;

   /* Array size of barriers_needed is based on number of sync pipeline
    * stages.
    */
   uint32_t barriers_needed[PVR_NUM_SYNC_PIPELINE_STAGES];

   struct pvr_descriptor_state gfx_desc_state;
   struct pvr_descriptor_state compute_desc_state;

   struct pvr_push_constants push_consts[PVR_STAGE_ALLOCATION_COUNT];

   VkFormat depth_format;

   struct {
      bool compute_pipeline_binding : 1;
      bool compute_desc_dirty : 1;

      bool gfx_pipeline_binding : 1;
      bool gfx_desc_dirty : 1;

      bool vertex_bindings : 1;
      bool index_buffer_binding : 1;
      bool vertex_descriptors : 1;
      bool fragment_descriptors : 1;

      bool isp_userpass : 1;

      /* Some draw state needs to be tracked for changes between draw calls
       * i.e. if we get a draw with baseInstance=0, followed by a call with
       * baseInstance=1 that needs to cause us to select a different PDS
       * attrib program and update the BASE_INSTANCE PDS const. If only
       * baseInstance changes then we just have to update the data section.
       */
      bool draw_base_instance : 1;
      bool draw_variant : 1;

      bool vis_test;
   } dirty;

   struct pvr_cmd_buffer_draw_state draw_state;

   struct {
      uint32_t code_offset;
      const struct pvr_pds_info *info;
   } pds_shader;

   const struct pvr_query_pool *query_pool;
   bool vis_test_enabled;
   uint32_t vis_reg;

   struct util_dynarray query_indices;

   uint32_t max_shared_regs;

   /* Address of data segment for vertex attrib upload program. */
   uint32_t pds_vertex_attrib_offset;

   uint32_t pds_fragment_descriptor_data_offset;
   uint32_t pds_compute_descriptor_data_offset;
};

/* Do not change this. This is the format used for the depth_bias_array
 * elements uploaded to the device.
 */
struct pvr_depth_bias_state {
   /* Saved information from pCreateInfo. */
   float constant_factor;
   float slope_factor;
   float clamp;
};

/* Do not change this. This is the format used for the scissor_array
 * elements uploaded to the device.
 */
struct pvr_scissor_words {
   /* Contains a packed IPF_SCISSOR_WORD_0. */
   uint32_t w0;
   /* Contains a packed IPF_SCISSOR_WORD_1. */
   uint32_t w1;
};

struct pvr_cmd_buffer {
   struct vk_command_buffer vk;

   struct pvr_device *device;

   /* Buffer usage flags */
   VkCommandBufferUsageFlags usage_flags;

   /* Array of struct pvr_depth_bias_state. */
   struct util_dynarray depth_bias_array;

   /* Array of struct pvr_scissor_words. */
   struct util_dynarray scissor_array;
   struct pvr_scissor_words scissor_words;

   struct pvr_cmd_buffer_state state;

   /* List of struct pvr_deferred_cs_command control stream related commands to
    * execute in secondary command buffer.
    */
   struct util_dynarray deferred_csb_commands;
   /* List of struct pvr_transfer_cmd used to emulate RTA clears on non RTA
    * capable cores.
    */
   struct util_dynarray deferred_clears;

   /* List of pvr_bo structs associated with this cmd buffer. */
   struct list_head bo_list;

   struct list_head sub_cmds;
};

struct pvr_stage_allocation_descriptor_state {
   struct pvr_pds_upload pds_code;
   /* Since we upload the code segment separately from the data segment
    * pds_code->data_size might be 0 whilst
    * pds_info->data_size_in_dwords might be >0 in the case of this struct
    * referring to the code upload.
    */
   struct pvr_pds_info pds_info;

   /* Already setup compile time static consts. */
   struct pvr_suballoc_bo *static_consts;
};

struct pvr_pds_attrib_program {
   struct pvr_pds_info info;
   /* The uploaded PDS program stored here only contains the code segment,
    * meaning the data size will be 0, unlike the data size stored in the
    * 'info' member above.
    */
   struct pvr_pds_upload program;
};

struct pvr_pipeline_stage_state {
   uint32_t pds_temps_count;
};

struct pvr_compute_shader_state {
   /* Pointer to a buffer object that contains the shader binary. */
   struct pvr_suballoc_bo *shader_bo;

   /* Buffer object for the coefficient update shader binary. */
   struct pvr_suballoc_bo *coeff_update_shader_bo;
   uint32_t coeff_update_shader_temps;
};

struct pvr_vertex_shader_state {
   /* Pointer to a buffer object that contains the shader binary. */
   struct pvr_suballoc_bo *shader_bo;

   struct pvr_pds_attrib_program
      pds_attrib_programs[PVR_PDS_VERTEX_ATTRIB_PROGRAM_COUNT];

   struct pvr_pipeline_stage_state stage_state;
   /* FIXME: Move this into stage_state? */
   struct pvr_stage_allocation_descriptor_state descriptor_state;
};

struct pvr_fragment_shader_state {
   /* Pointer to a buffer object that contains the shader binary. */
   struct pvr_suballoc_bo *shader_bo;

   struct pvr_pipeline_stage_state stage_state;
   /* FIXME: Move this into stage_state? */
   struct pvr_stage_allocation_descriptor_state descriptor_state;
   enum ROGUE_TA_PASSTYPE pass_type;

   struct pvr_pds_coeff_loading_program pds_coeff_program;
   uint32_t *pds_coeff_program_buffer;

   struct pvr_pds_kickusc_program pds_fragment_program;
   uint32_t *pds_fragment_program_buffer;
};

struct pvr_pipeline {
   struct vk_object_base base;
   enum pvr_pipeline_type type;
   struct vk_pipeline_layout *layout;
   VkPipelineCreateFlags2KHR pipeline_flags;
};

struct pvr_compute_pipeline {
   struct pvr_pipeline base;

   pco_data cs_data;

   struct pvr_compute_shader_state shader_state;
   struct pvr_stage_allocation_descriptor_state descriptor_state;

   struct pvr_pds_upload pds_cs_program;
   struct pvr_pds_info pds_cs_program_info;

   uint32_t *pds_cs_data_section;
   uint32_t base_workgroup_data_patching_offset;
   uint32_t num_workgroups_data_patching_offset;
   uint32_t num_workgroups_indirect_src_patching_offset;
   uint32_t num_workgroups_indirect_src_dma_patching_offset;
};

struct pvr_graphics_pipeline {
   struct pvr_pipeline base;

   struct vk_dynamic_graphics_state dynamic_state;

   /* Derived and other state */
   size_t stage_indices[MESA_SHADER_STAGES];

   pco_data vs_data;
   pco_data fs_data;

   struct {
      struct pvr_vertex_shader_state vertex;
      struct pvr_fragment_shader_state fragment;
   } shader_state;
};

struct pvr_query_pool {
   struct vk_object_base base;

   /* Stride of result_buffer to get to the start of the results for the next
    * Phantom.
    */
   uint32_t result_stride;

   uint32_t query_count;

   struct pvr_suballoc_bo *result_buffer;
   struct pvr_suballoc_bo *availability_buffer;
};

struct pvr_private_compute_pipeline {
   /* Used by pvr_compute_update_kernel_private(). */
   uint32_t pds_code_offset;
   uint32_t pds_data_offset;
   uint32_t pds_data_size_dw;
   uint32_t pds_temps_used;
   uint32_t coeff_regs_count;
   uint32_t unified_store_regs_count;
   VkExtent3D workgroup_size;

   /* Used by pvr_compute_update_shared_private(). */
   uint32_t pds_shared_update_code_offset;
   uint32_t pds_shared_update_data_offset;
   uint32_t pds_shared_update_data_size_dw;

   /* Used by both pvr_compute_update_{kernel,shared}_private(). */
   uint32_t const_shared_regs_count;

   pvr_dev_addr_t const_buffer_addr;
};

struct pvr_query_info {
   enum pvr_query_type type;

   union {
      struct {
         uint32_t num_query_indices;
         struct pvr_suballoc_bo *index_bo;
         uint32_t num_queries;
         struct pvr_suballoc_bo *availability_bo;
      } availability_write;

      struct {
         VkQueryPool query_pool;
         uint32_t first_query;
         uint32_t query_count;
      } reset_query_pool;

      struct {
         VkQueryPool query_pool;
         uint32_t first_query;
         uint32_t query_count;
         VkBuffer dst_buffer;
         VkDeviceSize dst_offset;
         VkDeviceSize stride;
         VkQueryResultFlags flags;
      } copy_query_results;
   };
};

struct pvr_render_target {
   struct pvr_rt_dataset *rt_dataset[PVR_MAX_MULTIVIEW];

   pthread_mutex_t mutex;

   uint32_t valid_mask;
};

struct pvr_framebuffer {
   struct vk_object_base base;

   /* Saved information from pCreateInfo. */
   uint32_t width;
   uint32_t height;
   uint32_t layers;

   uint32_t attachment_count;
   struct pvr_image_view **attachments;

   /* Derived and other state. */
   struct pvr_suballoc_bo *ppp_state_bo;
   /* PPP state size in dwords. */
   size_t ppp_state_size;

   uint32_t render_targets_count;
   struct pvr_render_target *render_targets;

   struct pvr_spm_scratch_buffer *scratch_buffer;

   uint32_t render_count;
   struct pvr_spm_eot_state *spm_eot_state_per_render;
   struct pvr_spm_bgobj_state *spm_bgobj_state_per_render;
};

struct pvr_render_pass_attachment {
   /* Saved information from pCreateInfo. */
   VkAttachmentLoadOp load_op;

   VkAttachmentStoreOp store_op;

   VkAttachmentLoadOp stencil_load_op;

   VkAttachmentStoreOp stencil_store_op;

   VkFormat vk_format;
   uint32_t sample_count;
   VkImageLayout initial_layout;

   /* Derived and other state. */
   VkImageAspectFlags aspects;

   /* Can this surface be resolved by the PBE. */
   bool is_pbe_downscalable;

   uint32_t index;
};

struct pvr_render_input_attachment {
   uint32_t attachment_idx;
   VkImageAspectFlags aspect_mask;
};

struct pvr_render_subpass {
   /* Saved information from pCreateInfo. */
   /* The number of samples per color attachment (or depth attachment if
    * z-only).
    */
   /* FIXME: rename to 'samples' to match struct pvr_image */
   uint32_t sample_count;

   uint32_t color_count;
   uint32_t *color_attachments;
   uint32_t *resolve_attachments;

   uint32_t input_count;
   struct pvr_render_input_attachment *input_attachments;

   uint32_t depth_stencil_attachment;

   uint32_t depth_stencil_resolve_attachment;
   VkResolveModeFlagBits depth_resolve_mode;
   VkResolveModeFlagBits stencil_resolve_mode;

   /*  Derived and other state. */
   uint32_t dep_count;
   uint32_t *dep_list;

   /* Array with dep_count elements. flush_on_dep[x] is true if this subpass
    * and the subpass dep_list[x] can't be in the same hardware render.
    */
   bool *flush_on_dep;

   uint32_t index;

   uint32_t isp_userpass;

   VkPipelineBindPoint pipeline_bind_point;

   /* View mask for multiview. */
   uint32_t view_mask;
};

struct pvr_render_pass {
   struct vk_object_base base;

   /* Saved information from pCreateInfo. */
   uint32_t attachment_count;

   struct pvr_render_pass_attachment *attachments;

   uint32_t subpass_count;

   struct pvr_render_subpass *subpasses;

   struct pvr_renderpass_hwsetup *hw_setup;

   /*  Derived and other state. */
   /* FIXME: rename to 'max_samples' as we use 'samples' elsewhere */
   uint32_t max_sample_count;

   /* The maximum number of tile buffers to use in any subpass. */
   uint32_t max_tilebuffer_count;

   /* VkSubpassDescription2::viewMask or 1 when non-multiview
    *
    * To determine whether multiview is enabled, check
    * pvr_render_pass::multiview_enabled.
    */
   bool multiview_enabled;
};

/* Max render targets for the clears loads state in load op.
 * To account for resolve attachments, double the color attachments.
 */
#define PVR_LOAD_OP_CLEARS_LOADS_MAX_RTS (PVR_MAX_COLOR_ATTACHMENTS * 2)

struct pvr_load_op {
   bool is_hw_object;

   struct pvr_suballoc_bo *usc_frag_prog_bo;
   uint32_t const_shareds_count;
   uint32_t shareds_count;
   uint32_t num_tile_buffers;

   struct pvr_pds_upload pds_frag_prog;

   struct pvr_pds_upload pds_tex_state_prog;
   uint32_t temps_count;

   union {
      const struct pvr_renderpass_hwsetup_render *hw_render;
      const struct pvr_render_subpass *subpass;
   };

   /* TODO: We might not need to keep all of this around. Some stuff might just
    * be for the compiler to ingest which we can then discard.
    */
   struct {
      uint16_t rt_clear_mask;
      uint16_t rt_load_mask;

      uint16_t unresolved_msaa_mask;

      /* The format to write to the output regs. */
      VkFormat dest_vk_format[PVR_LOAD_OP_CLEARS_LOADS_MAX_RTS];

#define PVR_NO_DEPTH_CLEAR_TO_REG (-1)
      /* If >= 0, write a depth clear value to the specified pixel output. */
      int32_t depth_clear_to_reg;

      const struct usc_mrt_setup *mrt_setup;
   } clears_loads_state;

   uint32_t view_indices[PVR_MAX_MULTIVIEW];

   uint32_t view_count;
};

#define CHECK_MASK_SIZE(_struct_type, _field_name, _nr_bits)               \
   static_assert(sizeof(((struct _struct_type *)NULL)->_field_name) * 8 >= \
                    _nr_bits,                                              \
                 #_field_name " mask of struct " #_struct_type " too small")

CHECK_MASK_SIZE(pvr_load_op,
                clears_loads_state.rt_clear_mask,
                PVR_LOAD_OP_CLEARS_LOADS_MAX_RTS);
CHECK_MASK_SIZE(pvr_load_op,
                clears_loads_state.rt_load_mask,
                PVR_LOAD_OP_CLEARS_LOADS_MAX_RTS);
CHECK_MASK_SIZE(pvr_load_op,
                clears_loads_state.unresolved_msaa_mask,
                PVR_LOAD_OP_CLEARS_LOADS_MAX_RTS);

#undef CHECK_MASK_SIZE

struct pvr_load_op_state {
   uint32_t load_op_count;

   /* Load op array indexed by HW render view (not by the index in the view
    * mask).
    */
   struct pvr_load_op *load_ops;
};

VkResult pvr_wsi_init(struct pvr_physical_device *pdevice);
void pvr_wsi_finish(struct pvr_physical_device *pdevice);

VkResult pvr_cmd_buffer_add_transfer_cmd(struct pvr_cmd_buffer *cmd_buffer,
                                         struct pvr_transfer_cmd *transfer_cmd);

VkResult pvr_cmd_buffer_alloc_mem(struct pvr_cmd_buffer *cmd_buffer,
                                  struct pvr_winsys_heap *heap,
                                  uint64_t size,
                                  struct pvr_suballoc_bo **const pvr_bo_out);

void pvr_calculate_vertex_cam_size(const struct pvr_device_info *dev_info,
                                   const uint32_t vs_output_size,
                                   const bool raster_enable,
                                   uint32_t *const cam_size_out,
                                   uint32_t *const vs_max_instances_out);

static inline struct pvr_compute_pipeline *
to_pvr_compute_pipeline(struct pvr_pipeline *pipeline)
{
   assert(pipeline->type == PVR_PIPELINE_TYPE_COMPUTE);
   return container_of(pipeline, struct pvr_compute_pipeline, base);
}

static inline struct pvr_graphics_pipeline *
to_pvr_graphics_pipeline(struct pvr_pipeline *pipeline)
{
   assert(pipeline->type == PVR_PIPELINE_TYPE_GRAPHICS);
   return container_of(pipeline, struct pvr_graphics_pipeline, base);
}

static inline struct pvr_descriptor_set_layout *
vk_to_pvr_descriptor_set_layout(struct vk_descriptor_set_layout *layout)
{
   return container_of(layout, struct pvr_descriptor_set_layout, vk);
}

static enum pvr_pipeline_stage_bits
pvr_stage_mask(VkPipelineStageFlags2 stage_mask)
{
   enum pvr_pipeline_stage_bits stages = 0;

   if (stage_mask & VK_PIPELINE_STAGE_ALL_COMMANDS_BIT)
      return PVR_PIPELINE_STAGE_ALL_BITS;

   if (stage_mask & (VK_PIPELINE_STAGE_ALL_GRAPHICS_BIT))
      stages |= PVR_PIPELINE_STAGE_ALL_GRAPHICS_BITS;

   if (stage_mask & (VK_PIPELINE_STAGE_DRAW_INDIRECT_BIT |
                     VK_PIPELINE_STAGE_VERTEX_INPUT_BIT |
                     VK_PIPELINE_STAGE_VERTEX_SHADER_BIT |
                     VK_PIPELINE_STAGE_TESSELLATION_CONTROL_SHADER_BIT |
                     VK_PIPELINE_STAGE_TESSELLATION_EVALUATION_SHADER_BIT |
                     VK_PIPELINE_STAGE_GEOMETRY_SHADER_BIT)) {
      stages |= PVR_PIPELINE_STAGE_GEOM_BIT;
   }

   if (stage_mask & (VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT |
                     VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT |
                     VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT |
                     VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT)) {
      stages |= PVR_PIPELINE_STAGE_FRAG_BIT;
   }

   if (stage_mask & (VK_PIPELINE_STAGE_DRAW_INDIRECT_BIT |
                     VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT)) {
      stages |= PVR_PIPELINE_STAGE_COMPUTE_BIT;
   }

   if (stage_mask & (VK_PIPELINE_STAGE_TRANSFER_BIT))
      stages |= PVR_PIPELINE_STAGE_TRANSFER_BIT;

   return stages;
}

static inline enum pvr_pipeline_stage_bits
pvr_stage_mask_src(VkPipelineStageFlags2 stage_mask)
{
   /* If the source is bottom of pipe, all stages will need to be waited for. */
   if (stage_mask & VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT)
      return PVR_PIPELINE_STAGE_ALL_BITS;

   return pvr_stage_mask(stage_mask);
}

static inline enum pvr_pipeline_stage_bits
pvr_stage_mask_dst(VkPipelineStageFlags2 stage_mask)
{
   /* If the destination is top of pipe, all stages should be blocked by prior
    * commands.
    */
   if (stage_mask & VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT)
      return PVR_PIPELINE_STAGE_ALL_BITS;

   return pvr_stage_mask(stage_mask);
}

static inline bool pvr_sub_cmd_gfx_requires_split_submit(
   const struct pvr_sub_cmd_gfx *const sub_cmd)
{
   return sub_cmd->job.run_frag && sub_cmd->framebuffer->layers > 1;
}

/* This function is intended to be used when the error being set has been
 * returned from a function call, i.e. the error happened further down the
 * stack. `vk_command_buffer_set_error()` should be used at the point an error
 * occurs, i.e. VK_ERROR_* is being passed in.
 * This ensures we only ever get the error printed once.
 */
static inline VkResult
pvr_cmd_buffer_set_error_unwarned(struct pvr_cmd_buffer *cmd_buffer,
                                  VkResult error)
{
   assert(error != VK_SUCCESS);

   if (cmd_buffer->vk.record_result == VK_SUCCESS)
      cmd_buffer->vk.record_result = error;

   return error;
}

VkResult pvr_pds_unitex_state_program_create_and_upload(
   struct pvr_device *device,
   const VkAllocationCallbacks *allocator,
   uint32_t texture_kicks,
   uint32_t uniform_kicks,
   struct pvr_pds_upload *const pds_upload_out);

VkResult
pvr_cmd_buffer_upload_general(struct pvr_cmd_buffer *const cmd_buffer,
                              const void *const data,
                              const size_t size,
                              struct pvr_suballoc_bo **const pvr_bo_out);
VkResult pvr_cmd_buffer_upload_pds(struct pvr_cmd_buffer *const cmd_buffer,
                                   const uint32_t *data,
                                   uint32_t data_size_dwords,
                                   uint32_t data_alignment,
                                   const uint32_t *code,
                                   uint32_t code_size_dwords,
                                   uint32_t code_alignment,
                                   uint64_t min_alignment,
                                   struct pvr_pds_upload *const pds_upload_out);

VkResult pvr_cmd_buffer_start_sub_cmd(struct pvr_cmd_buffer *cmd_buffer,
                                      enum pvr_sub_cmd_type type);
VkResult pvr_cmd_buffer_end_sub_cmd(struct pvr_cmd_buffer *cmd_buffer);

void pvr_compute_generate_fence(struct pvr_cmd_buffer *cmd_buffer,
                                struct pvr_sub_cmd_compute *const sub_cmd,
                                bool deallocate_shareds);
void pvr_compute_update_shared_private(
   struct pvr_cmd_buffer *cmd_buffer,
   struct pvr_sub_cmd_compute *const sub_cmd,
   struct pvr_private_compute_pipeline *pipeline);
void pvr_compute_update_kernel_private(
   struct pvr_cmd_buffer *cmd_buffer,
   struct pvr_sub_cmd_compute *const sub_cmd,
   struct pvr_private_compute_pipeline *pipeline,
   const uint32_t global_workgroup_size[static const PVR_WORKGROUP_DIMENSIONS]);

size_t pvr_pds_get_max_descriptor_upload_const_map_size_in_bytes(void);

VkResult pvr_device_create_compute_query_programs(struct pvr_device *device);
void pvr_device_destroy_compute_query_programs(struct pvr_device *device);

VkResult pvr_add_query_program(struct pvr_cmd_buffer *cmd_buffer,
                               const struct pvr_query_info *query_info);

void pvr_reset_graphics_dirty_state(struct pvr_cmd_buffer *const cmd_buffer,
                                    bool start_geom);

const struct pvr_renderpass_hwsetup_subpass *
pvr_get_hw_subpass(const struct pvr_render_pass *pass, const uint32_t subpass);

static inline void
pvr_render_targets_datasets_destroy(struct pvr_render_target *render_target)
{
   u_foreach_bit (valid_idx, render_target->valid_mask) {
      struct pvr_rt_dataset *rt_dataset = render_target->rt_dataset[valid_idx];

      if (rt_dataset && render_target->valid_mask & BITFIELD_BIT(valid_idx))
         pvr_render_target_dataset_destroy(rt_dataset);

      render_target->rt_dataset[valid_idx] = NULL;
      render_target->valid_mask &= ~BITFIELD_BIT(valid_idx);
   }
}

VK_DEFINE_HANDLE_CASTS(pvr_cmd_buffer,
                       vk.base,
                       VkCommandBuffer,
                       VK_OBJECT_TYPE_COMMAND_BUFFER)

VK_DEFINE_NONDISP_HANDLE_CASTS(pvr_descriptor_set_layout,
                               vk.base,
                               VkDescriptorSetLayout,
                               VK_OBJECT_TYPE_DESCRIPTOR_SET_LAYOUT)
VK_DEFINE_NONDISP_HANDLE_CASTS(pvr_descriptor_set,
                               base,
                               VkDescriptorSet,
                               VK_OBJECT_TYPE_DESCRIPTOR_SET)
VK_DEFINE_NONDISP_HANDLE_CASTS(pvr_descriptor_pool,
                               base,
                               VkDescriptorPool,
                               VK_OBJECT_TYPE_DESCRIPTOR_POOL)
VK_DEFINE_NONDISP_HANDLE_CASTS(pvr_pipeline,
                               base,
                               VkPipeline,
                               VK_OBJECT_TYPE_PIPELINE)
VK_DEFINE_NONDISP_HANDLE_CASTS(pvr_query_pool,
                               base,
                               VkQueryPool,
                               VK_OBJECT_TYPE_QUERY_POOL)
VK_DEFINE_NONDISP_HANDLE_CASTS(pvr_framebuffer,
                               base,
                               VkFramebuffer,
                               VK_OBJECT_TYPE_FRAMEBUFFER)
VK_DEFINE_NONDISP_HANDLE_CASTS(pvr_render_pass,
                               base,
                               VkRenderPass,
                               VK_OBJECT_TYPE_RENDER_PASS)

#define PVR_CHECK_COMMAND_BUFFER_BUILDING_STATE(cmd_buffer)                  \
   do {                                                                      \
      struct pvr_cmd_buffer *const _cmd_buffer = (cmd_buffer);               \
      const VkResult _record_result =                                        \
         vk_command_buffer_get_record_result(&_cmd_buffer->vk);              \
                                                                             \
      if (_cmd_buffer->vk.state != MESA_VK_COMMAND_BUFFER_STATE_RECORDING) { \
         vk_errorf(_cmd_buffer,                                              \
                   VK_ERROR_OUT_OF_DEVICE_MEMORY,                            \
                   "Command buffer is not in recording state");              \
         return;                                                             \
      } else if (_record_result < VK_SUCCESS) {                              \
         vk_errorf(_cmd_buffer,                                              \
                   _record_result,                                           \
                   "Skipping function as command buffer has "                \
                   "previous build error");                                  \
         return;                                                             \
      }                                                                      \
   } while (0)

/**
 * Print a FINISHME message, including its source location.
 */
#define pvr_finishme(format, ...)              \
   do {                                        \
      static bool reported = false;            \
      if (!reported) {                         \
         mesa_logw("%s:%d: FINISHME: " format, \
                   __FILE__,                   \
                   __LINE__,                   \
                   ##__VA_ARGS__);             \
         reported = true;                      \
      }                                        \
   } while (false)

#define PVR_WRITE(_buffer, _value, _offset, _max)                \
   do {                                                          \
      __typeof__(_value) __value = _value;                       \
      uint64_t __offset = _offset;                               \
      uint32_t __nr_dwords = sizeof(__value) / sizeof(uint32_t); \
      static_assert(__same_type(*_buffer, __value),              \
                    "Buffer and value type mismatch");           \
      assert((__offset + __nr_dwords) <= (_max));                \
      assert((__offset % __nr_dwords) == 0U);                    \
      _buffer[__offset / __nr_dwords] = __value;                 \
   } while (0)

/* A non-fatal assert. Useful for debugging. */
#if MESA_DEBUG
#   define pvr_assert(x)                                           \
      ({                                                           \
         if (unlikely(!(x)))                                       \
            mesa_loge("%s:%d ASSERT: %s", __FILE__, __LINE__, #x); \
      })
#else
#   define pvr_assert(x)
#endif

#endif /* PVR_PRIVATE_H */
