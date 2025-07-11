/*
 * Copyright © 2015 Intel Corporation
 * SPDX-License-Identifier: MIT
 */

#include "tu_util.h"

#include <errno.h>
#include <stdarg.h>

#include "common/freedreno_rd_output.h"
#include "util/u_math.h"
#include "util/timespec.h"
#include "util/os_file_notify.h"
#include "vk_enum_to_str.h"

#include "tu_device.h"
#include "tu_pass.h"

static const struct debug_control tu_debug_options[] = {
   { "startup", TU_DEBUG_STARTUP },
   { "nir", TU_DEBUG_NIR },
   { "nobin", TU_DEBUG_NOBIN },
   { "sysmem", TU_DEBUG_SYSMEM },
   { "gmem", TU_DEBUG_GMEM },
   { "forcebin", TU_DEBUG_FORCEBIN },
   { "layout", TU_DEBUG_LAYOUT },
   { "noubwc", TU_DEBUG_NOUBWC },
   { "nomultipos", TU_DEBUG_NOMULTIPOS },
   { "nolrz", TU_DEBUG_NOLRZ },
   { "nolrzfc", TU_DEBUG_NOLRZFC },
   { "perf", TU_DEBUG_PERF },
   { "perfc", TU_DEBUG_PERFC },
   { "flushall", TU_DEBUG_FLUSHALL },
   { "syncdraw", TU_DEBUG_SYNCDRAW },
   { "push_consts_per_stage", TU_DEBUG_PUSH_CONSTS_PER_STAGE },
   { "rast_order", TU_DEBUG_RAST_ORDER },
   { "unaligned_store", TU_DEBUG_UNALIGNED_STORE },
   { "log_skip_gmem_ops", TU_DEBUG_LOG_SKIP_GMEM_OPS },
   { "dynamic", TU_DEBUG_DYNAMIC },
   { "bos", TU_DEBUG_BOS },
   { "3d_load", TU_DEBUG_3D_LOAD },
   { "fdm", TU_DEBUG_FDM },
   { "noconform", TU_DEBUG_NOCONFORM },
   { "rd", TU_DEBUG_RD },
   { "hiprio", TU_DEBUG_HIPRIO },
   { "noconcurrentresolves", TU_DEBUG_NO_CONCURRENT_RESOLVES },
   { "noconcurrentunresolves", TU_DEBUG_NO_CONCURRENT_UNRESOLVES },
   { "dumpas", TU_DEBUG_DUMPAS },
   { "nobinmerging", TU_DEBUG_NO_BIN_MERGING },
   { "perfcraw", TU_DEBUG_PERFCRAW },
   { "fdmoffset", TU_DEBUG_FDM_OFFSET },
   { "check_cmd_buffer_status", TU_DEBUG_CHECK_CMD_BUFFER_STATUS },
   { "comm", TU_DEBUG_COMM },
   { "nofdm", TU_DEBUG_NOFDM },
   { NULL, 0 }
};

/*
 * The runtime debug flags are a subset of the debug flags that can be set at
 * runtime. Flags which depend on running state of the driver, the application
 * or the hardware and would otherwise break when toggled should not be set here.
 * Note: Keep in sync with the list of flags in 'docs/drivers/freedreno.rst'.
 */
const uint64_t tu_runtime_debug_flags =
   TU_DEBUG_NIR | TU_DEBUG_NOBIN | TU_DEBUG_SYSMEM | TU_DEBUG_GMEM |
   TU_DEBUG_FORCEBIN | TU_DEBUG_LAYOUT | TU_DEBUG_NOLRZ | TU_DEBUG_NOLRZFC |
   TU_DEBUG_PERF | TU_DEBUG_FLUSHALL | TU_DEBUG_SYNCDRAW |
   TU_DEBUG_RAST_ORDER | TU_DEBUG_UNALIGNED_STORE |
   TU_DEBUG_LOG_SKIP_GMEM_OPS | TU_DEBUG_3D_LOAD | TU_DEBUG_FDM |
   TU_DEBUG_NO_CONCURRENT_RESOLVES | TU_DEBUG_NO_CONCURRENT_UNRESOLVES |
   TU_DEBUG_NO_BIN_MERGING;

os_file_notifier_t tu_debug_notifier;
struct tu_env tu_env;

