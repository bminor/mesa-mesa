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
