// Copyright 2025 Google
// SPDX-License-Identifier: MIT

use std::os::fd::OwnedFd;

use rustix::event::epoll;
use rustix::event::epoll::CreateFlags;
use rustix::event::epoll::EventData;
use rustix::event::epoll::EventFlags;
use rustix::event::epoll::EventVec;
use rustix::io::Errno;

use crate::MesaResult;
use crate::OwnedDescriptor;
use crate::WaitEvent;
use crate::WaitTimeout;
use crate::WAIT_CONTEXT_MAX;

pub struct WaitContext {
    epoll_ctx: OwnedFd,
}

impl WaitContext {
    pub fn new() -> MesaResult<WaitContext> {
        let epoll = epoll::create(CreateFlags::CLOEXEC)?;
        Ok(WaitContext { epoll_ctx: epoll })
    }

    pub fn add(&mut self, connection_id: u64, descriptor: &OwnedDescriptor) -> MesaResult<()> {
        epoll::add(
            &self.epoll_ctx,
            descriptor,
            EventData::new_u64(connection_id),
            EventFlags::IN,
        )?;
        Ok(())
    }

    pub fn wait(&mut self, timeout: WaitTimeout) -> MesaResult<Vec<WaitEvent>> {
        let mut event_vec = EventVec::with_capacity(WAIT_CONTEXT_MAX);
        let epoll_timeout = match timeout {
            WaitTimeout::Finite(duration) => duration.as_millis().try_into()?,
            WaitTimeout::NoTimeout => -1,
        };

        loop {
            match epoll::wait(&self.epoll_ctx, &mut event_vec, epoll_timeout) {
                Err(Errno::INTR) => (), // Continue loop on EINTR
                result => break result?,
            }
        }

        let events = event_vec
            .iter()
            .map(|e| {
                let flags: EventFlags = e.flags;
                WaitEvent {
                    connection_id: e.data.u64(),
                    readable: flags.contains(EventFlags::IN),
                    hung_up: flags.contains(EventFlags::HUP),
                }
            })
            .collect();

        Ok(events)
    }

    pub fn delete(&mut self, descriptor: &OwnedDescriptor) -> MesaResult<()> {
        epoll::delete(&self.epoll_ctx, descriptor)?;
        Ok(())
    }
}