static void
tu_env_notify(
   void *data, const char *path, bool created, bool deleted, bool dir_deleted)
{
   uint64_t file_flags = 0;
   if (!deleted) {
      FILE *file = fopen(path, "r");
      if (file) {
         char buf[512];
         size_t len = fread(buf, 1, sizeof(buf) - 1, file);
         fclose(file);
         buf[len] = '\0';

         file_flags = parse_debug_string(buf, tu_debug_options);
      }
   }

   uint64_t runtime_flags = file_flags & tu_runtime_debug_flags;
   if (unlikely(runtime_flags != file_flags)) {
      mesa_logw(
         "Certain options in TU_DEBUG_FILE don't support runtime changes: 0x%" PRIx64 ", ignoring",
         file_flags & ~tu_runtime_debug_flags);
   }

   tu_env.debug.store(runtime_flags | tu_env.env_debug, std::memory_order_release);

   if (unlikely(dir_deleted))
      mesa_logw(
         "Directory containing TU_DEBUG_FILE (%s) was deleted, stopping watching",
         path);
}

static void
tu_env_deinit(void)
{
   if (tu_debug_notifier)
      os_file_notifier_destroy(tu_debug_notifier);
}

static void
tu_env_init_once(void)
{
   tu_env.debug = parse_debug_string(os_get_option("TU_DEBUG"), tu_debug_options);
   tu_env.env_debug = tu_env.debug & ~tu_runtime_debug_flags;

   if (TU_DEBUG(STARTUP))
      mesa_logi("TU_DEBUG=0x%" PRIx64 " (ENV: 0x%" PRIx64 ")", tu_env.debug.load(), tu_env.env_debug);

   /* TU_DEBUG=rd functionality was moved to fd_rd_output. This debug option
    * should translate to the basic-level FD_RD_DUMP_ENABLE option.
    */
   if (TU_DEBUG(RD))
      fd_rd_dump_env.flags |= FD_RD_DUMP_ENABLE;

   const char *debug_file = os_get_option("TU_DEBUG_FILE");
   if (debug_file) {
      if (tu_env.debug != tu_env.env_debug) {
         mesa_logw("TU_DEBUG_FILE is set (%s), but TU_DEBUG is also set. "
                   "Any runtime options (0x%" PRIx64 ") in TU_DEBUG will be ignored.",
                   debug_file, tu_env.debug & tu_runtime_debug_flags);
      }

      if (TU_DEBUG(STARTUP))
         mesa_logi("Watching TU_DEBUG_FILE: %s", debug_file);

      const char* error_str = "Unknown error";
      tu_debug_notifier =
         os_file_notifier_create(debug_file, tu_env_notify, NULL, &error_str);
      if (!tu_debug_notifier)
         mesa_logw("Failed to watch TU_DEBUG_FILE (%s): %s", debug_file, error_str);
   } else {
      tu_debug_notifier = NULL;
   }

   atexit(tu_env_deinit);
}

void
tu_env_init(void)
{
   fd_rd_dump_env_init();

   static once_flag once = ONCE_FLAG_INIT;
   call_once(&once, tu_env_init_once);
}

const char *
tu_env_debug_as_string(void)
{
   static thread_local char debug_string[96];
   dump_debug_control_string(debug_string, sizeof(debug_string),
                             tu_debug_options,
                             tu_env.debug.load(std::memory_order_acquire));
   return debug_string;
}

void PRINTFLIKE(3, 4)
   __tu_finishme(const char *file, int line, const char *format, ...)
{
   va_list ap;
   char buffer[256];

   va_start(ap, format);
   vsnprintf(buffer, sizeof(buffer), format, ap);
   va_end(ap);

   mesa_loge("%s:%d: FINISHME: %s\n", file, line, buffer);
}

VkResult
__vk_startup_errorf(struct tu_instance *instance,
                    VkResult error,
                    const char *file,
                    int line,
                    const char *format,
                    ...)
{
   va_list ap;
   char buffer[256];

   const char *error_str = vk_Result_to_str(error);

   if (format) {
      va_start(ap, format);
      vsnprintf(buffer, sizeof(buffer), format, ap);
      va_end(ap);

      mesa_loge("%s:%d: %s (%s)\n", file, line, buffer, error_str);
   } else {
      mesa_loge("%s:%d: %s\n", file, line, error_str);
   }

   return error;
}

static void
tu_tiling_config_update_tile_layout(struct tu_framebuffer *fb,
                                    const struct tu_device *dev,
                                    const struct tu_render_pass *pass,
                                    enum tu_gmem_layout gmem_layout)
{
   const uint32_t tile_align_w = pass->tile_align_w;
   uint32_t tile_align_h = dev->physical_device->info->tile_align_h;
   struct tu_tiling_config *tiling = &fb->tiling[gmem_layout];

