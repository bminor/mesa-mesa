// Copyright 2025 Red Hat.
// SPDX-License-Identifier: MIT

use {
    crate::api::{
        icd::CLResult,
        util::{CLInfo, CLInfoRes, CLInfoValue},
    },
    rusticl_opencl_gen::*,
    rusticl_proc_macros::{cl_entrypoint, cl_info_entrypoint},
    std::ffi::{c_int, c_void},
};

#[cl_info_entrypoint(clGetSemaphoreInfoKHR)]
unsafe impl CLInfo<cl_semaphore_info_khr> for cl_semaphore_khr {
    fn query(&self, _q: cl_semaphore_info_khr, _v: CLInfoValue) -> CLResult<CLInfoRes> {
        Err(CL_INVALID_OPERATION)
    }
}

#[cl_entrypoint(clCreateSemaphoreWithPropertiesKHR)]
fn create_semaphore(
    _context: cl_context,
    _sema_props: *const cl_semaphore_properties_khr,
) -> CLResult<cl_semaphore_khr> {
    Err(CL_INVALID_OPERATION)
}

#[cl_entrypoint(clEnqueueSignalSemaphoresKHR)]
fn enqueue_signal_semaphores(
    _command_queue: cl_command_queue,
    _num_sema_objects: cl_uint,
    _sema_objects: *const cl_semaphore_khr,
    _sema_payload_list: *const cl_semaphore_payload_khr,
    _num_events_in_wait_list: cl_uint,
    _event_wait_list: *const cl_event,
    _event: *mut cl_event,
) -> CLResult<()> {
    Err(CL_INVALID_OPERATION)
}

#[cl_entrypoint(clEnqueueWaitSemaphoresKHR)]
fn enqueue_wait_semaphores(
    _command_queue: cl_command_queue,
    _num_sema_objects: cl_uint,
    _sema_objects: *const cl_semaphore_khr,
    _sema_payload_list: *const cl_semaphore_payload_khr,
    _num_events_in_wait_list: cl_uint,
    _event_wait_list: *const cl_event,
    _event: *mut cl_event,
) -> CLResult<()> {
    Err(CL_INVALID_OPERATION)
}

#[cl_entrypoint(clGetSemaphoreHandleForTypeKHR)]
fn get_semaphore_handle_for_type(
    _sema_object: cl_semaphore_khr,
    _device: cl_device_id,
    _handle_type: cl_external_semaphore_handle_type_khr,
    _handle_size: usize,
    _handle_ptr: *mut c_void,
    _handle_size_ret: *mut usize,
) -> CLResult<()> {
    Err(CL_INVALID_OPERATION)
}

#[cl_entrypoint(clReleaseSemaphoreKHR)]
fn release_semaphore(_sema_object: cl_semaphore_khr) -> CLResult<()> {
    Err(CL_INVALID_OPERATION)
}

#[cl_entrypoint(clRetainSemaphoreKHR)]
fn retain_semaphore(_sema_object: cl_semaphore_khr) -> CLResult<()> {
    Err(CL_INVALID_OPERATION)
}

#[cl_entrypoint(clReImportSemaphoreSyncFdKHR)]
fn re_import_semaphore_sync_fd(
    _sema_object: cl_semaphore_khr,
    _reimport_props: *mut cl_semaphore_reimport_properties_khr,
    _fd: c_int,
) -> CLResult<()> {
    Err(CL_INVALID_OPERATION)
}
