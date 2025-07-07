// Copyright 2025 Red Hat.
// SPDX-License-Identifier: MIT

use {
    crate::{
        api::{
            event::create_and_queue,
            icd::{ArcedCLObject, BaseCLObject, CLResult, ReferenceCountedAPIPointer},
            util::{event_list_from_cl, CLInfo, CLInfoRes, CLInfoValue},
        },
        core::{context::Context, device::Device, queue::Queue, semaphore::Semaphore},
    },
    mesa_rust_util::{conversion::TryIntoWithErr, properties::MultiValProperties},
    rusticl_opencl_gen::*,
    rusticl_proc_macros::{cl_entrypoint, cl_info_entrypoint},
    std::{
        ffi::{c_int, c_void},
        sync::Arc,
    },
};

#[cl_info_entrypoint(clGetSemaphoreInfoKHR)]
unsafe impl CLInfo<cl_semaphore_info_khr> for cl_semaphore_khr {
    fn query(&self, q: cl_semaphore_info_khr, v: CLInfoValue) -> CLResult<CLInfoRes> {
        let sema = Semaphore::ref_from_raw(*self)?;

        match q {
            CL_SEMAPHORE_CONTEXT_KHR => {
                v.write::<cl_context>(cl_context::from_ptr(Arc::as_ptr(&sema.ctx)))
            }
            CL_SEMAPHORE_DEVICE_HANDLE_LIST_KHR => {
                v.write::<&[cl_device_id]>(&[cl_device_id::from_ptr(sema.dev)])
            }
            CL_SEMAPHORE_EXPORT_HANDLE_TYPES_KHR => {
                // In reality it's a list, but we are not supporting more than one anyway, so Option
                // is fine here.
                v.write::<Option<cl_external_semaphore_handle_type_khr>>(None)
            }
            CL_SEMAPHORE_EXPORTABLE_KHR => v.write::<cl_bool>(CL_FALSE),
            CL_SEMAPHORE_PAYLOAD_KHR => {
                v.write::<cl_semaphore_payload_khr>(sema.is_signalled().into())
            }
            CL_SEMAPHORE_PROPERTIES_KHR => {
                v.write::<&MultiValProperties<cl_semaphore_properties_khr>>(&sema.props)
            }
            CL_SEMAPHORE_REFERENCE_COUNT_KHR => v.write::<cl_uint>(Semaphore::refcnt(*self)?),
            CL_SEMAPHORE_TYPE_KHR => v.write::<cl_semaphore_type_khr>(CL_SEMAPHORE_TYPE_BINARY_KHR),
            _ => Err(CL_INVALID_VALUE),
        }
    }
}

