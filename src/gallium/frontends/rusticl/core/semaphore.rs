// Copyright 2025 Red Hat.
// SPDX-License-Identifier: MIT

use {
    crate::{
        api::icd::{CLObjectBase, CLResult, RusticlTypes},
        core::{context::Context, device::Device, event::EventSig},
        impl_cl_type_trait,
    },
    mesa_rust::pipe::{context::PipeContext, fence::PipeFence},
    mesa_rust_util::properties::MultiValProperties,
    rusticl_opencl_gen::*,
    std::sync::{Arc, Condvar, Mutex, MutexGuard},
};

struct RealFence {
    fence: PipeFence,
    is_signalled: bool,
}

struct SemaphoreState {
    fence: RealFence,
}

enum SemaphoreWaitAction {
    Signal,
    Wait,
}

impl SemaphoreState {
    fn gpu_signal(&mut self, ctx: &PipeContext) {
        self.fence.fence.gpu_signal(ctx);
        self.fence.is_signalled = true;
    }

    fn gpu_wait(&mut self, ctx: &PipeContext) {
        self.fence.fence.gpu_wait(ctx);
        self.fence.is_signalled = false;
    }

    fn is_signalled(&self) -> bool {
        self.fence.is_signalled
    }

    fn do_action(
        mut this: MutexGuard<Self>,
        cv: &Condvar,
        action: SemaphoreWaitAction,
        ctx: &PipeContext,
    ) -> CLResult<()> {
        // We need to wait until is_signalled gets set to the proper state.
        let is_signalled_expected = !matches!(action, SemaphoreWaitAction::Signal);

        this = cv
            .wait_while(this, |state| state.is_signalled() != is_signalled_expected)
            .or(Err(CL_OUT_OF_HOST_MEMORY))?;

        match action {
            // If this semaphore was already signalled, we need to wait until something
            // successfully waited on it before we can re-signal.
            SemaphoreWaitAction::Signal => this.gpu_signal(ctx),

            // We need to wait until something signals this semaphore before we can wait on it.
            SemaphoreWaitAction::Wait => this.gpu_wait(ctx),
        }

        drop(this);
        cv.notify_all();
        Ok(())
    }
}

/// Object representing a GPU semaphore that can be signalled and waited on. A semaphore is either
/// in a signalled or reset state and can only be waited on by a single consumer after which it
/// needs to be reset by a new call to gpu_signal.
pub struct Semaphore {
    pub base: CLObjectBase<CL_INVALID_SEMAPHORE_KHR>,
    pub ctx: Arc<Context>,
    pub props: MultiValProperties<cl_semaphore_properties_khr>,
    pub dev: &'static Device,
    state: Mutex<SemaphoreState>,
    /// Condition Variable used for waiting on a gpu_signal or gpu_wait operation to executed on the
    /// queue thread.
    signal_cv: Condvar,
}

impl_cl_type_trait!(cl_semaphore_khr, Semaphore, CL_INVALID_SEMAPHORE_KHR);

impl Semaphore {
    pub fn new(
        ctx: Arc<Context>,
        props: MultiValProperties<cl_semaphore_properties_khr>,
        dev: &'static Device,
    ) -> CLResult<Arc<Self>> {
        Ok(Arc::new(Self {
            base: CLObjectBase::new(RusticlTypes::Semaphore),
            ctx: ctx,
            props: props,
            dev: dev,
            state: Mutex::new(SemaphoreState {
                fence: RealFence {
                    fence: dev
                        .screen()
                        .create_semaphore()
                        .ok_or(CL_OUT_OF_HOST_MEMORY)?,
                    is_signalled: false,
                },
            }),
            signal_cv: Condvar::new(),
        }))
    }

    /// Makes the GPU signal the semaphore.
    pub fn gpu_signal(semas: Vec<Arc<Self>>) -> EventSig {
        Box::new(move |_, ctx| {
            for sema in semas {
                SemaphoreState::do_action(
                    sema.state(),
                    &sema.signal_cv,
                    SemaphoreWaitAction::Signal,
                    ctx,
                )?;
            }
            Ok(())
        })
    }

    /// Makes the GPU wait on the semaphore to be signalled.
    pub fn gpu_wait(&self, ctx: &PipeContext) -> CLResult<()> {
        SemaphoreState::do_action(
            self.state(),
            &self.signal_cv,
            SemaphoreWaitAction::Wait,
            ctx,
        )
    }

    pub fn is_signalled(&self) -> bool {
        self.state().is_signalled()
    }

    fn state(&self) -> MutexGuard<SemaphoreState> {
        self.state.lock().unwrap()
    }
}
