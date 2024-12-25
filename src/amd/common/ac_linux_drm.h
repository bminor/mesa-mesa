/*
 * Copyright 2024 Advanced Micro Devices, Inc.
 * SPDX-License-Identifier: MIT
 */

#ifndef AC_LINUX_DRM_H
#define AC_LINUX_DRM_H

#include <stdbool.h>
#include <stdint.h>

#ifndef _WIN32
#include "drm-uapi/amdgpu_drm.h"
#include "amdgpu.h"
#endif

#ifdef __cplusplus
extern "C" {
#endif

/* All functions are static inline stubs on Windows. */
#ifdef _WIN32
#define PROC static inline
#define TAIL                                                                                       \
   {                                                                                               \
      return -1;                                                                                   \
   }
#define TAILV                                                                                      \
   {                                                                                               \
   }
#else
#define PROC
#define TAIL
#define TAILV
#endif

PROC int ac_drm_bo_set_metadata(int device_fd, uint32_t bo_handle,
                                struct amdgpu_bo_metadata *info) TAIL;
PROC int ac_drm_bo_query_info(int device_fd, uint32_t bo_handle, struct amdgpu_bo_info *info) TAIL;
PROC int ac_drm_bo_wait_for_idle(int device_fd, uint32_t bo_handle, uint64_t timeout_ns,
                                 bool *busy) TAIL;
PROC int ac_drm_bo_va_op(int device_fd, uint32_t bo_handle, uint64_t offset, uint64_t size,
                         uint64_t addr, uint64_t flags, uint32_t ops) TAIL;
PROC int ac_drm_bo_va_op_raw(int device_fd, uint32_t bo_handle, uint64_t offset, uint64_t size,
                             uint64_t addr, uint64_t flags, uint32_t ops) TAIL;
PROC int ac_drm_bo_va_op_raw2(int device_fd, uint32_t bo_handle, uint64_t offset, uint64_t size,
                              uint64_t addr, uint64_t flags, uint32_t ops,
                              uint32_t vm_timeline_syncobj_out, uint64_t vm_timeline_point,
                              uint64_t input_fence_syncobj_handles,
                              uint32_t num_syncobj_handles) TAIL;
PROC int ac_drm_cs_ctx_create2(int device_fd, uint32_t priority, uint32_t *ctx_handle) TAIL;
PROC int ac_drm_cs_ctx_free(int device_fd, uint32_t ctx_handle) TAIL;
PROC int ac_drm_cs_ctx_stable_pstate(int device_fd, uint32_t ctx_handle, uint32_t op,
                                     uint32_t flags, uint32_t *out_flags) TAIL;
PROC int ac_drm_cs_query_reset_state2(int device_fd, uint32_t ctx_handle, uint64_t *flags) TAIL;
PROC int ac_drm_cs_query_fence_status(int device_fd, uint32_t ctx_handle, uint32_t ip_type,
                                      uint32_t ip_instance, uint32_t ring, uint64_t fence_seq_no,
                                      uint64_t timeout_ns, uint64_t flags, uint32_t *expired) TAIL;
PROC int ac_drm_cs_create_syncobj2(int device_fd, uint32_t flags, uint32_t *handle) TAIL;
PROC int ac_drm_cs_create_syncobj(int device_fd, uint32_t *handle) TAIL;
PROC int ac_drm_cs_destroy_syncobj(int device_fd, uint32_t handle) TAIL;
PROC int ac_drm_cs_syncobj_wait(int device_fd, uint32_t *handles, unsigned num_handles,
                                int64_t timeout_nsec, unsigned flags,
                                uint32_t *first_signaled) TAIL;
PROC int ac_drm_cs_syncobj_query2(int device_fd, uint32_t *handles, uint64_t *points,
                                  unsigned num_handles, uint32_t flags) TAIL;
PROC int ac_drm_cs_import_syncobj(int device_fd, int shared_fd, uint32_t *handle) TAIL;
PROC int ac_drm_cs_syncobj_export_sync_file(int device_fd, uint32_t syncobj,
                                            int *sync_file_fd) TAIL;
PROC int ac_drm_cs_syncobj_import_sync_file(int device_fd, uint32_t syncobj, int sync_file_fd) TAIL;
PROC int ac_drm_cs_syncobj_export_sync_file2(int device_fd, uint32_t syncobj, uint64_t point,
                                             uint32_t flags, int *sync_file_fd) TAIL;
PROC int ac_drm_cs_syncobj_transfer(int device_fd, uint32_t dst_handle, uint64_t dst_point,
                                    uint32_t src_handle, uint64_t src_point, uint32_t flags) TAIL;
PROC int ac_drm_cs_submit_raw2(int device_fd, uint32_t ctx_handle, uint32_t bo_list_handle,
                               int num_chunks, struct drm_amdgpu_cs_chunk *chunks,
                               uint64_t *seq_no) TAIL;
PROC void ac_drm_cs_chunk_fence_info_to_data(uint32_t bo_handle, uint64_t offset,
                                             struct drm_amdgpu_cs_chunk_data *data) TAILV;
PROC int ac_drm_query_info(int device_fd, unsigned info_id, unsigned size, void *value) TAIL;
PROC int ac_drm_read_mm_registers(int device_fd, unsigned dword_offset, unsigned count,
                                  uint32_t instance, uint32_t flags, uint32_t *values) TAIL;
PROC int ac_drm_query_hw_ip_count(int device_fd, unsigned type, uint32_t *count) TAIL;
PROC int ac_drm_query_hw_ip_info(int device_fd, unsigned type, unsigned ip_instance,
                                 struct drm_amdgpu_info_hw_ip *info) TAIL;
PROC int ac_drm_query_firmware_version(int device_fd, unsigned fw_type, unsigned ip_instance,
                                       unsigned index, uint32_t *version, uint32_t *feature) TAIL;
PROC int ac_drm_query_uq_fw_area_info(int device_fd, unsigned type, unsigned ip_instance,
                                      struct drm_amdgpu_info_uq_fw_areas *info) TAIL;
PROC int ac_drm_query_gpu_info(int device_fd, struct amdgpu_gpu_info *info) TAIL;
PROC int ac_drm_query_heap_info(int device_fd, uint32_t heap, uint32_t flags,
                                struct amdgpu_heap_info *info) TAIL;
PROC int ac_drm_query_sensor_info(int device_fd, unsigned sensor_type, unsigned size,
                                  void *value) TAIL;
PROC int ac_drm_query_video_caps_info(int device_fd, unsigned cap_type, unsigned size,
                                      void *value) TAIL;
PROC int ac_drm_vm_reserve_vmid(int device_fd, uint32_t flags) TAIL;
PROC int ac_drm_vm_unreserve_vmid(int device_fd, uint32_t flags) TAIL;
PROC int ac_drm_create_userqueue(int device_fd, uint32_t ip_type, uint32_t doorbell_handle,
                                 uint32_t doorbell_offset, uint64_t queue_va, uint64_t queue_size,
                                 uint64_t wptr_va, uint64_t rptr_va, void *mqd_in,
                                 uint32_t *queue_id) TAIL;
PROC int ac_drm_free_userqueue(int device_fd, uint32_t queue_id) TAIL;
PROC int ac_drm_userq_signal(int device_fd, struct drm_amdgpu_userq_signal *signal_data) TAIL;
PROC int ac_drm_userq_wait(int device_fd, struct drm_amdgpu_userq_wait *wait_data) TAIL;

#ifdef __cplusplus
}
#endif

#endif