#[cl_entrypoint(clCreateSemaphoreWithPropertiesKHR)]
fn create_semaphore(
    context: cl_context,
    sema_props: *const cl_semaphore_properties_khr,
) -> CLResult<cl_semaphore_khr> {
    let context = Context::arc_from_raw(context)?;

    // CL_INVALID_VALUE if sema_props is NULL
    if sema_props.is_null() {
        return Err(CL_INVALID_VALUE);
    }

    let mut sema_type = 0;
    let mut dev = None;
    let sema_props = unsafe {
        MultiValProperties::new(
            sema_props,
            &[
                CL_SEMAPHORE_DEVICE_HANDLE_LIST_KHR.into(),
                CL_SEMAPHORE_EXPORT_HANDLE_TYPES_KHR.into(),
            ],
        )
        // CL_INVALID_PROPERTY [..] if the same property name is specified more than once.
        .ok_or(CL_INVALID_PROPERTY)?
    };

    for (key, vals) in sema_props.iter() {
        // CL_INVALID_PROPERTY if a property name in sema_props is not a supported property name
        match u32::try_from(key).or(Err(CL_INVALID_PROPERTY))? {
            CL_SEMAPHORE_DEVICE_HANDLE_LIST_KHR => {
                // CL_INVALID_DEVICE if CL_SEMAPHORE_DEVICE_HANDLE_LIST_KHR is specified as part of
                // sema_props, but it does not identify exactly one valid device;
                let Some((&dev_in, &[])) = vals.split_first() else {
                    return Err(CL_INVALID_DEVICE);
                };

                let dev_in = Device::ref_from_raw(dev_in as _)?
                    .to_static()
                    .ok_or(CL_INVALID_DEVICE)?;

                // CL_INVALID_DEVICE [..] if a device identified by
                // CL_SEMAPHORE_DEVICE_HANDLE_LIST_KHR is not one of the devices within context.
                if !context.devs.contains(&dev_in) {
                    return Err(CL_INVALID_DEVICE);
                }

                dev = Some(dev_in);
            }
            CL_SEMAPHORE_EXPORT_HANDLE_TYPES_KHR => {
                // CL_INVALID_VALUE if more than one semaphore handle type is specified in the
                // CL_SEMAPHORE_EXPORT_HANDLE_TYPES_KHR list.
                if vals.len() > 1 {
                    return Err(CL_INVALID_VALUE);
                }
            }
            CL_SEMAPHORE_TYPE_KHR => {
                // CL_INVALID_PROPERTY [..] if the value specified for a supported property name is
                // not valid
                sema_type = vals[0].try_into_with_err(CL_INVALID_PROPERTY)?;
                if sema_type != CL_SEMAPHORE_TYPE_BINARY_KHR {
                    return Err(CL_INVALID_PROPERTY);
                }
            }
            // CL_INVALID_PROPERTY if a property name in sema_props is not a supported property name
            _ => return Err(CL_INVALID_PROPERTY),
        }
    }

    let dev = match dev {
        Some(dev) => dev,
        None => {
            // CL_INVALID_PROPERTY [..] Additionally, if context is a multiple device context and
            // sema_props does not specify CL_SEMAPHORE_DEVICE_HANDLE_LIST_KHR.
            let Some((dev_from_ctx, &[])) = context.devs.split_first() else {
                return Err(CL_INVALID_PROPERTY);
            };

            // If CL_SEMAPHORE_DEVICE_HANDLE_LIST_KHR is not specified as part of sema_props, the
            // semaphore object created by clCreateSemaphoreWithPropertiesKHR is by default
            // associated with all devices in the context.
            dev_from_ctx
        }
    };

    // CL_INVALID_VALUE [..] if sema_props do not specify <property, value> pairs for minimum set of
    // properties (i.e. CL_SEMAPHORE_TYPE_KHR) required for successful creation of a
    // cl_semaphore_khr
    if sema_type == 0 {
        return Err(CL_INVALID_VALUE);
    }

    Ok(Semaphore::new(context, sema_props, dev)?.into_cl())

    // CL_INVALID_DEVICE if one or more devices identified by properties CL_SEMAPHORE_DEVICE_HANDLE_LIST_KHR cannot import the requested external semaphore handle type.
    // CL_INVALID_OPERATION If props_list specifies a cl_external_semaphore_handle_type_khr followed by a handle as well as CL_SEMAPHORE_EXPORT_HANDLE_TYPES_KHR. Exporting a semaphore handle from a semaphore that was created by importing an external semaphore handle is not permitted.
    // CL_INVALID_PROPERTY if sema_props includes more than one external semaphore handle.
}

