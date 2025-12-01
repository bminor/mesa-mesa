/*
 * Copyright (C) 2025 Collabora, Ltd.
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
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 *
 */

#ifndef __PAN_COMPILER_H__
#define __PAN_COMPILER_H__

#include <stdbool.h>
#include <stdio.h>

typedef struct nir_shader nir_shader;
struct nir_shader_compiler_options;

const struct nir_shader_compiler_options *
pan_get_nir_shader_compiler_options(unsigned arch);

void pan_preprocess_nir(nir_shader *nir, unsigned gpu_id);
void pan_optimize_nir(nir_shader *nir, unsigned gpu_id);
void pan_postprocess_nir(nir_shader *nir, unsigned gpu_id);

void pan_nir_lower_texture_early(nir_shader *nir, unsigned gpu_id);
void pan_nir_lower_texture_late(nir_shader *nir, unsigned gpu_id);

void pan_disassemble(FILE *fp, const void *code, size_t size,
                     unsigned gpu_id, bool verbose);

#endif /* __PAN_COMPILER_H__ */
