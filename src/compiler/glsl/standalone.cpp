/*
 * Copyright © 2008, 2009 Intel Corporation
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
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 */
#include <getopt.h>

/** @file standalone.cpp
 *
 * Standalone compiler helper lib.  Used by standalone glsl_compiler and
 * also available to drivers to implement their own standalone compiler
 * with driver backend.
 */

#include "ast.h"
#include "glsl_parser_extras.h"
#include "ir_optimization.h"
#include "standalone_scaffolding.h"
#include "standalone.h"
#include "util/set.h"
#include "gl_nir_linker.h"
#include "glsl_parser_extras.h"
#include "builtin_functions.h"
#include "linker_util.h"
#include "main/mtypes.h"
#include "program/program.h"
#include "nir_shader_compiler_options.h"
#include "pipe/p_screen.h"

static const struct standalone_options *options;

static const struct nir_shader_compiler_options nir_vs_options = { 0 };
static const struct nir_shader_compiler_options nir_fs_options = { 0 };

static void
initialize_context(struct gl_context *ctx, gl_api api)
{
   initialize_context_to_defaults(ctx, api);
   _mesa_glsl_builtin_functions_init_or_ref();

   ctx->Version = 450;

   ctx->screen->nir_options[MESA_SHADER_VERTEX] = &nir_vs_options;
   ctx->screen->nir_options[MESA_SHADER_FRAGMENT] = &nir_fs_options;


   /* The standalone compiler needs to claim support for almost
    * everything in order to compile the built-in functions.
    */
   ctx->Const.GLSLVersion = options->glsl_version;
   ctx->Extensions.ARB_ES3_compatibility = true;
   ctx->Extensions.ARB_ES3_1_compatibility = true;
   ctx->Extensions.ARB_ES3_2_compatibility = true;
   ctx->Const.MaxComputeWorkGroupCount[0] = 65535;
   ctx->Const.MaxComputeWorkGroupCount[1] = 65535;
   ctx->Const.MaxComputeWorkGroupCount[2] = 65535;
   ctx->Const.MaxComputeWorkGroupSize[0] = 1024;
   ctx->Const.MaxComputeWorkGroupSize[1] = 1024;
   ctx->Const.MaxComputeWorkGroupSize[2] = 64;
   ctx->Const.MaxComputeWorkGroupInvocations = 1024;
   ctx->Const.MaxComputeSharedMemorySize = 32768;
   ctx->Const.MaxComputeVariableGroupSize[0] = 512;
   ctx->Const.MaxComputeVariableGroupSize[1] = 512;
   ctx->Const.MaxComputeVariableGroupSize[2] = 64;
   ctx->Const.MaxComputeVariableGroupInvocations = 512;
   ctx->Const.Program[MESA_SHADER_COMPUTE].MaxTextureImageUnits = 16;
   ctx->Const.Program[MESA_SHADER_COMPUTE].MaxUniformComponents = 1024;
   ctx->Const.Program[MESA_SHADER_COMPUTE].MaxCombinedUniformComponents = 1024;
   ctx->Const.Program[MESA_SHADER_COMPUTE].MaxInputComponents = 0; /* not used */
   ctx->Const.Program[MESA_SHADER_COMPUTE].MaxOutputComponents = 0; /* not used */
   ctx->Const.Program[MESA_SHADER_COMPUTE].MaxAtomicBuffers = 8;
   ctx->Const.Program[MESA_SHADER_COMPUTE].MaxAtomicCounters = 8;
   ctx->Const.Program[MESA_SHADER_COMPUTE].MaxImageUniforms = 8;
   ctx->Const.Program[MESA_SHADER_COMPUTE].MaxUniformBlocks = 12;

   switch (ctx->Const.GLSLVersion) {
   case 100:
      ctx->Const.MaxClipPlanes = 0;
      ctx->Const.MaxCombinedTextureImageUnits = 8;
      ctx->Const.MaxDrawBuffers = 2;
      ctx->Const.MinProgramTexelOffset = 0;
      ctx->Const.MaxProgramTexelOffset = 0;
      ctx->Const.MaxLights = 0;
      ctx->Const.MaxTextureCoordUnits = 0;
      ctx->Const.MaxTextureUnits = 8;

      ctx->Const.Program[MESA_SHADER_VERTEX].MaxAttribs = 8;
      ctx->Const.Program[MESA_SHADER_VERTEX].MaxTextureImageUnits = 0;
      ctx->Const.Program[MESA_SHADER_VERTEX].MaxUniformComponents = 128 * 4;
      ctx->Const.Program[MESA_SHADER_VERTEX].MaxCombinedUniformComponents = 128 * 4;
      ctx->Const.Program[MESA_SHADER_VERTEX].MaxInputComponents = 0; /* not used */
      ctx->Const.Program[MESA_SHADER_VERTEX].MaxOutputComponents = 32;

      ctx->Const.Program[MESA_SHADER_FRAGMENT].MaxTextureImageUnits =
         ctx->Const.MaxCombinedTextureImageUnits;
      ctx->Const.Program[MESA_SHADER_FRAGMENT].MaxUniformComponents = 16 * 4;
      ctx->Const.Program[MESA_SHADER_FRAGMENT].MaxCombinedUniformComponents = 16 * 4;
      ctx->Const.Program[MESA_SHADER_FRAGMENT].MaxInputComponents =
         ctx->Const.Program[MESA_SHADER_VERTEX].MaxOutputComponents;
      ctx->Const.Program[MESA_SHADER_FRAGMENT].MaxOutputComponents = 0; /* not used */

      ctx->Const.MaxVarying = ctx->Const.Program[MESA_SHADER_VERTEX].MaxOutputComponents / 4;
      break;
   case 110:
   case 120:
      ctx->Const.MaxClipPlanes = 6;
      ctx->Const.MaxCombinedTextureImageUnits = 2;
      ctx->Const.MaxDrawBuffers = 1;
      ctx->Const.MinProgramTexelOffset = 0;
      ctx->Const.MaxProgramTexelOffset = 0;
      ctx->Const.MaxLights = 8;
      ctx->Const.MaxTextureCoordUnits = 2;
      ctx->Const.MaxTextureUnits = 2;

      ctx->Const.Program[MESA_SHADER_VERTEX].MaxAttribs = 16;
      ctx->Const.Program[MESA_SHADER_VERTEX].MaxTextureImageUnits = 0;
      ctx->Const.Program[MESA_SHADER_VERTEX].MaxUniformComponents = 512;
      ctx->Const.Program[MESA_SHADER_VERTEX].MaxCombinedUniformComponents = 512;
      ctx->Const.Program[MESA_SHADER_VERTEX].MaxInputComponents = 0; /* not used */
      ctx->Const.Program[MESA_SHADER_VERTEX].MaxOutputComponents = 32;

      ctx->Const.Program[MESA_SHADER_FRAGMENT].MaxTextureImageUnits =
         ctx->Const.MaxCombinedTextureImageUnits;
      ctx->Const.Program[MESA_SHADER_FRAGMENT].MaxUniformComponents = 64;
      ctx->Const.Program[MESA_SHADER_FRAGMENT].MaxCombinedUniformComponents = 64;
      ctx->Const.Program[MESA_SHADER_FRAGMENT].MaxInputComponents =
         ctx->Const.Program[MESA_SHADER_VERTEX].MaxOutputComponents;
      ctx->Const.Program[MESA_SHADER_FRAGMENT].MaxOutputComponents = 0; /* not used */

      ctx->Const.MaxVarying = ctx->Const.Program[MESA_SHADER_VERTEX].MaxOutputComponents / 4;
      break;
   case 130:
   case 140:
      ctx->Const.MaxClipPlanes = 8;
      ctx->Const.MaxCombinedTextureImageUnits = 16;
      ctx->Const.MaxDrawBuffers = 8;
      ctx->Const.MinProgramTexelOffset = -8;
      ctx->Const.MaxProgramTexelOffset = 7;
      ctx->Const.MaxLights = 8;
      ctx->Const.MaxTextureCoordUnits = 8;
      ctx->Const.MaxTextureUnits = 2;
      ctx->Const.MaxUniformBufferBindings = 84;
      ctx->Const.MaxVertexStreams = 4;
      ctx->Const.MaxTransformFeedbackBuffers = 4;

      ctx->Const.Program[MESA_SHADER_VERTEX].MaxAttribs = 16;
      ctx->Const.Program[MESA_SHADER_VERTEX].MaxTextureImageUnits = 16;
      ctx->Const.Program[MESA_SHADER_VERTEX].MaxUniformComponents = 1024;
      ctx->Const.Program[MESA_SHADER_VERTEX].MaxCombinedUniformComponents = 1024;
      ctx->Const.Program[MESA_SHADER_VERTEX].MaxInputComponents = 0; /* not used */
      ctx->Const.Program[MESA_SHADER_VERTEX].MaxOutputComponents = 64;

      ctx->Const.Program[MESA_SHADER_FRAGMENT].MaxTextureImageUnits = 16;
      ctx->Const.Program[MESA_SHADER_FRAGMENT].MaxUniformComponents = 1024;
      ctx->Const.Program[MESA_SHADER_FRAGMENT].MaxCombinedUniformComponents = 1024;
      ctx->Const.Program[MESA_SHADER_FRAGMENT].MaxInputComponents =
         ctx->Const.Program[MESA_SHADER_VERTEX].MaxOutputComponents;
      ctx->Const.Program[MESA_SHADER_FRAGMENT].MaxOutputComponents = 0; /* not used */

      ctx->Const.MaxVarying = ctx->Const.Program[MESA_SHADER_VERTEX].MaxOutputComponents / 4;
      break;
   case 150:
   case 330:
   case 400:
   case 410:
   case 420:
   case 430:
   case 440:
   case 450:
   case 460:
      ctx->Const.MaxClipPlanes = 8;
      ctx->Const.MaxDrawBuffers = 8;
      ctx->Const.MinProgramTexelOffset = -8;
      ctx->Const.MaxProgramTexelOffset = 7;
      ctx->Const.MaxLights = 8;
      ctx->Const.MaxTextureCoordUnits = 8;
      ctx->Const.MaxTextureUnits = 2;
      ctx->Const.MaxUniformBufferBindings = 84;
      ctx->Const.MaxVertexStreams = 4;
      ctx->Const.MaxTransformFeedbackBuffers = 4;
      ctx->Const.MaxShaderStorageBufferBindings = 4;
      ctx->Const.MaxShaderStorageBlockSize = 4096;
      ctx->Const.MaxAtomicBufferBindings = 4;

      ctx->Const.Program[MESA_SHADER_VERTEX].MaxAttribs = 16;
      ctx->Const.Program[MESA_SHADER_VERTEX].MaxTextureImageUnits = 16;
      ctx->Const.Program[MESA_SHADER_VERTEX].MaxUniformComponents = 1024;
      ctx->Const.Program[MESA_SHADER_VERTEX].MaxCombinedUniformComponents = 1024;
      ctx->Const.Program[MESA_SHADER_VERTEX].MaxInputComponents = 0; /* not used */
      ctx->Const.Program[MESA_SHADER_VERTEX].MaxOutputComponents = 64;

      ctx->Const.Program[MESA_SHADER_GEOMETRY].MaxTextureImageUnits = 16;
      ctx->Const.Program[MESA_SHADER_GEOMETRY].MaxUniformComponents = 1024;
      ctx->Const.Program[MESA_SHADER_GEOMETRY].MaxCombinedUniformComponents = 1024;
      ctx->Const.Program[MESA_SHADER_GEOMETRY].MaxInputComponents =
         ctx->Const.Program[MESA_SHADER_VERTEX].MaxOutputComponents;
      ctx->Const.Program[MESA_SHADER_GEOMETRY].MaxOutputComponents = 128;

      ctx->Const.Program[MESA_SHADER_FRAGMENT].MaxTextureImageUnits = 16;
      ctx->Const.Program[MESA_SHADER_FRAGMENT].MaxUniformComponents = 1024;
      ctx->Const.Program[MESA_SHADER_FRAGMENT].MaxCombinedUniformComponents = 1024;
      ctx->Const.Program[MESA_SHADER_FRAGMENT].MaxInputComponents =
         ctx->Const.Program[MESA_SHADER_GEOMETRY].MaxOutputComponents;
      ctx->Const.Program[MESA_SHADER_FRAGMENT].MaxOutputComponents = 0; /* not used */

      ctx->Const.MaxCombinedTextureImageUnits =
         ctx->Const.Program[MESA_SHADER_VERTEX].MaxTextureImageUnits
         + ctx->Const.Program[MESA_SHADER_GEOMETRY].MaxTextureImageUnits
         + ctx->Const.Program[MESA_SHADER_FRAGMENT].MaxTextureImageUnits;

      ctx->Const.MaxGeometryOutputVertices = 256;
      ctx->Const.MaxGeometryTotalOutputComponents = 1024;

      ctx->Const.MaxVarying = 60 / 4;
      break;
   case 300:
      ctx->Const.MaxClipPlanes = 8;
      ctx->Const.MaxCombinedTextureImageUnits = 32;
      ctx->Const.MaxDrawBuffers = 4;
      ctx->Const.MinProgramTexelOffset = -8;
      ctx->Const.MaxProgramTexelOffset = 7;
      ctx->Const.MaxLights = 0;
      ctx->Const.MaxTextureCoordUnits = 0;
      ctx->Const.MaxTextureUnits = 0;
      ctx->Const.MaxUniformBufferBindings = 84;
      ctx->Const.MaxVertexStreams = 4;
      ctx->Const.MaxTransformFeedbackBuffers = 4;

      ctx->Const.Program[MESA_SHADER_VERTEX].MaxAttribs = 16;
      ctx->Const.Program[MESA_SHADER_VERTEX].MaxTextureImageUnits = 16;
      ctx->Const.Program[MESA_SHADER_VERTEX].MaxUniformComponents = 1024;
      ctx->Const.Program[MESA_SHADER_VERTEX].MaxCombinedUniformComponents = 1024;
      ctx->Const.Program[MESA_SHADER_VERTEX].MaxInputComponents = 0; /* not used */
      ctx->Const.Program[MESA_SHADER_VERTEX].MaxOutputComponents = 16 * 4;

      ctx->Const.Program[MESA_SHADER_FRAGMENT].MaxTextureImageUnits = 16;
      ctx->Const.Program[MESA_SHADER_FRAGMENT].MaxUniformComponents = 224;
      ctx->Const.Program[MESA_SHADER_FRAGMENT].MaxCombinedUniformComponents = 224;
      ctx->Const.Program[MESA_SHADER_FRAGMENT].MaxInputComponents = 15 * 4;
      ctx->Const.Program[MESA_SHADER_FRAGMENT].MaxOutputComponents = 0; /* not used */

      ctx->Const.MaxVarying = ctx->Const.Program[MESA_SHADER_FRAGMENT].MaxInputComponents / 4;
      break;
   }

   ctx->Const.GenerateTemporaryNames = true;
   ctx->Const.MaxPatchVertices = 32;

   /* GL_ARB_explicit_uniform_location, GL_MAX_UNIFORM_LOCATIONS */
   ctx->Const.MaxUserAssignableUniformLocations =
      4 * MESA_SHADER_STAGES * MAX_UNIFORMS;
}

