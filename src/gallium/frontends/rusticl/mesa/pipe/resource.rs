use mesa_rust_gen::*;

use std::{
    marker::PhantomData,
    mem,
    ptr::{self, NonNull},
};

use super::context::PipeContext;

#[derive(PartialEq, Eq, Hash)]
#[repr(transparent)]
pub struct PipeResource {
    pipe: NonNull<pipe_resource>,
}

const PIPE_RESOURCE_FLAG_RUSTICL_IS_USER: u32 = PIPE_RESOURCE_FLAG_FRONTEND_PRIV;

// SAFETY: pipe_resource is considered a thread safe type
unsafe impl Send for PipeResource {}
unsafe impl Sync for PipeResource {}

/// A thread safe wrapper around [pipe_image_view]. It's purpose is to increase the reference count
/// on the [pipe_resource] this view belongs to.
#[repr(transparent)]
pub struct PipeImageView {
    pub(super) pipe: pipe_image_view,
}

impl PipeImageView {
    fn new(pipe: pipe_image_view) -> Self {
        unsafe { pipe_resource_reference(&mut ptr::null_mut(), pipe.resource) }
        Self { pipe: pipe }
    }

    pub fn slice_to_pipe(slice: &[PipeImageView]) -> &[pipe_image_view] {
        // SAFETY: `PipeImageView` is a transparent wrapper around `pipe_image_view`, so transmute
        //         on the slice is safe.
        unsafe { mem::transmute(slice) }
    }
}

impl Drop for PipeImageView {
    fn drop(&mut self) {
        unsafe { pipe_resource_reference(&mut self.pipe.resource, ptr::null_mut()) }
    }
}

// SAFETY: pipe_image_view is just static data around a pipe_resource, which itself is a thread-safe
//         type.
unsafe impl Send for PipeImageView {}
unsafe impl Sync for PipeImageView {}

// Image dimensions provide by application to be used in both
// image and sampler views when image is created from buffer
#[derive(PartialEq, Eq)]
pub struct AppImgInfo {
    row_stride: u32,
    width: u32,
    height: u32,
}

impl AppImgInfo {
    pub fn new(row_stride: u32, width: u32, height: u32) -> AppImgInfo {
        AppImgInfo {
            row_stride: row_stride,
            width: width,
            height: height,
        }
    }
}

impl PipeResource {
    pub(super) fn new(res: *mut pipe_resource, is_user: bool) -> Option<Self> {
        let mut res = NonNull::new(res)?;

        if is_user {
            unsafe {
                res.as_mut().flags |= PIPE_RESOURCE_FLAG_RUSTICL_IS_USER;
            }
        }

        Some(Self { pipe: res })
    }

    pub(super) fn pipe(&self) -> *mut pipe_resource {
        self.pipe.as_ptr()
    }

    fn as_ref(&self) -> &pipe_resource {
        // SAFETY: it contains a valid pointer
        unsafe { self.pipe.as_ref() }
    }

    pub fn width(&self) -> u32 {
        self.as_ref().width0
    }

    pub fn height(&self) -> u16 {
        self.as_ref().height0
    }

    pub fn depth(&self) -> u16 {
        self.as_ref().depth0
    }

    pub fn array_size(&self) -> u16 {
        self.as_ref().array_size
    }

    pub fn is_buffer(&self) -> bool {
        self.as_ref().target() == pipe_texture_target::PIPE_BUFFER
    }

    pub fn is_linear(&self) -> bool {
        self.as_ref().bind & PIPE_BIND_LINEAR != 0
    }

    pub fn is_staging(&self) -> bool {
        self.as_ref().usage() == pipe_resource_usage::PIPE_USAGE_STAGING
    }

    pub fn is_user(&self) -> bool {
        self.as_ref().flags & PIPE_RESOURCE_FLAG_RUSTICL_IS_USER != 0
    }