   *tiling = (struct tu_tiling_config) {
      /* Put in dummy values that will assertion fail in register setup using
       * them, since you shouldn't be doing gmem work if gmem is not possible.
       */
      .tile0 = (VkExtent2D) { ~0, ~0 },
      .possible = false,
      .vsc = {
         .tile_count = (VkExtent2D) { .width = 1, .height = 1 },
      },
   };

   /* From the Vulkan 1.3.232 spec, under VkFramebufferCreateInfo:
    *
    *   If the render pass uses multiview, then layers must be one and each
    *   attachment requires a number of layers that is greater than the
    *   maximum bit index set in the view mask in the subpasses in which it is
    *   used.
    */

   uint32_t layers = MAX2(fb->layers, pass->num_views);

   /* If there is more than one layer, we need to make sure that the layer
    * stride is expressible as an offset in RB_RESOLVE_GMEM_BUFFER_BASE which ignores
    * the low 12 bits. The layer stride seems to be implicitly calculated from
    * the tile width and height so we need to adjust one of them.
    */
   const uint32_t gmem_align_log2 = 12;
   const uint32_t gmem_align = 1 << gmem_align_log2;
   uint32_t min_layer_stride = tile_align_h * tile_align_w * pass->min_cpp;
   if (layers > 1 && align(min_layer_stride, gmem_align) != min_layer_stride) {
      /* Make sure that min_layer_stride is a multiple of gmem_align. Because
       * gmem_align is a power of two and min_layer_stride isn't already a
       * multiple of gmem_align, this is equivalent to shifting tile_align_h
       * until the number of 0 bits at the bottom of min_layer_stride is at
       * least gmem_align_log2.
       */
      tile_align_h <<= gmem_align_log2 - (ffs(min_layer_stride) - 1);

      /* Check that we did the math right. */
      min_layer_stride = tile_align_h * tile_align_w * pass->min_cpp;
      assert(align(min_layer_stride, gmem_align) == min_layer_stride);
   }

   /* will force to sysmem, don't bother trying to have a valid tile config
    * TODO: just skip all GMEM stuff when sysmem is forced?
    */
   if (!pass->gmem_pixels[gmem_layout])
      return;

   uint32_t best_tile_count = ~0;
   VkExtent2D tile_count;
   VkExtent2D tile_size;
   /* There aren't that many different tile widths possible, so just walk all
    * of them finding which produces the lowest number of bins.
    */
   const uint32_t max_tile_width = MIN2(
      dev->physical_device->info->tile_max_w, util_align_npot(fb->width, tile_align_w));
   const uint32_t max_tile_height =
      MIN2(dev->physical_device->info->tile_max_h,
           align(fb->height, tile_align_h));
   for (tile_size.width = tile_align_w; tile_size.width <= max_tile_width;
        tile_size.width += tile_align_w) {
      tile_size.height = pass->gmem_pixels[gmem_layout] / (tile_size.width * layers);
      tile_size.height = MIN2(tile_size.height, max_tile_height);
      tile_size.height = ROUND_DOWN_TO(tile_size.height, tile_align_h);
      if (!tile_size.height)
         continue;

      tile_count.width = DIV_ROUND_UP(fb->width, tile_size.width);
      tile_count.height = DIV_ROUND_UP(fb->height, tile_size.height);

      /* Drop the height of the tile down to split tiles more evenly across the
       * screen for a given tile count.
       */
      tile_size.height =
         align(DIV_ROUND_UP(fb->height, tile_count.height), tile_align_h);

      /* Pick the layout with the minimum number of bins (lowest CP overhead
       * and amount of cache flushing), but the most square tiles in the case
       * of a tie (likely highest cache locality).
       */
      if (tile_count.width * tile_count.height < best_tile_count ||
          (tile_count.width * tile_count.height == best_tile_count &&
           abs((int)(tile_size.width - tile_size.height)) <
              abs((int)(tiling->tile0.width - tiling->tile0.height)))) {
         tiling->possible = true;
         tiling->tile0 = tile_size;
         tiling->vsc.tile_count = tile_count;
         best_tile_count = tile_count.width * tile_count.height;
      }
   }