/* Returned string will have 'ctx' as its ralloc owner. */
static char *
load_text_file(void *ctx, const char *file_name)
{
   char *text = NULL;
   size_t size;
   size_t total_read = 0;
   FILE *fp = fopen(file_name, "rb");

   if (!fp) {
      return NULL;
   }

   fseek(fp, 0L, SEEK_END);
   size = ftell(fp);
   fseek(fp, 0L, SEEK_SET);

   text = (char *) ralloc_size(ctx, size + 1);
   if (text != NULL) {
      do {
         size_t bytes = fread(text + total_read,
               1, size - total_read, fp);
         if (bytes < size - total_read) {
            text = NULL;
            goto error;
         }

         if (bytes == 0) {
            break;
         }

         total_read += bytes;
      } while (total_read < size);

      text[total_read] = '\0';
      error:;
   }

   fclose(fp);

   return text;
}

static void
compile_shader(struct gl_context *ctx, struct gl_shader *shader)
{
   /* Print out the resulting IR if requested */
   FILE *print_file = options->dump_lir ? stdout : NULL;

   _mesa_glsl_compile_shader(ctx, shader, print_file, options->dump_ast,
                             options->dump_hir, true);
}

extern "C" struct gl_shader_program *
standalone_compile_shader(const struct standalone_options *_options,
      unsigned num_files, char* const* files, struct gl_context *ctx)
{
   int status = EXIT_SUCCESS;
   bool glsl_es = false;

   options = _options;

   switch (options->glsl_version) {
   case 100:
   case 300:
   case 310:
   case 320:
      glsl_es = true;
      break;
   case 110:
   case 120:
   case 130:
   case 140:
   case 150:
   case 330:
   case 400:
   case 410:
   case 420:
   case 430:
   case 440:
   case 450:
   case 460:
      glsl_es = false;
      break;
   default:
      fprintf(stderr, "Unrecognized GLSL version `%d'\n", options->glsl_version);
      return NULL;
   }

   if (glsl_es) {
      initialize_context(ctx, API_OPENGLES2);
   } else {
      initialize_context(ctx, options->glsl_version > 130 ? API_OPENGL_CORE : API_OPENGL_COMPAT);
   }

   if (options->lower_precision) {
      for (unsigned i = MESA_SHADER_VERTEX; i <= MESA_SHADER_COMPUTE; i++) {
         struct gl_shader_compiler_options *options =
            &ctx->Const.ShaderCompilerOptions[i];
         options->LowerPrecisionFloat16 = true;
         options->LowerPrecisionInt16 = true;
         options->LowerPrecisionDerivatives = true;
         options->LowerPrecisionConstants = true;
         options->LowerPrecisionFloat16Uniforms = true;
         options->LowerPrecision16BitLoadDst = true;
      }
   }

   struct gl_shader_program *whole_program = standalone_create_shader_program();

   for (unsigned i = 0; i < num_files; i++) {
      const unsigned len = strlen(files[i]);
      if (len < 6)
         goto fail;

      const char *const ext = & files[i][len - 5];
      /* TODO add support to read a .shader_test */
      GLenum type;
      if (strncmp(".vert", ext, 5) == 0 || strncmp(".glsl", ext, 5) == 0)
         type = GL_VERTEX_SHADER;
      else if (strncmp(".tesc", ext, 5) == 0)
         type = GL_TESS_CONTROL_SHADER;
      else if (strncmp(".tese", ext, 5) == 0)
         type = GL_TESS_EVALUATION_SHADER;
      else if (strncmp(".geom", ext, 5) == 0)
         type = GL_GEOMETRY_SHADER;
      else if (strncmp(".frag", ext, 5) == 0)
         type = GL_FRAGMENT_SHADER;
      else if (strncmp(".comp", ext, 5) == 0)
         type = GL_COMPUTE_SHADER;
      else
         goto fail;

      const char *source = load_text_file(whole_program, files[i]);
      if (source == NULL) {
         printf("File \"%s\" does not exist.\n", files[i]);
         exit(EXIT_FAILURE);
      }

      struct gl_shader *shader = standalone_add_shader_source(ctx, whole_program, type, source);

      compile_shader(ctx, shader);

      if (strlen(shader->InfoLog) > 0) {
         if (!options->just_log)
            printf("Info log for %s:\n", files[i]);

         printf("%s", shader->InfoLog);
         if (!options->just_log)
            printf("\n");
      }

      if (!shader->CompileStatus) {
         status = EXIT_FAILURE;
         break;
      }
   }

   if (status == EXIT_SUCCESS && options->do_link) {
      _mesa_clear_shader_program_data(ctx, whole_program);

      whole_program->data->LinkStatus = LINKING_SUCCESS;
      link_shaders_init(ctx, whole_program);
      gl_nir_link_glsl(ctx, whole_program);

      status = (whole_program->data->LinkStatus) ? EXIT_SUCCESS : EXIT_FAILURE;

      if (strlen(whole_program->data->InfoLog) > 0) {
         printf("\n");
         if (!options->just_log)
            printf("Info log for linking:\n");
         printf("%s", whole_program->data->InfoLog);
         if (!options->just_log)
            printf("\n");
      }
   }

   return whole_program;

fail:
   for (unsigned i = 0; i < MESA_SHADER_STAGES; i++) {
      if (whole_program->_LinkedShaders[i])
         _mesa_delete_linked_shader(ctx, whole_program->_LinkedShaders[i]);
   }

   ralloc_free(whole_program);
   return NULL;
}

extern "C" void
standalone_compiler_cleanup(struct gl_shader_program *whole_program,
                            struct gl_context *ctx)
{
   standalone_destroy_shader_program(whole_program);

   free(ctx->screen);
   _mesa_glsl_builtin_functions_decref();
}
