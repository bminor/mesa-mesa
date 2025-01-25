/*
 * Mesa 3-D graphics library
 *
 * Copyright (C) 1999-2008  Brian Paul   All Rights Reserved.
 * Copyright (C) 2010 LunarG Inc.
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
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 *
 * Authors:
 *    Chia-I Wu <olv@lunarg.com>
 */

#include <string.h>
#include <stdlib.h>
#include "glapi/glapi.h"
#include "table.h"
#include "stub.h"

/*
 * _mesa_glapi_Dispatch,
 * _mesa_glapi_tls_Dispatch, _mesa_glapi_tls_Context,
 * _mesa_glapi_set_dispatch, and _mesa_glapi_get_dispatch
 * are defined in u_current.c.
 */

/**
 * Return size of dispatch table struct as number of functions (or
 * slots).
 */
unsigned int
_mesa_glapi_get_dispatch_table_size(void)
{
   return _gloffset_COUNT;
}

static const struct mapi_stub *
_glapi_get_stub(const char *name)
{
   if (!name || name[0] != 'g' || name[1] != 'l')
      return NULL;
   name += 2;

   return stub_find_public(name);
}

/**
 * Return offset of entrypoint for named function within dispatch table.
 */
int
_mesa_glapi_get_proc_offset(const char *funcName)
{
   const struct mapi_stub *stub = _glapi_get_stub(funcName);
   return (stub) ? stub_get_slot(stub) : -1;
}

/**
 * Return pointer to the named function.  If the function name isn't found
 * in the name of static functions, try generating a new API entrypoint on
 * the fly with assembly language.
 */
_glapi_proc
_mesa_glapi_get_proc_address(const char *funcName)
{
   const struct mapi_stub *stub = _glapi_get_stub(funcName);
   return (stub) ? (_glapi_proc) stub_get_addr(stub) : NULL;
}

/**
 * Return the name of the function at the given dispatch offset.
 * This is only intended for debugging.
 */
const char *
_glapi_get_proc_name(unsigned int offset)
{
   const struct mapi_stub *stub = stub_find_by_slot(offset);
   return stub ? stub_get_name(stub) : NULL;
}

/** Return pointer to new dispatch table filled with no-op functions */
struct _glapi_table *
_glapi_new_nop_table(void)
{
   struct _glapi_table *table;

   table = malloc(_gloffset_COUNT * sizeof(mapi_func));
   if (table) {
      memcpy(table, table_noop_array, _gloffset_COUNT * sizeof(mapi_func));
   }
   return table;
}

void
_glapi_set_nop_handler(_glapi_nop_handler_proc func)
{
   table_set_noop_handler(func);
}
