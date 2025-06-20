/*
 * Copyright 2024 Intel Corporation
 * SPDX-License-Identifier: MIT
 */

#include "debug_archiver.h"

#include "tar.h"
#include "util/ralloc.h"
#include "util/u_debug.h"

#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

struct debug_archiver
{
   FILE *f;
   tar_writer *tw;
   char prefix[128];
   char *archive_path;
};

DEBUG_GET_ONCE_OPTION(mda_output_dir, "MDA_OUTPUT_DIR", ".")
DEBUG_GET_ONCE_OPTION(mda_prefix, "MDA_PREFIX", NULL)

static bool
ensure_output_dir(const char *dir)
{
   if (!dir || !*dir || !strcmp(dir, "."))
      return true;

   struct stat st;
   if (stat(dir, &st) == 0)
      return S_ISDIR(st.st_mode);

   return mkdir(dir, 0755) == 0;
}

debug_archiver *
debug_archiver_open(void *mem_ctx, const char *name, const char *info)
{
   debug_archiver *da = rzalloc(mem_ctx, debug_archiver);

   char *filename = ralloc_asprintf(mem_ctx, "%s.mda.tar", name);

   const char *output_dir = debug_get_option_mda_output_dir();
   const char *prefix     = debug_get_option_mda_prefix();

   if (!ensure_output_dir(output_dir)) {
      /* Fallback to current directory on failure. */
      output_dir = ".";
   }

   if (prefix) {
      /* Prefix shouldn't have any `/` characters. */
      if (strchr(prefix, '/')) {
         char *safe_prefix = ralloc_strdup(da, prefix);
         for (char *p = safe_prefix; *p; p++) {
            if (*p == '/')
               *p = '_';
         }
         prefix = safe_prefix;
      }
   }

   da->archive_path = ralloc_asprintf(da, "%s/%s%s%s",
                                      output_dir,
                                      prefix ? prefix : "",
                                      prefix ? "."    : "",
                                      filename);

   da->f = fopen(da->archive_path, "wb+");

   da->tw = rzalloc(da, tar_writer);
   tar_writer_init(da->tw, da->f);

   debug_archiver_set_prefix(da, "");

   tar_writer_start_file(da->tw, "mesa.txt");
   fprintf(da->f, "Mesa %s\n", info);
   tar_writer_finish_file(da->tw);

   return da;
}

void
debug_archiver_set_prefix(debug_archiver *da, const char *prefix)
{
   if (!prefix || !*prefix) {
      strcpy(da->prefix, "mda");
   } else {
      snprintf(da->prefix, ARRAY_SIZE(da->prefix) - 1, "mda/%s", prefix);
   }

   da->tw->prefix = da->prefix;
}

void
debug_archiver_write_file(debug_archiver *da,
                          const char *filename,
                          const char *data, unsigned size)
{
   tar_writer_start_file(da->tw, filename);
   fwrite(data, size, 1, da->tw->file);
   tar_writer_finish_file(da->tw);
}

FILE *
debug_archiver_start_file(debug_archiver *da, const char *filename)
{
   tar_writer_start_file(da->tw, filename);
   return da->f;
}

void
debug_archiver_finish_file(debug_archiver *da)
{
   tar_writer_finish_file(da->tw);
}

void
debug_archiver_close(debug_archiver *da)
{
   if (da != NULL) {
      fclose(da->f);
      ralloc_free(da);
   }
}