   /* If forcing binning, try to get at least 2 tiles in each direction. */
   if (TU_DEBUG(FORCEBIN) && tiling->possible) {
      if (tiling->vsc.tile_count.width == 1 && tiling->tile0.width != tile_align_w) {
         tiling->tile0.width = util_align_npot(DIV_ROUND_UP(tiling->tile0.width, 2), tile_align_w);
         tiling->vsc.tile_count.width = 2;
      }
      if (tiling->vsc.tile_count.height == 1 && tiling->tile0.height != tile_align_h) {
         tiling->tile0.height = align(DIV_ROUND_UP(tiling->tile0.height, 2), tile_align_h);
         tiling->vsc.tile_count.height = 2;
      }
   }
}

static bool
is_hw_binning_possible(const struct tu_vsc_config *vsc)
{
   /* Similar to older gens, # of tiles per pipe cannot be more than 32.
    * But there are no hangs with 16 or more tiles per pipe in either
    * X or Y direction, so that limit does not seem to apply.
    */
   uint32_t tiles_per_pipe = vsc->pipe0.width * vsc->pipe0.height;
   return tiles_per_pipe <= 32;
}

static void
tu_tiling_config_update_pipe_layout(struct tu_vsc_config *vsc,
                                    const struct tu_device *dev,
                                    bool fdm)
{
   const uint32_t max_pipe_count =
      dev->physical_device->info->num_vsc_pipes;

   /* If there is a fragment density map and bin merging is enabled, we will
    * likely be able to merge some bins. Bins can only be merged if they are
    * in the same visibility stream, so making the pipes cover too small an
    * area can prevent bin merging from happening. Maximize the size of each
    * pipe instead of minimizing it.
    */
   if (fdm && dev->physical_device->info->a6xx.has_bin_mask &&
       !TU_DEBUG(NO_BIN_MERGING)) {
      vsc->pipe0.width = 4;
      vsc->pipe0.height = 8;
      vsc->pipe_count.width =
         DIV_ROUND_UP(vsc->tile_count.width, vsc->pipe0.width);
      vsc->pipe_count.height =
         DIV_ROUND_UP(vsc->tile_count.height, vsc->pipe0.height);
      vsc->binning_possible =
         vsc->pipe_count.width * vsc->pipe_count.height <= max_pipe_count;
      return;
   }

   /* start from 1 tile per pipe */
   vsc->pipe0 = (VkExtent2D) {
      .width = 1,
      .height = 1,
   };
   vsc->pipe_count = vsc->tile_count;

   while (vsc->pipe_count.width * vsc->pipe_count.height > max_pipe_count) {
      if (vsc->pipe0.width < vsc->pipe0.height) {
         vsc->pipe0.width += 1;
         vsc->pipe_count.width =
            DIV_ROUND_UP(vsc->tile_count.width, vsc->pipe0.width);
      } else {
         vsc->pipe0.height += 1;
         vsc->pipe_count.height =
            DIV_ROUND_UP(vsc->tile_count.height, vsc->pipe0.height);
      }
   }

   vsc->binning_possible = is_hw_binning_possible(vsc);
}

static void
tu_tiling_config_update_pipes(struct tu_vsc_config *vsc,
                              const struct tu_device *dev)
{
   const uint32_t max_pipe_count =
      dev->physical_device->info->num_vsc_pipes;
   const uint32_t used_pipe_count =
      vsc->pipe_count.width * vsc->pipe_count.height;
   const VkExtent2D last_pipe = {
      .width = (vsc->tile_count.width - 1) % vsc->pipe0.width + 1,
      .height = (vsc->tile_count.height - 1) % vsc->pipe0.height + 1,
   };

   if (!vsc->binning_possible)
      return;

   assert(used_pipe_count <= max_pipe_count);
   assert(max_pipe_count <= ARRAY_SIZE(vsc->pipe_config));

   for (uint32_t y = 0; y < vsc->pipe_count.height; y++) {
      for (uint32_t x = 0; x < vsc->pipe_count.width; x++) {
         const uint32_t pipe_x = vsc->pipe0.width * x;
         const uint32_t pipe_y = vsc->pipe0.height * y;
         const uint32_t pipe_w = (x == vsc->pipe_count.width - 1)
                                    ? last_pipe.width
                                    : vsc->pipe0.width;
         const uint32_t pipe_h = (y == vsc->pipe_count.height - 1)
                                    ? last_pipe.height
                                    : vsc->pipe0.height;
         const uint32_t n = vsc->pipe_count.width * y + x;

         vsc->pipe_config[n] = A6XX_VSC_PIPE_CONFIG_REG_X(pipe_x) |
                                  A6XX_VSC_PIPE_CONFIG_REG_Y(pipe_y) |
                                  A6XX_VSC_PIPE_CONFIG_REG_W(pipe_w) |
                                  A6XX_VSC_PIPE_CONFIG_REG_H(pipe_h);
         vsc->pipe_sizes[n] = CP_SET_BIN_DATA5_0_VSC_SIZE(pipe_w * pipe_h);
      }
   }

   memset(vsc->pipe_config + used_pipe_count, 0,
          sizeof(uint32_t) * (max_pipe_count - used_pipe_count));
}

