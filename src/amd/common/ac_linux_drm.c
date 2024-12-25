/*
 * Copyright 2024 Advanced Micro Devices, Inc.
 * SPDX-License-Identifier: MIT
 */

#include "util/os_drm.h"
#include "ac_linux_drm.h"
#include "util/u_math.h"

#include <stdlib.h>
#include <time.h>
#include <unistd.h>

int ac_drm_bo_set_metadata(int device_fd, uint32_t bo_handle, struct amdgpu_bo_metadata *info)
{
   struct drm_amdgpu_gem_metadata args = {};

   args.handle = bo_handle;
   args.op = AMDGPU_GEM_METADATA_OP_SET_METADATA;
   args.data.flags = info->flags;
   args.data.tiling_info = info->tiling_info;

   if (info->size_metadata > sizeof(args.data.data))
      return -EINVAL;

   if (info->size_metadata) {
      args.data.data_size_bytes = info->size_metadata;
      memcpy(args.data.data, info->umd_metadata, info->size_metadata);
   }

   return drm_ioctl_write_read(device_fd, DRM_AMDGPU_GEM_METADATA, &args, sizeof(args));
}

int ac_drm_bo_query_info(int device_fd, uint32_t bo_handle, struct amdgpu_bo_info *info)
{
   struct drm_amdgpu_gem_metadata metadata = {};
   struct drm_amdgpu_gem_create_in bo_info = {};
   struct drm_amdgpu_gem_op gem_op = {};
   int r;

   /* Validate the BO passed in */
   if (!bo_handle)
      return -EINVAL;

   /* Query metadata. */
   metadata.handle = bo_handle;
   metadata.op = AMDGPU_GEM_METADATA_OP_GET_METADATA;

   r = drm_ioctl_write_read(device_fd, DRM_AMDGPU_GEM_METADATA, &metadata, sizeof(metadata));
   if (r)
      return r;

   if (metadata.data.data_size_bytes > sizeof(info->metadata.umd_metadata))
      return -EINVAL;

   /* Query buffer info. */
   gem_op.handle = bo_handle;
   gem_op.op = AMDGPU_GEM_OP_GET_GEM_CREATE_INFO;
   gem_op.value = (uintptr_t)&bo_info;

   r = drm_ioctl_write_read(device_fd, DRM_AMDGPU_GEM_OP, &gem_op, sizeof(gem_op));
   if (r)
      return r;

   memset(info, 0, sizeof(*info));
   info->alloc_size = bo_info.bo_size;
   info->phys_alignment = bo_info.alignment;
   info->preferred_heap = bo_info.domains;
   info->alloc_flags = bo_info.domain_flags;
   info->metadata.flags = metadata.data.flags;
   info->metadata.tiling_info = metadata.data.tiling_info;

   info->metadata.size_metadata = metadata.data.data_size_bytes;
   if (metadata.data.data_size_bytes > 0)
      memcpy(info->metadata.umd_metadata, metadata.data.data, metadata.data.data_size_bytes);

   return 0;
}

static uint64_t amdgpu_cs_calculate_timeout(uint64_t timeout)
{
   int r;

   if (timeout != AMDGPU_TIMEOUT_INFINITE) {
      struct timespec current;
      uint64_t current_ns;
      r = clock_gettime(CLOCK_MONOTONIC, &current);
      if (r) {
         fprintf(stderr, "clock_gettime() returned error (%d)!", errno);
         return AMDGPU_TIMEOUT_INFINITE;
      }

      current_ns = ((uint64_t)current.tv_sec) * 1000000000ull;
      current_ns += current.tv_nsec;
      timeout += current_ns;
      if (timeout < current_ns)
         timeout = AMDGPU_TIMEOUT_INFINITE;
   }
   return timeout;
}

int ac_drm_bo_wait_for_idle(int device_fd, uint32_t bo_handle, uint64_t timeout_ns, bool *busy)
{
   union drm_amdgpu_gem_wait_idle args;
   int r;

   memset(&args, 0, sizeof(args));
   args.in.handle = bo_handle;
   args.in.timeout = amdgpu_cs_calculate_timeout(timeout_ns);

   r = drm_ioctl_write_read(device_fd, DRM_AMDGPU_GEM_WAIT_IDLE, &args, sizeof(args));

   if (r == 0) {
      *busy = args.out.status;
      return 0;
   } else {
      fprintf(stderr, "amdgpu: GEM_WAIT_IDLE failed with %i\n", r);
      return r;
   }
}

