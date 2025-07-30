/*
 * Copyright © 2025 Imagination Technologies Ltd.
 * SPDX-License-Identifier: MIT
 */

#include "libcl.h"

KERNEL(1)
vs_nop_common(void)
{
   return;
}

KERNEL(1)
fs_nop_common(void)
{
   return;
}

KERNEL(1)
cs_nop_common(void)
{
   return;
}

/* gl_Position = vec4(in.xyz, 1.0f); */
KERNEL(1)
vs_passthrough_common(void)
{
   nir_uvsw_write_pco(0, nir_load_vtxin_pco(3, 0));
   nir_uvsw_write_pco(3, 1.0f);
}

/* gl_Position = vec4(in.xyz, 1.0f); rta_out = in.w; */
KERNEL(1)
vs_passthrough_rta_common(void)
{
   vs_passthrough_common();
   nir_uvsw_write_pco(4, nir_load_vtxin_pco(1, 3));
}
