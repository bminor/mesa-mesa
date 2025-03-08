/* Copyright (C) 2010 LunarG Inc.
 * SPDX-License-Identifier: MIT
 *
 * Authors:
 *    Chia-I Wu <olv@lunarg.com>
 */

#include "glapi/glapi.h"
#include "stub.h"
#include "entry.h"

/* REALLY_INITIAL_EXEC implies __GLIBC__ */
#if defined(USE_X86_ASM) && defined(REALLY_INITIAL_EXEC)
#include "entry_x86_tls.h"
#define MAPI_TMP_STUB_ASM_GCC
#include "shared_glapi_mapi_tmp.h"

extern unsigned long
x86_current_tls();

extern char x86_entry_start[] HIDDEN;
extern char x86_entry_end[] HIDDEN;

static inline _glapi_proc
entry_generate_or_patch(int, char *, size_t);

void
entry_patch_public(void)
{
#ifndef GLX_X86_READONLY_TEXT
   char *entry;
   int slot = 0;
   for (entry = x86_entry_start; entry < x86_entry_end;
        entry += X86_ENTRY_SIZE, ++slot)
      entry_generate_or_patch(slot, entry, X86_ENTRY_SIZE);
#endif
}

_glapi_proc
entry_get_public(int slot)
{
   return (_glapi_proc) (x86_entry_start + slot * X86_ENTRY_SIZE);
}

static void
entry_patch(_glapi_proc entry, int slot)
{
   char *code = (char *) entry;
   *((unsigned long *) (code + 8)) = slot * sizeof(_glapi_proc);
}

static inline _glapi_proc
entry_generate_or_patch(int slot, char *code, size_t size)
{
   const char code_templ[16] = {
      0x65, 0xa1, 0x00, 0x00, 0x00, 0x00, /* movl %gs:0x0, %eax */
      0xff, 0xa0, 0x34, 0x12, 0x00, 0x00, /* jmp *0x1234(%eax) */
      0x90, 0x90, 0x90, 0x90              /* nop's */
   };
   _glapi_proc entry;

   if (size < sizeof(code_templ))
      return NULL;

   memcpy(code, code_templ, sizeof(code_templ));

   *((unsigned long *) (code + 2)) = x86_current_tls();
   entry = (_glapi_proc) code;
   entry_patch(entry, slot);

   return entry;
}

#elif defined(USE_X86_64_ASM) && defined(REALLY_INITIAL_EXEC)
#include "entry_x86-64_tls.h"
#define MAPI_TMP_STUB_ASM_GCC
#include "shared_glapi_mapi_tmp.h"

#include <string.h>

void
entry_patch_public(void)
{
}

extern char
x86_64_entry_start[] HIDDEN;

_glapi_proc
entry_get_public(int slot)
{
   return (_glapi_proc) (x86_64_entry_start + slot * 32);
}

#elif defined(USE_PPC64LE_ASM) && UTIL_ARCH_LITTLE_ENDIAN && defined(REALLY_INITIAL_EXEC)
#include "entry_ppc64le_tls.h"
#define MAPI_TMP_STUB_ASM_GCC
#include "shared_glapi_mapi_tmp.h"

#include <string.h>

void
entry_patch_public(void)
{
}

extern char
ppc64le_entry_start[] HIDDEN;

_glapi_proc
entry_get_public(int slot)
{
   return (_glapi_proc) (ppc64le_entry_start + slot * PPC64LE_ENTRY_SIZE);
}

#else

/* C version of the public entries */
#define MAPI_TMP_DEFINES
#define MAPI_TMP_PUBLIC_ENTRIES
#include "shared_glapi_mapi_tmp.h"

_glapi_proc
entry_get_public(int slot)
{
   /* pubic_entries are defined by MAPI_TMP_PUBLIC_ENTRIES */
   return public_entries[slot];
}

#if defined(_WIN32) && defined(_WINDOWS_)
#error "Should not include <windows.h> here"
#endif

#endif /* asm */