int ac_drm_bo_va_op(int device_fd, uint32_t bo_handle, uint64_t offset, uint64_t size,
                    uint64_t addr, uint64_t flags, uint32_t ops)
{
   size = ALIGN(size, getpagesize());

   return ac_drm_bo_va_op_raw(
      device_fd, bo_handle, offset, size, addr,
      AMDGPU_VM_PAGE_READABLE | AMDGPU_VM_PAGE_WRITEABLE | AMDGPU_VM_PAGE_EXECUTABLE, ops);
}

int ac_drm_bo_va_op_raw(int device_fd, uint32_t bo_handle, uint64_t offset, uint64_t size,
                        uint64_t addr, uint64_t flags, uint32_t ops)
{
   struct drm_amdgpu_gem_va va;
   int r;

   if (ops != AMDGPU_VA_OP_MAP && ops != AMDGPU_VA_OP_UNMAP && ops != AMDGPU_VA_OP_REPLACE &&
       ops != AMDGPU_VA_OP_CLEAR)
      return -EINVAL;

   memset(&va, 0, sizeof(va));
   va.handle = bo_handle;
   va.operation = ops;
   va.flags = flags;
   va.va_address = addr;
   va.offset_in_bo = offset;
   va.map_size = size;

   r = drm_ioctl_write_read(device_fd, DRM_AMDGPU_GEM_VA, &va, sizeof(va));

   return r;
}

int ac_drm_bo_va_op_raw2(int device_fd, uint32_t bo_handle, uint64_t offset, uint64_t size,
                         uint64_t addr, uint64_t flags, uint32_t ops,
                         uint32_t vm_timeline_syncobj_out, uint64_t vm_timeline_point,
                         uint64_t input_fence_syncobj_handles, uint32_t num_syncobj_handles)
{
   struct drm_amdgpu_gem_va va;
   int r;

   if (ops != AMDGPU_VA_OP_MAP && ops != AMDGPU_VA_OP_UNMAP &&
       ops != AMDGPU_VA_OP_REPLACE && ops != AMDGPU_VA_OP_CLEAR)
      return -EINVAL;

   memset(&va, 0, sizeof(va));
   va.handle = bo_handle;
   va.operation = ops;
   va.flags = flags;
   va.va_address = addr;
   va.offset_in_bo = offset;
   va.map_size = size;
   va.vm_timeline_syncobj_out = vm_timeline_syncobj_out;
   va.vm_timeline_point = vm_timeline_point;
   va.input_fence_syncobj_handles = input_fence_syncobj_handles;
   va.num_syncobj_handles = num_syncobj_handles;

   r = drm_ioctl_write_read(device_fd, DRM_AMDGPU_GEM_VA, &va, sizeof(va));

   return r;
}

int ac_drm_cs_ctx_create2(int device_fd, uint32_t priority, uint32_t *ctx_handle)
{
   union drm_amdgpu_ctx args;
   int r;
   char *override_priority;

   override_priority = getenv("AMD_PRIORITY");
   if (override_priority) {
      /* The priority is a signed integer. The variable type is
       * wrong. If parsing fails, priority is unchanged.
       */
      if (sscanf(override_priority, "%i", &priority) == 1) {
         printf("amdgpu: context priority changed to %i\n", priority);
      }
   }

   /* Create the context */
   memset(&args, 0, sizeof(args));
   args.in.op = AMDGPU_CTX_OP_ALLOC_CTX;
   args.in.priority = priority;

   r = drm_ioctl_write_read(device_fd, DRM_AMDGPU_CTX, &args, sizeof(args));
   if (r)
      return r;

   *ctx_handle = args.out.alloc.ctx_id;
   return 0;
}

int ac_drm_cs_ctx_free(int device_fd, uint32_t ctx_handle)
{
   union drm_amdgpu_ctx args;

   /* now deal with kernel side */
   memset(&args, 0, sizeof(args));
   args.in.op = AMDGPU_CTX_OP_FREE_CTX;
   args.in.ctx_id = ctx_handle;
   return drm_ioctl_write_read(device_fd, DRM_AMDGPU_CTX, &args, sizeof(args));
}

