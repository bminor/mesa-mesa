/*
 * Copyright © Microsoft Corporation
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the next
 * paragraph) shall be included in all copies or substantial portions of the
 * Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
 * IN THE SOFTWARE.
 */

#include "dpb_buffer_manager.h"
#include "frontend/winsys_handle.h"
#include "gallium/drivers/d3d12/d3d12_interop_public.h"
#include "vl/vl_video_buffer.h"

HRESULT
dpb_buffer_manager::get_read_only_handle( struct pipe_video_buffer *buffer,
                                          struct pipe_context *pipe,
                                          ComPtr<ID3D12Device> &device,
                                          HANDLE *pReadOnlyHandle,
                                          UINT *pSubresourceIndex )
{
   if( !buffer || !pipe || !device || !pReadOnlyHandle || !pSubresourceIndex )
      return E_POINTER;

   *pReadOnlyHandle = nullptr;

   // Get pipe resources from the video buffer
   struct pipe_resource *buf_resources[VL_NUM_COMPONENTS];
   memset( buf_resources, 0, sizeof( buf_resources ) );
   buffer->get_resources( buffer, &buf_resources[0] );

   if( !buf_resources[0] )
      return E_INVALIDARG;

   // Get the winsys handle for the resource
   struct winsys_handle src_wshandle = {};
   src_wshandle.type = WINSYS_HANDLE_TYPE_D3D12_RES;

   if( !pipe->screen->resource_get_handle( pipe->screen, pipe, buf_resources[0], &src_wshandle, 0 /*usage*/ ) )
   {
      return E_FAIL;
   }

   if( !src_wshandle.com_obj )
      return E_FAIL;

   // Create a read-only shared handle from the D3D12 resource
   HRESULT hr = S_OK;
   HANDLE originalHandle = nullptr;

   // First, create a shared handle with full access from the original resource
   hr = device->CreateSharedHandle( static_cast<ID3D12Resource *>( src_wshandle.com_obj ),
                                    nullptr,       // Security attributes (default)
                                    GENERIC_ALL,   // Full access for the original handle
                                    nullptr,       // Name
                                    &originalHandle );
   if( FAILED( hr ) || !originalHandle )
      return hr;

   // Duplicate the handle with restricted (read-only) access rights
   // This creates a new handle that can only be used for reading
   BOOL duplicateResult = DuplicateHandle( GetCurrentProcess(),   // Source process handle
                                           originalHandle,        // Source handle
                                           GetCurrentProcess(),   // Target process handle
                                           pReadOnlyHandle,       // Target handle
                                           GENERIC_READ,          // Desired access (read-only)
                                           FALSE,                 // Inherit handle
                                           0 );                   // Options

   // Clean up the original handle since we only need the read-only version
   CloseHandle( originalHandle );

   if( !duplicateResult || !*pReadOnlyHandle )
   {
      return HRESULT_FROM_WIN32( GetLastError() );
   }

   // Retrieve subresource index if available
   // from the associated data of the video buffer
   // which will contain the subresource index
   // if the underlying resource uses texture arrays
   if( buffer->associated_data )
   {
      struct d3d12_interop_video_buffer_associated_data *associated_data =
         static_cast<struct d3d12_interop_video_buffer_associated_data *>( buffer->associated_data );
      *pSubresourceIndex = associated_data->subresource_index;
   }
   else
   {
      *pSubresourceIndex = 0;   // Default to 0 if no associated data
   }

   return S_OK;
}

// retrieve a buffer from the pool
struct pipe_video_buffer *
dpb_buffer_manager::get_fresh_dpb_buffer()
{
   for( auto &entry : m_pool )
   {
      if( !entry.used )
      {
         entry.used = true;
         return entry.buffer;
      }
   }

   assert( false );   // Did not find an unused buffer
   return NULL;
}

// release a buffer back to the pool
void
dpb_buffer_manager::release_dpb_buffer( struct pipe_video_buffer *target )
{
   for( auto &entry : m_pool )
   {
      if( entry.buffer == target )
      {
         entry.used = false;
         break;
      }
   }
}

dpb_buffer_manager::dpb_buffer_manager(
   struct pipe_video_codec *codec, unsigned width, unsigned height, enum pipe_format buffer_format, unsigned pool_size )
   : m_codec( codec ), m_pool( pool_size, { NULL, false } )
{
   m_template.width = width;
   m_template.height = height;
   m_template.buffer_format = buffer_format;

   if( codec->context->screen->get_video_param( codec->context->screen,
                                                codec->profile,
                                                PIPE_VIDEO_ENTRYPOINT_ENCODE,
                                                PIPE_VIDEO_CAP_ENC_READABLE_RECONSTRUCTED_PICTURE ) != 0 )
   {
      m_template.bind = PIPE_BIND_SHARED;   // Indicate we want shared resource capabilities
   }

   for( auto &entry : m_pool )
      entry.buffer = m_codec->create_dpb_buffer( m_codec, NULL, &m_template );
}

dpb_buffer_manager::~dpb_buffer_manager()
{
   for( auto &entry : m_pool )
      entry.buffer->destroy( entry.buffer );
}