#[cl_entrypoint(clEnqueueSignalSemaphoresKHR)]
fn enqueue_signal_semaphores(
    command_queue: cl_command_queue,
    num_sema_objects: cl_uint,
    sema_objects: *const cl_semaphore_khr,
    _sema_payload_list: *const cl_semaphore_payload_khr,
    num_events_in_wait_list: cl_uint,
    event_wait_list: *const cl_event,
    event: *mut cl_event,
) -> CLResult<()> {
    let q = Queue::arc_from_raw(command_queue)?;
    let evs = event_list_from_cl(&q, num_events_in_wait_list, event_wait_list)?;

    // CL_INVALID_VALUE if num_sema_objects is 0.
    if num_sema_objects == 0 {
        return Err(CL_INVALID_VALUE);
    }

    // CL_INVALID_SEMAPHORE_KHR if any of the semaphore objects specified by sema_objects is not
    // valid.
    let semas = Semaphore::arcs_from_arr(sema_objects, num_sema_objects)?;

    // CL_INVALID_CONTEXT if the context associated with command_queue and any of the semaphore
    // objects in sema_objects are not the same
    if semas.iter().any(|sema| sema.ctx != q.context) {
        return Err(CL_INVALID_CONTEXT);
    }

    // CL_INVALID_COMMAND_QUEUE [..] if the device associated with command_queue is not same as one
    // of the devices specified by CL_SEMAPHORE_DEVICE_HANDLE_LIST_KHR at the time of creating one
    // or more of sema_objects, or if one or more of sema_objects belong to a context that does not
    // contain a device associated with command_queue.
    for sema in &semas {
        if q.device != sema.dev || !sema.ctx.devs.contains(&q.device) {
            return Err(CL_INVALID_COMMAND_QUEUE);
        }
    }

    create_and_queue(
        q,
        CL_COMMAND_SEMAPHORE_SIGNAL_KHR,
        evs,
        event,
        false,
        Semaphore::gpu_signal(semas),
    )

    // CL_INVALID_VALUE if any of the semaphore objects specified by sema_objects requires a semaphore payload and sema_payload_list is NULL.
}

#[cl_entrypoint(clEnqueueWaitSemaphoresKHR)]
fn enqueue_wait_semaphores(
    command_queue: cl_command_queue,
    num_sema_objects: cl_uint,
    sema_objects: *const cl_semaphore_khr,
    _sema_payload_list: *const cl_semaphore_payload_khr,
    num_events_in_wait_list: cl_uint,
    event_wait_list: *const cl_event,
    event: *mut cl_event,
) -> CLResult<()> {
    let q = Queue::arc_from_raw(command_queue)?;
    let evs = event_list_from_cl(&q, num_events_in_wait_list, event_wait_list)?;

    // CL_INVALID_VALUE if num_sema_objects is 0.
    if num_sema_objects == 0 {
        return Err(CL_INVALID_VALUE);
    }

    // CL_INVALID_SEMAPHORE_KHR if any of the semaphore objects specified by sema_objects is not
    // valid.
    let semas = Semaphore::arcs_from_arr(sema_objects, num_sema_objects)?;

    // CL_INVALID_CONTEXT if the context associated with command_queue and any of the semaphore
    // objects in sema_objects are not the same
    if semas.iter().any(|sema| sema.ctx != q.context) {
        return Err(CL_INVALID_CONTEXT);
    }

    // CL_INVALID_COMMAND_QUEUE [..] if the device associated with command_queue is not same as one
    // of the devices specified by CL_SEMAPHORE_DEVICE_HANDLE_LIST_KHR at the time of creating one
    // or more of sema_objects, or if one or more of sema_objects belong to a context that does not
    // contain a device associated with command_queue.
    for sema in &semas {
        if q.device != sema.dev || !sema.ctx.devs.contains(&q.device) {
            return Err(CL_INVALID_COMMAND_QUEUE);
        }
    }

    create_and_queue(
        q,
        CL_COMMAND_SEMAPHORE_WAIT_KHR,
        evs,
        event,
        false,
        Box::new(|_, ctx| {
            for sema in semas {
                sema.gpu_wait(ctx)?;
            }
            Ok(())
        }),
    )

    // CL_INVALID_VALUE if any of the semaphore objects specified by sema_objects requires a semaphore payload and sema_payload_list is NULL.
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
fn release_semaphore(sema_object: cl_semaphore_khr) -> CLResult<()> {
    Semaphore::release(sema_object)
}

#[cl_entrypoint(clRetainSemaphoreKHR)]
fn retain_semaphore(sema_object: cl_semaphore_khr) -> CLResult<()> {
    Semaphore::retain(sema_object)
}

#[cl_entrypoint(clReImportSemaphoreSyncFdKHR)]
fn re_import_semaphore_sync_fd(
    _sema_object: cl_semaphore_khr,
    _reimport_props: *mut cl_semaphore_reimport_properties_khr,
    _fd: c_int,
) -> CLResult<()> {
    Err(CL_INVALID_OPERATION)
}