int ac_drm_cs_ctx_stable_pstate(int device_fd, uint32_t ctx_handle, uint32_t op, uint32_t flags,
                                uint32_t *out_flags)
{
   union drm_amdgpu_ctx args;
   int r;

   if (!ctx_handle)
      return -EINVAL;

   memset(&args, 0, sizeof(args));
   args.in.op = op;
   args.in.ctx_id = ctx_handle;
   args.in.flags = flags;
   r = drm_ioctl_write_read(device_fd, DRM_AMDGPU_CTX, &args, sizeof(args));
   if (!r && out_flags)
      *out_flags = args.out.pstate.flags;
   return r;
}

int ac_drm_cs_query_reset_state2(int device_fd, uint32_t ctx_handle, uint64_t *flags)
{
   union drm_amdgpu_ctx args;
   int r;

   if (!ctx_handle)
      return -EINVAL;

   memset(&args, 0, sizeof(args));
   args.in.op = AMDGPU_CTX_OP_QUERY_STATE2;
   args.in.ctx_id = ctx_handle;
   r = drm_ioctl_write_read(device_fd, DRM_AMDGPU_CTX, &args, sizeof(args));
   if (!r)
      *flags = args.out.state.flags;
   return r;
}

static int amdgpu_ioctl_wait_cs(int device_fd, uint32_t ctx_handle, unsigned ip,
                                unsigned ip_instance, uint32_t ring, uint64_t handle,
                                uint64_t timeout_ns, uint64_t flags, bool *busy)
{
   union drm_amdgpu_wait_cs args;
   int r;

   memset(&args, 0, sizeof(args));
   args.in.handle = handle;
   args.in.ip_type = ip;
   args.in.ip_instance = ip_instance;
   args.in.ring = ring;
   args.in.ctx_id = ctx_handle;

   if (flags & AMDGPU_QUERY_FENCE_TIMEOUT_IS_ABSOLUTE)
      args.in.timeout = timeout_ns;
   else
      args.in.timeout = amdgpu_cs_calculate_timeout(timeout_ns);

   r = drm_ioctl(device_fd, DRM_IOCTL_AMDGPU_WAIT_CS, &args);
   if (r)
      return -errno;

   *busy = args.out.status;
   return 0;
}

int ac_drm_cs_query_fence_status(int device_fd, uint32_t ctx_handle, uint32_t ip_type,
                                 uint32_t ip_instance, uint32_t ring, uint64_t fence_seq_no,
                                 uint64_t timeout_ns, uint64_t flags, uint32_t *expired)
{
   bool busy = true;
   int r;

   if (!fence_seq_no) {
      *expired = true;
      return 0;
   }

   *expired = false;

   r = amdgpu_ioctl_wait_cs(device_fd, ctx_handle, ip_type, ip_instance, ring, fence_seq_no,
                            timeout_ns, flags, &busy);

   if (!r && !busy)
      *expired = true;

   return r;
}

int ac_drm_cs_create_syncobj2(int device_fd, uint32_t flags, uint32_t *handle)
{
   return drmSyncobjCreate(device_fd, flags, handle);
}

int ac_drm_cs_create_syncobj(int device_fd, uint32_t *handle)
{
   return drmSyncobjCreate(device_fd, 0, handle);
}

int ac_drm_cs_destroy_syncobj(int device_fd, uint32_t handle)
{
   return drmSyncobjDestroy(device_fd, handle);
}

int ac_drm_cs_syncobj_wait(int device_fd, uint32_t *handles, unsigned num_handles,
                           int64_t timeout_nsec, unsigned flags, uint32_t *first_signaled)
{
   return drmSyncobjWait(device_fd, handles, num_handles, timeout_nsec, flags, first_signaled);
}

int ac_drm_cs_syncobj_query2(int device_fd, uint32_t *handles, uint64_t *points,
                             unsigned num_handles, uint32_t flags)
{
   return drmSyncobjQuery2(device_fd, handles, points, num_handles, flags);
}

int ac_drm_cs_import_syncobj(int device_fd, int shared_fd, uint32_t *handle)
{
   return drmSyncobjFDToHandle(device_fd, shared_fd, handle);
}