    pub fn pipe_image_view(
        &self,
        format: pipe_format,
        read_write: bool,
        host_access: u16,
        size: u32,
        app_img_info: Option<&AppImgInfo>,
    ) -> PipeImageView {
        let pipe = PipeResource::as_ref(self);
        let u = if let Some(app_img_info) = app_img_info {
            pipe_image_view__bindgen_ty_1 {
                tex2d_from_buf: pipe_image_view__bindgen_ty_1__bindgen_ty_3 {
                    offset: 0,
                    row_stride: app_img_info.row_stride as u16,
                    width: app_img_info.width as u16,
                    height: app_img_info.height as u16,
                },
            }
        } else if self.is_buffer() {
            pipe_image_view__bindgen_ty_1 {
                buf: pipe_image_view__bindgen_ty_1__bindgen_ty_2 {
                    offset: 0,
                    size: size,
                },
            }
        } else {
            let mut tex = pipe_image_view__bindgen_ty_1__bindgen_ty_1::default();
            tex.set_level(0);
            tex.set_first_layer(0);
            if pipe.target() == pipe_texture_target::PIPE_TEXTURE_3D {
                tex.set_last_layer((pipe.depth0 - 1).into());
            } else if pipe.array_size > 0 {
                tex.set_last_layer((pipe.array_size - 1).into());
            } else {
                tex.set_last_layer(0);
            }

            pipe_image_view__bindgen_ty_1 { tex: tex }
        };

        let shader_access = if read_write {
            PIPE_IMAGE_ACCESS_READ_WRITE
        } else {
            PIPE_IMAGE_ACCESS_WRITE
        } as u16;

        let access = if app_img_info.is_some() {
            PIPE_IMAGE_ACCESS_TEX2D_FROM_BUFFER
        } else {
            0
        } as u16;

        PipeImageView::new(pipe_image_view {
            resource: self.pipe(),
            format: format,
            access: access | host_access,
            shader_access: shader_access,
            u: u,
        })
    }

    pub fn pipe_sampler_view_template(
        &self,
        format: pipe_format,
        size: u32,
        app_img_info: Option<&AppImgInfo>,
    ) -> pipe_sampler_view {
        let mut res = pipe_sampler_view::default();
        unsafe {
            u_sampler_view_default_template(&mut res, self.pipe(), format);
        }

        if let Some(app_img_info) = app_img_info {
            res.u.tex2d_from_buf.offset = 0;
            res.u.tex2d_from_buf.row_stride = app_img_info.row_stride as u16;
            res.u.tex2d_from_buf.width = app_img_info.width as u16;
            res.u.tex2d_from_buf.height = app_img_info.height as u16;

            res.set_is_tex2d_from_buf(true);
        } else if res.target() == pipe_texture_target::PIPE_BUFFER {
            res.u.buf.offset = 0;
            res.u.buf.size = size;
        }

        res
    }
}

impl Drop for PipeResource {
    fn drop(&mut self) {
        unsafe { pipe_resource_reference(&mut self.pipe.as_ptr(), ptr::null_mut()) }
    }
}

/// Wrapper around gallium's pipe_sampler_view.
///
/// It deals with the refcounting and frees the object automatically if not needed anymore.
#[repr(transparent)]
pub struct PipeSamplerView<'c, 'r> {
    view: NonNull<pipe_sampler_view>,
    // the pipe_sampler_view object references both a context and a resource.
    _ctx: PhantomData<&'c PipeContext>,
    _res: PhantomData<&'r PipeResource>,
}

impl<'c, 'r> PipeSamplerView<'c, 'r> {
    pub fn new(
        ctx: &'c PipeContext,
        res: &'r PipeResource,
        format: pipe_format,
        size: u32,
        app_img_info: Option<&AppImgInfo>,
    ) -> Option<Self> {
        let template = res.pipe_sampler_view_template(format, size, app_img_info);
        let view = unsafe {
            ctx.pipe().as_ref().create_sampler_view.unwrap()(
                ctx.pipe().as_ptr(),
                res.pipe(),
                &template,
            )
        };

        let view = NonNull::new(view)?;
        unsafe {
            debug_assert_eq!(view.as_ref().context, ctx.pipe().as_ptr());
            debug_assert_eq!(view.as_ref().texture, res.pipe());
        }

        Some(Self {
            view: view,
            _ctx: PhantomData,
            _res: PhantomData,
        })
    }

    pub(crate) fn as_pipe(views: &mut [Self]) -> *mut *mut pipe_sampler_view {
        // We are transparent over *mut pipe_sample_view, so this is sound.
        views.as_mut_ptr().cast()
    }
}

impl Drop for PipeSamplerView<'_, '_> {
    fn drop(&mut self) {
        unsafe {
            pipe_sampler_view_reference(&mut ptr::null_mut(), self.view.as_ptr());
        }
    }
}
