use crate::pipe::screen::*;

use libc_rust_gen::close;
use mesa_rust_gen::*;

use std::sync::Arc;

pub struct FenceFd {
    pub fd: i32,
}

impl Drop for FenceFd {
    fn drop(&mut self) {
        unsafe {
            close(self.fd);
        }
    }
}

pub struct PipeFence {
    fence: *mut pipe_fence_handle,
    screen: Arc<PipeScreen>,
}

unsafe impl Send for PipeFence {}

impl PipeFence {
    pub fn new(fence: *mut pipe_fence_handle, screen: &Arc<PipeScreen>) -> Self {
        Self {
            fence: fence,
            screen: Arc::clone(screen),
        }
    }

    /// Returns false on errors.
    ///
    /// TODO: should be a Result.
    pub fn wait(&self) -> bool {
        self.screen.fence_finish(self.fence)
    }
}

impl Drop for PipeFence {
    fn drop(&mut self) {
        self.screen.unref_fence(self.fence);
    }
}