int ac_drm_cs_syncobj_export_sync_file(int device_fd, uint32_t syncobj, int *sync_file_fd)
{
   return drmSyncobjExportSyncFile(device_fd, syncobj, sync_file_fd);
}

int ac_drm_cs_syncobj_import_sync_file(int device_fd, uint32_t syncobj, int sync_file_fd)
{
   return drmSyncobjImportSyncFile(device_fd, syncobj, sync_file_fd);
}

int ac_drm_cs_syncobj_export_sync_file2(int device_fd, uint32_t syncobj, uint64_t point,
                                        uint32_t flags, int *sync_file_fd)
{
   uint32_t binary_handle;
   int ret;

   if (!point)
      return drmSyncobjExportSyncFile(device_fd, syncobj, sync_file_fd);

   ret = drmSyncobjCreate(device_fd, 0, &binary_handle);
   if (ret)
      return ret;

   ret = drmSyncobjTransfer(device_fd, binary_handle, 0, syncobj, point, flags);
   if (ret)
      goto out;
   ret = drmSyncobjExportSyncFile(device_fd, binary_handle, sync_file_fd);
out:
   drmSyncobjDestroy(device_fd, binary_handle);
   return ret;
}

int ac_drm_cs_syncobj_transfer(int device_fd, uint32_t dst_handle, uint64_t dst_point,
                               uint32_t src_handle, uint64_t src_point, uint32_t flags)
{
   return drmSyncobjTransfer(device_fd, dst_handle, dst_point, src_handle, src_point, flags);
}

int ac_drm_cs_submit_raw2(int device_fd, uint32_t ctx_handle, uint32_t bo_list_handle,
                          int num_chunks, struct drm_amdgpu_cs_chunk *chunks, uint64_t *seq_no)
{
   union drm_amdgpu_cs cs;
   uint64_t *chunk_array;
   int i, r;

   memset(&cs, 0, sizeof(cs));
   chunk_array = alloca(sizeof(uint64_t) * num_chunks);
   for (i = 0; i < num_chunks; i++)
      chunk_array[i] = (uint64_t)(uintptr_t)&chunks[i];
   cs.in.chunks = (uint64_t)(uintptr_t)chunk_array;
   cs.in.ctx_id = ctx_handle;
   cs.in.bo_list_handle = bo_list_handle;
   cs.in.num_chunks = num_chunks;
   r = drm_ioctl_write_read(device_fd, DRM_AMDGPU_CS, &cs, sizeof(cs));
   if (!r && seq_no)
      *seq_no = cs.out.handle;
   return r;
}

void ac_drm_cs_chunk_fence_info_to_data(uint32_t bo_handle, uint64_t offset,
                                        struct drm_amdgpu_cs_chunk_data *data)
{
   data->fence_data.handle = bo_handle;
   data->fence_data.offset = offset * sizeof(uint64_t);
}

int ac_drm_query_info(int device_fd, unsigned info_id, unsigned size, void *value)
{
   struct drm_amdgpu_info request;

   memset(&request, 0, sizeof(request));
   request.return_pointer = (uintptr_t)value;
   request.return_size = size;
   request.query = info_id;

   return drm_ioctl_write(device_fd, DRM_AMDGPU_INFO, &request, sizeof(struct drm_amdgpu_info));
}

int ac_drm_read_mm_registers(int device_fd, unsigned dword_offset, unsigned count,
                             uint32_t instance, uint32_t flags, uint32_t *values)
{
   struct drm_amdgpu_info request;

   memset(&request, 0, sizeof(request));
   request.return_pointer = (uintptr_t)values;
   request.return_size = count * sizeof(uint32_t);
   request.query = AMDGPU_INFO_READ_MMR_REG;
   request.read_mmr_reg.dword_offset = dword_offset;
   request.read_mmr_reg.count = count;
   request.read_mmr_reg.instance = instance;
   request.read_mmr_reg.flags = flags;

   return drm_ioctl_write(device_fd, DRM_AMDGPU_INFO, &request, sizeof(struct drm_amdgpu_info));
}