static void
tu_tiling_config_update_binning(struct tu_vsc_config *vsc, const struct tu_device *device)
{
   if (vsc->binning_possible) {
      vsc->binning = (vsc->tile_count.width * vsc->tile_count.height) > 2;

      if (TU_DEBUG(FORCEBIN))
         vsc->binning = true;
      if (TU_DEBUG(NOBIN))
         vsc->binning = false;
   } else {
      vsc->binning = false;
   }
}

void
tu_framebuffer_tiling_config(struct tu_framebuffer *fb,
                             const struct tu_device *device,
                             const struct tu_render_pass *pass)
{
   for (int gmem_layout = 0; gmem_layout < TU_GMEM_LAYOUT_COUNT; gmem_layout++) {
      struct tu_tiling_config *tiling = &fb->tiling[gmem_layout];
      tu_tiling_config_update_tile_layout(fb, device, pass,
                                          (enum tu_gmem_layout) gmem_layout);
      if (!tiling->possible)
         continue;

      struct tu_vsc_config *vsc = &tiling->vsc;
      tu_tiling_config_update_pipe_layout(vsc, device, pass->has_fdm);
      tu_tiling_config_update_pipes(vsc, device);
      tu_tiling_config_update_binning(vsc, device);

      if (pass->has_fdm) {
         struct tu_vsc_config *fdm_offset_vsc = &tiling->fdm_offset_vsc;
         fdm_offset_vsc->tile_count = (VkExtent2D) {
            vsc->tile_count.width + 1, vsc->tile_count.height + 1
         };
         tu_tiling_config_update_pipe_layout(fdm_offset_vsc, device, true);
         tu_tiling_config_update_pipes(fdm_offset_vsc, device);
         tu_tiling_config_update_binning(fdm_offset_vsc, device);
      }
   }
}

void
tu_dbg_log_gmem_load_store_skips(struct tu_device *device)
{
   static uint32_t last_skipped_loads = 0;
   static uint32_t last_skipped_stores = 0;
   static uint32_t last_total_loads = 0;
   static uint32_t last_total_stores = 0;
   static struct timespec last_time = {};

   pthread_mutex_lock(&device->submit_mutex);

   struct timespec current_time;
   clock_gettime(CLOCK_MONOTONIC, &current_time);

   if (timespec_sub_to_nsec(&current_time, &last_time) > 1000 * 1000 * 1000) {
      last_time = current_time;
   } else {
      pthread_mutex_unlock(&device->submit_mutex);
      return;
   }

   struct tu6_global *global = device->global_bo_map;

   uint32_t current_taken_loads = global->dbg_gmem_taken_loads;
   uint32_t current_taken_stores = global->dbg_gmem_taken_stores;
   uint32_t current_total_loads = global->dbg_gmem_total_loads;
   uint32_t current_total_stores = global->dbg_gmem_total_stores;

   uint32_t skipped_loads = current_total_loads - current_taken_loads;
   uint32_t skipped_stores = current_total_stores - current_taken_stores;

   uint32_t current_time_frame_skipped_loads = skipped_loads - last_skipped_loads;
   uint32_t current_time_frame_skipped_stores = skipped_stores - last_skipped_stores;

   uint32_t current_time_frame_total_loads = current_total_loads - last_total_loads;
   uint32_t current_time_frame_total_stores = current_total_stores - last_total_stores;

   mesa_logi("[GMEM] loads total: %u skipped: %.1f%%\n",
         current_time_frame_total_loads,
         current_time_frame_skipped_loads / (float) current_time_frame_total_loads * 100.f);
   mesa_logi("[GMEM] stores total: %u skipped: %.1f%%\n",
         current_time_frame_total_stores,
         current_time_frame_skipped_stores / (float) current_time_frame_total_stores * 100.f);

   last_skipped_loads = skipped_loads;
   last_skipped_stores = skipped_stores;
   last_total_loads = current_total_loads;
   last_total_stores = current_total_stores;

   pthread_mutex_unlock(&device->submit_mutex);
}
