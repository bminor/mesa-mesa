/*
 * Mesa 3-D graphics library
 *
 * Copyright (C) 1999-2008  Brian Paul   All Rights Reserved.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included
 * in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS
 * OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR
 * OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE,
 * ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR
 * OTHER DEALINGS IN THE SOFTWARE.
 */


/*
 * This file manages the OpenGL API dispatch layer.
 * The dispatch table (struct _glapi_table) is basically just a list
 * of function pointers.
 * There are functions to set/get the current dispatch table for the
 * current thread.
 */

#include "c11/threads.h"
#include "util/u_thread.h"
#include "glapi/glapi.h"

#include "table.h"
#include "stub.h"

/**
 * \name Current dispatch and current context control variables
 *
 * Depending on whether or not multithreading is support, and the type of
 * support available, several variables are used to store the current context
 * pointer and the current dispatch table pointer. In the non-threaded case,
 * the variables \c _mesa_glapi_Dispatch and \c _glapi_Context are used for this
 * purpose.
 *
 * In multi threaded case, The TLS variables \c _mesa_glapi_tls_Dispatch and
 * \c _mesa_glapi_tls_Context are used. Having \c _mesa_glapi_Dispatch
 * be hardcoded to \c NULL maintains binary compatability between TLS enabled
 * loaders and non-TLS DRI drivers. When \c _mesa_glapi_Dispatch
 * are \c NULL, the thread state data \c ContextTSD are used.
 */
/*@{*/

__THREAD_INITIAL_EXEC struct _glapi_table *_mesa_glapi_tls_Dispatch
   = (struct _glapi_table *) table_noop_array;

__THREAD_INITIAL_EXEC void *_mesa_glapi_tls_Context;

const struct _glapi_table *_mesa_glapi_Dispatch;

/*@}*/

/**
 * Set the current context pointer for this thread.
 * The context pointer is an opaque type which should be cast to
 * void from the real context pointer type.
 */
void
_mesa_glapi_set_context(void *ptr)
{
   _mesa_glapi_tls_Context = ptr;
}

/**
 * Get the current context pointer for this thread.
 * The context pointer is an opaque type which should be cast from
 * void to the real context pointer type.
 */
void *
_mesa_glapi_get_context(void)
{
   return _mesa_glapi_tls_Context;
}

/**
 * Set the global or per-thread dispatch table pointer.
 * If the dispatch parameter is NULL we'll plug in the no-op dispatch
 * table (__glapi_noop_table).
 */
void
_mesa_glapi_set_dispatch(struct _glapi_table *tbl)
{
   stub_init_once();

   if (!tbl)
      tbl = (struct _glapi_table *) table_noop_array;

   _mesa_glapi_tls_Dispatch = tbl;
}

/**
 * Return pointer to current dispatch table for calling thread.
 */
struct _glapi_table *
_mesa_glapi_get_dispatch(void)
{
   return _mesa_glapi_tls_Dispatch;
}