int ac_drm_query_hw_ip_count(int device_fd, unsigned type, uint32_t *count)
{
   struct drm_amdgpu_info request;

   memset(&request, 0, sizeof(request));
   request.return_pointer = (uintptr_t)count;
   request.return_size = sizeof(*count);
   request.query = AMDGPU_INFO_HW_IP_COUNT;
   request.query_hw_ip.type = type;

   return drm_ioctl_write(device_fd, DRM_AMDGPU_INFO, &request, sizeof(struct drm_amdgpu_info));
}

int ac_drm_query_hw_ip_info(int device_fd, unsigned type, unsigned ip_instance,
                            struct drm_amdgpu_info_hw_ip *info)
{
   struct drm_amdgpu_info request;

   memset(&request, 0, sizeof(request));
   request.return_pointer = (uintptr_t)info;
   request.return_size = sizeof(*info);
   request.query = AMDGPU_INFO_HW_IP_INFO;
   request.query_hw_ip.type = type;
   request.query_hw_ip.ip_instance = ip_instance;

   return drm_ioctl_write(device_fd, DRM_AMDGPU_INFO, &request, sizeof(struct drm_amdgpu_info));
}

int ac_drm_query_firmware_version(int device_fd, unsigned fw_type, unsigned ip_instance,
                                  unsigned index, uint32_t *version, uint32_t *feature)
{
   struct drm_amdgpu_info request;
   struct drm_amdgpu_info_firmware firmware = {};
   int r;

   memset(&request, 0, sizeof(request));
   request.return_pointer = (uintptr_t)&firmware;
   request.return_size = sizeof(firmware);
   request.query = AMDGPU_INFO_FW_VERSION;
   request.query_fw.fw_type = fw_type;
   request.query_fw.ip_instance = ip_instance;
   request.query_fw.index = index;

   r = drm_ioctl_write(device_fd, DRM_AMDGPU_INFO, &request, sizeof(struct drm_amdgpu_info));
   if (r)
      return r;

   *version = firmware.ver;
   *feature = firmware.feature;
   return 0;
}

int ac_drm_query_uq_fw_area_info(int device_fd, unsigned type, unsigned ip_instance,
                                 struct drm_amdgpu_info_uq_fw_areas *info)
{
   struct drm_amdgpu_info request;

   memset(&request, 0, sizeof(request));
   request.return_pointer = (uintptr_t)info;
   request.return_size = sizeof(*info);
   request.query = AMDGPU_INFO_UQ_FW_AREAS;
   request.query_hw_ip.type = type;
   request.query_hw_ip.ip_instance = ip_instance;

   return drm_ioctl_write(device_fd, DRM_AMDGPU_INFO, &request, sizeof(struct drm_amdgpu_info));
}

int ac_drm_query_gpu_info(int device_fd, struct amdgpu_gpu_info *info)
{
   struct drm_amdgpu_info_device dev_info = {0};
   int r, i;

   r = ac_drm_query_info(device_fd, AMDGPU_INFO_DEV_INFO, sizeof(dev_info), &dev_info);
   if (r)
      return r;

   memset(info, 0, sizeof(*info));
   info->asic_id = dev_info.device_id;
   info->chip_rev = dev_info.chip_rev;
   info->chip_external_rev = dev_info.external_rev;
   info->family_id = dev_info.family;
   info->max_engine_clk = dev_info.max_engine_clock;
   info->max_memory_clk = dev_info.max_memory_clock;
   info->gpu_counter_freq = dev_info.gpu_counter_freq;
   info->enabled_rb_pipes_mask = dev_info.enabled_rb_pipes_mask;
   info->rb_pipes = dev_info.num_rb_pipes;
   info->ids_flags = dev_info.ids_flags;
   info->num_hw_gfx_contexts = dev_info.num_hw_gfx_contexts;
   info->num_shader_engines = dev_info.num_shader_engines;
   info->num_shader_arrays_per_engine = dev_info.num_shader_arrays_per_engine;
   info->vram_type = dev_info.vram_type;
   info->vram_bit_width = dev_info.vram_bit_width;
   info->ce_ram_size = dev_info.ce_ram_size;
   info->vce_harvest_config = dev_info.vce_harvest_config;
   info->pci_rev_id = dev_info.pci_rev;

   if (info->family_id < AMDGPU_FAMILY_AI) {
      for (i = 0; i < (int)info->num_shader_engines; i++) {
         unsigned instance = (i << AMDGPU_INFO_MMR_SE_INDEX_SHIFT) |
                             (AMDGPU_INFO_MMR_SH_INDEX_MASK << AMDGPU_INFO_MMR_SH_INDEX_SHIFT);

         r = ac_drm_read_mm_registers(device_fd, 0x263d, 1, instance, 0, &info->backend_disable[i]);
         if (r)
            return r;
         /* extract bitfield CC_RB_BACKEND_DISABLE.BACKEND_DISABLE */
         info->backend_disable[i] = (info->backend_disable[i] >> 16) & 0xff;

         r =
            ac_drm_read_mm_registers(device_fd, 0xa0d4, 1, instance, 0, &info->pa_sc_raster_cfg[i]);
         if (r)
            return r;

         if (info->family_id >= AMDGPU_FAMILY_CI) {
            r = ac_drm_read_mm_registers(device_fd, 0xa0d5, 1, instance, 0,
                                         &info->pa_sc_raster_cfg1[i]);
            if (r)
               return r;
         }
      }
   }

   r = ac_drm_read_mm_registers(device_fd, 0x263e, 1, 0xffffffff, 0, &info->gb_addr_cfg);
   if (r)
      return r;

   if (info->family_id < AMDGPU_FAMILY_AI) {
      r = ac_drm_read_mm_registers(device_fd, 0x2644, 32, 0xffffffff, 0, info->gb_tile_mode);
      if (r)
         return r;

      if (info->family_id >= AMDGPU_FAMILY_CI) {
         r = ac_drm_read_mm_registers(device_fd, 0x2664, 16, 0xffffffff, 0,
                                      info->gb_macro_tile_mode);
         if (r)
            return r;
      }

      r = ac_drm_read_mm_registers(device_fd, 0x9d8, 1, 0xffffffff, 0, &info->mc_arb_ramcfg);
      if (r)
         return r;
   }

   info->cu_active_number = dev_info.cu_active_number;
   info->cu_ao_mask = dev_info.cu_ao_mask;
   memcpy(&info->cu_bitmap[0][0], &dev_info.cu_bitmap[0][0], sizeof(info->cu_bitmap));
   return 0;
}

int ac_drm_query_heap_info(int device_fd, uint32_t heap, uint32_t flags,
                           struct amdgpu_heap_info *info)
{
   struct drm_amdgpu_info_vram_gtt vram_gtt_info = {};
   int r;

   r = ac_drm_query_info(device_fd, AMDGPU_INFO_VRAM_GTT, sizeof(vram_gtt_info), &vram_gtt_info);
   if (r)
      return r;

   /* Get heap information */
   switch (heap) {
   case AMDGPU_GEM_DOMAIN_VRAM:
      /* query visible only vram heap */
      if (flags & AMDGPU_GEM_CREATE_CPU_ACCESS_REQUIRED)
         info->heap_size = vram_gtt_info.vram_cpu_accessible_size;
      else /* query total vram heap */
         info->heap_size = vram_gtt_info.vram_size;

      info->max_allocation = vram_gtt_info.vram_cpu_accessible_size;

      if (flags & AMDGPU_GEM_CREATE_CPU_ACCESS_REQUIRED)
         r = ac_drm_query_info(device_fd, AMDGPU_INFO_VIS_VRAM_USAGE, sizeof(info->heap_usage),
                               &info->heap_usage);
      else
         r = ac_drm_query_info(device_fd, AMDGPU_INFO_VRAM_USAGE, sizeof(info->heap_usage),
                               &info->heap_usage);
      if (r)
         return r;
      break;
   case AMDGPU_GEM_DOMAIN_GTT:
      info->heap_size = vram_gtt_info.gtt_size;
      info->max_allocation = vram_gtt_info.vram_cpu_accessible_size;

      r = ac_drm_query_info(device_fd, AMDGPU_INFO_GTT_USAGE, sizeof(info->heap_usage),
                            &info->heap_usage);
      if (r)
         return r;
      break;
   default:
      return -EINVAL;
   }

   return 0;
}

int ac_drm_query_sensor_info(int device_fd, unsigned sensor_type, unsigned size, void *value)
{
   struct drm_amdgpu_info request;

   memset(&request, 0, sizeof(request));
   request.return_pointer = (uintptr_t)value;
   request.return_size = size;
   request.query = AMDGPU_INFO_SENSOR;
   request.sensor_info.type = sensor_type;

   return drm_ioctl_write(device_fd, DRM_AMDGPU_INFO, &request, sizeof(struct drm_amdgpu_info));
}

int ac_drm_query_video_caps_info(int device_fd, unsigned cap_type, unsigned size, void *value)
{
   struct drm_amdgpu_info request;

   memset(&request, 0, sizeof(request));
   request.return_pointer = (uintptr_t)value;
   request.return_size = size;
   request.query = AMDGPU_INFO_VIDEO_CAPS;
   request.sensor_info.type = cap_type;

   return drm_ioctl_write(device_fd, DRM_AMDGPU_INFO, &request, sizeof(struct drm_amdgpu_info));
}

int ac_drm_vm_reserve_vmid(int device_fd, uint32_t flags)
{
   union drm_amdgpu_vm vm;

   vm.in.op = AMDGPU_VM_OP_RESERVE_VMID;
   vm.in.flags = flags;

   return drm_ioctl_write_read(device_fd, DRM_AMDGPU_VM, &vm, sizeof(vm));
}

int ac_drm_vm_unreserve_vmid(int device_fd, uint32_t flags)
{
   union drm_amdgpu_vm vm;

   vm.in.op = AMDGPU_VM_OP_UNRESERVE_VMID;
   vm.in.flags = flags;

   return drm_ioctl_write_read(device_fd, DRM_AMDGPU_VM, &vm, sizeof(vm));
}

int ac_drm_create_userqueue(int device_fd, uint32_t ip_type, uint32_t doorbell_handle,
                            uint32_t doorbell_offset, uint64_t queue_va, uint64_t queue_size,
                            uint64_t wptr_va, uint64_t rptr_va, void *mqd_in, uint32_t *queue_id)
{
   int ret;
   union drm_amdgpu_userq userq;
   uint64_t mqd_size;

   switch (ip_type) {
   case AMDGPU_HW_IP_GFX:
      mqd_size = sizeof(struct drm_amdgpu_userq_mqd_gfx11);
      break;
   case AMDGPU_HW_IP_DMA:
      mqd_size = sizeof(struct drm_amdgpu_userq_mqd_sdma_gfx11);
      break;
   case AMDGPU_HW_IP_COMPUTE:
      mqd_size = sizeof(struct drm_amdgpu_userq_mqd_compute_gfx11);
      break;
      default:
      return -EINVAL;
   }

   memset(&userq, 0, sizeof(userq));

   userq.in.op = AMDGPU_USERQ_OP_CREATE;
   userq.in.ip_type = ip_type;

   userq.in.doorbell_handle = doorbell_handle;
   userq.in.doorbell_offset = doorbell_offset;

   userq.in.queue_va = queue_va;
   userq.in.queue_size = queue_size;
   userq.in.wptr_va = wptr_va;
   userq.in.rptr_va = rptr_va;

   userq.in.mqd = (uintptr_t)mqd_in;
   userq.in.mqd_size = mqd_size;

   ret = drm_ioctl_write_read(device_fd, DRM_AMDGPU_USERQ,
                              &userq, sizeof(userq));
   *queue_id = userq.out.queue_id;

   return ret;
}

int ac_drm_free_userqueue(int device_fd, uint32_t queue_id)
{
   union drm_amdgpu_userq userq;

   memset(&userq, 0, sizeof(userq));
   userq.in.op = AMDGPU_USERQ_OP_FREE;
   userq.in.queue_id = queue_id;

   return drm_ioctl_write_read(device_fd, DRM_AMDGPU_USERQ, &userq, sizeof(userq));
}

int ac_drm_userq_signal(int device_fd, struct drm_amdgpu_userq_signal *signal_data)
{
   return drm_ioctl_write_read(device_fd, DRM_AMDGPU_USERQ_SIGNAL,
                               signal_data, sizeof(struct drm_amdgpu_userq_signal));
}

int ac_drm_userq_wait(int device_fd, struct drm_amdgpu_userq_wait *wait_data)
{
   return drm_ioctl_write_read(device_fd, DRM_AMDGPU_USERQ_WAIT, wait_data,
                               sizeof(struct drm_amdgpu_userq_wait));
}
