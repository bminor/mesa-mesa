#
# Copyright © 2021 Google, Inc.
#
# SPDX-License-Identifier: MIT

from mako.template import Template
import sys
import argparse
from enum import Enum

def max_bitfield_val(high, low, shift):
    return ((1 << (high - low)) - 1) << shift


parser = argparse.ArgumentParser()
parser.add_argument('-p', '--import-path', required=True)
args = parser.parse_args()
sys.path.insert(0, args.import_path)

from a6xx import *


class CHIP(Enum):
    A2XX = 2
    A3XX = 3
    A4XX = 4
    A5XX = 5
    A6XX = 6
    A7XX = 7

class CCUColorCacheFraction(Enum):
    FULL = 0
    HALF = 1
    QUARTER = 2
    EIGHTH = 3


class State(object):
    def __init__(self):
        # List of unique device-info structs, multiple different GPU ids
        # can map to a single info struct in cases where the differences
        # are not sw visible, or the only differences are parameters
        # queried from the kernel (like GMEM size)
        self.gpu_infos = []

        # Table mapping GPU id to device-info struct
        self.gpus = {}

    def info_index(self, gpu_info):
        i = 0
        for info in self.gpu_infos:
            if gpu_info == info:
                return i
            i += 1
        raise Error("invalid info")

s = State()

def add_gpus(ids, info):
    for id in ids:
        s.gpus[id] = info

class GPUId(object):
    def __init__(self, gpu_id = None, chip_id = None, name=None):
        if chip_id is None:
            assert(gpu_id is not None)
            val = gpu_id
            core = int(val / 100)
            val -= (core * 100)
            major = int(val / 10)
            val -= (major * 10)
            minor = val
            chip_id = (core << 24) | (major << 16) | (minor << 8) | 0xff
        self.chip_id = chip_id
        if gpu_id is None:
            gpu_id = 0
        self.gpu_id = gpu_id
        if name is None:
            assert(gpu_id != 0)
            name = "FD%d" % gpu_id
        self.name = name

class Struct(object):
    """A helper class that stringifies itself to a 'C' struct initializer
    """
    def __str__(self):
        s = "{"
        for name, value in vars(self).items():
            s += "." + name + "=" + str(value) + ","
        return s + "}"

class GPUInfo(Struct):
    """Base class for any generation of adreno, consists of GMEM layout
       related parameters

       Note that tile_max_h is normally only constrained by corresponding
       bitfield size/shift (ie. VSC_BIN_SIZE, or similar), but tile_max_h
       tends to have lower limits, in which case a comment will describe
       the bitfield size/shift
    """
    def __init__(self, chip, gmem_align_w, gmem_align_h,
                 tile_align_w, tile_align_h,
                 tile_max_w, tile_max_h, num_vsc_pipes,
                 cs_shared_mem_size, num_sp_cores, wave_granularity, fibers_per_sp,
                 highest_bank_bit = 0, ubwc_swizzle = 0x7, macrotile_mode = 0,
                 threadsize_base = 64, max_waves = 16, compute_lb_size = 0):
        self.chip          = chip.value
        self.gmem_align_w  = gmem_align_w
        self.gmem_align_h  = gmem_align_h
        self.tile_align_w  = tile_align_w
        self.tile_align_h  = tile_align_h
        self.tile_max_w    = tile_max_w
        self.tile_max_h    = tile_max_h
        self.num_vsc_pipes = num_vsc_pipes
        self.cs_shared_mem_size = cs_shared_mem_size
        self.num_sp_cores  = num_sp_cores
        self.wave_granularity = wave_granularity
        self.fibers_per_sp = fibers_per_sp
        self.threadsize_base = threadsize_base
        self.max_waves     = max_waves
        self.highest_bank_bit = highest_bank_bit
        self.ubwc_swizzle = ubwc_swizzle
        self.macrotile_mode = macrotile_mode

        s.gpu_infos.append(self)


class A6xxGPUInfo(GPUInfo):
    """The a6xx generation has a lot more parameters, and is broken down
       into distinct sub-generations.  The template parameter avoids
       duplication of parameters that are unique to the sub-generation.
    """
    def __init__(self, chip, template, num_ccu,
                 tile_align_w, tile_align_h, num_vsc_pipes,
                 cs_shared_mem_size, wave_granularity, fibers_per_sp,
                 magic_regs, raw_magic_regs = None, highest_bank_bit = 15,
                 ubwc_swizzle = 0x6, macrotile_mode = 1,
                 threadsize_base = 64, max_waves = 16):
        if chip == CHIP.A6XX:
            tile_max_w   = 1024 # max_bitfield_val(5, 0, 5)
            tile_max_h   = max_bitfield_val(14, 8, 4) # 1008
            compute_lb_size = 0
        else:
            tile_max_w   = 1728
            tile_max_h   = 1728
            # on a7xx the compute_lb_size is 40KB for all known parts for now.
            # We have a parameter for it in case some low-end parts cut it down.
            compute_lb_size = 40 * 1024

        super().__init__(chip, gmem_align_w = 16, gmem_align_h = 4,
                         tile_align_w = tile_align_w,
                         tile_align_h = tile_align_h,
                         tile_max_w   = tile_max_w,
                         tile_max_h   = tile_max_h,
                         num_vsc_pipes = num_vsc_pipes,
                         cs_shared_mem_size = cs_shared_mem_size,
                         num_sp_cores = num_ccu, # The # of SP cores seems to always match # of CCU
                         wave_granularity   = wave_granularity,
                         fibers_per_sp      = fibers_per_sp,
                         highest_bank_bit = highest_bank_bit,
                         ubwc_swizzle = ubwc_swizzle,
                         macrotile_mode = macrotile_mode,
                         threadsize_base    = threadsize_base,
                         max_waves    = max_waves,
                         compute_lb_size = compute_lb_size)

        self.num_ccu = num_ccu

        self.a6xx = Struct()
        self.a7xx = Struct()

        self.a6xx.magic = Struct()

        for name, val in magic_regs.items():
            setattr(self.a6xx.magic, name, val)

        if raw_magic_regs:
            self.a6xx.magic_raw = [[int(r[0]), r[1]] for r in raw_magic_regs]

        templates = template if isinstance(template, list) else [template]
        for template in templates:
            template.apply_props(self)


    def __str__(self):
     return super(A6xxGPUInfo, self).__str__().replace('[', '{').replace("]", "}")


# a2xx is really two sub-generations, a20x and a22x, but we don't currently
# capture that in the device-info tables
add_gpus([
        GPUId(200),
        GPUId(201),
        GPUId(205),
        GPUId(220),
    ], GPUInfo(
        CHIP.A2XX,
        gmem_align_w = 32,  gmem_align_h = 32,
        tile_align_w = 32,  tile_align_h = 32,
        tile_max_w   = 512,
        tile_max_h   = ~0, # TODO
        num_vsc_pipes = 8,
        cs_shared_mem_size = 0,
        num_sp_cores = 0, # TODO
        wave_granularity = 2,
        fibers_per_sp = 0, # TODO
        threadsize_base = 8, # TODO: Confirm this
    ))

add_gpus([
        GPUId(305),
        GPUId(307),
        GPUId(320),
        GPUId(330),
        GPUId(chip_id=0x03000512, name="FD305B"),
        GPUId(chip_id=0x03000620, name="FD306A"),
    ], GPUInfo(
        CHIP.A3XX,
        gmem_align_w = 32,  gmem_align_h = 32,
        tile_align_w = 32,  tile_align_h = 32,
        tile_max_w   = 992, # max_bitfield_val(4, 0, 5)
        tile_max_h   = max_bitfield_val(9, 5, 5),
        num_vsc_pipes = 8,
        cs_shared_mem_size = 32 * 1024,
        num_sp_cores = 0, # TODO
        wave_granularity = 2,
        fibers_per_sp = 0, # TODO
        threadsize_base = 8,
    ))

add_gpus([
        GPUId(405),
        GPUId(420),
        GPUId(430),
    ], GPUInfo(
        CHIP.A4XX,
        gmem_align_w = 32,  gmem_align_h = 32,
        tile_align_w = 32,  tile_align_h = 32,
        tile_max_w   = 1024, # max_bitfield_val(4, 0, 5)
        tile_max_h   = max_bitfield_val(9, 5, 5),
        num_vsc_pipes = 8,
        cs_shared_mem_size = 32 * 1024,
        num_sp_cores = 0, # TODO
        wave_granularity = 2,
        fibers_per_sp = 0, # TODO
        threadsize_base = 32, # TODO: Confirm this
    ))

add_gpus([
        GPUId(505),
        GPUId(506),
        GPUId(508),
        GPUId(509),
    ], GPUInfo(
        CHIP.A5XX,
        gmem_align_w = 64,  gmem_align_h = 32,
        tile_align_w = 64,  tile_align_h = 32,
        tile_max_w   = 1024, # max_bitfield_val(7, 0, 5)
        tile_max_h   = max_bitfield_val(16, 9, 5),
        num_vsc_pipes = 16,
        cs_shared_mem_size = 32 * 1024,
        num_sp_cores = 1,
        wave_granularity = 2,
        fibers_per_sp = 64 * 16, # Lowest number that didn't fault on spillall fs-varying-array-mat4-col-row-rd.
        highest_bank_bit = 14,
        threadsize_base = 32,
    ))

add_gpus([
        GPUId(510),
        GPUId(512),
    ], GPUInfo(
        CHIP.A5XX,
        gmem_align_w = 64,  gmem_align_h = 32,
        tile_align_w = 64,  tile_align_h = 32,
        tile_max_w   = 1024, # max_bitfield_val(7, 0, 5)
        tile_max_h   = max_bitfield_val(16, 9, 5),
        num_vsc_pipes = 16,
        cs_shared_mem_size = 32 * 1024,
        num_sp_cores = 2,
        wave_granularity = 2,
        fibers_per_sp = 64 * 16, # Lowest number that didn't fault on spillall fs-varying-array-mat4-col-row-rd.
        highest_bank_bit = 14,
        threadsize_base = 32,
    ))

add_gpus([
        GPUId(530),
        GPUId(540),
    ], GPUInfo(
        CHIP.A5XX,
        gmem_align_w = 64,  gmem_align_h = 32,
        tile_align_w = 64,  tile_align_h = 32,
        tile_max_w   = 1024, # max_bitfield_val(7, 0, 5)
        tile_max_h   = max_bitfield_val(16, 9, 5),
        num_vsc_pipes = 16,
        cs_shared_mem_size = 32 * 1024,
        num_sp_cores = 4,
        wave_granularity = 2,
        fibers_per_sp = 64 * 16, # Lowest number that didn't fault on spillall fs-varying-array-mat4-col-row-rd.
        highest_bank_bit = 15,
        threadsize_base = 32,
    ))


class A6XXProps(dict):
    unique_props = dict()
    def apply_gen_props(self, gen, gpu_info):
        for name, val in self.items():
            setattr(getattr(gpu_info, gen), name, val)
            A6XXProps.unique_props[(name, gen)] = val

    def apply_props(self, gpu_info):
        self.apply_gen_props("a6xx", gpu_info)


class A7XXProps(A6XXProps):
    def apply_props(self, gpu_info):
        self.apply_gen_props("a7xx", gpu_info)


# Props could be modified with env var:
#  FD_DEV_FEATURES=%feature_name%=%value%:%feature_name%=%value%:...
# e.g.
#  FD_DEV_FEATURES=has_fs_tex_prefetch=0:max_sets=4

a6xx_base = A6XXProps(
        has_cp_reg_write = True,
        has_8bpp_ubwc = True,
        has_gmem_fast_clear = True,
        has_hw_multiview = True,
        has_fs_tex_prefetch = True,
        has_sampler_minmax = True,

        supports_double_threadsize = True,

        sysmem_per_ccu_depth_cache_size = 64 * 1024,
        sysmem_per_ccu_color_cache_size = 64 * 1024,
        gmem_ccu_color_cache_fraction = CCUColorCacheFraction.QUARTER.value,

        prim_alloc_threshold = 0x7,
        vs_max_inputs_count = 32,
        max_sets = 5,
        line_width_min = 1.0,
        line_width_max = 1.0,
        mov_half_shared_quirk = True,
    )


# a6xx and a7xx can be divided into distinct sub-generations, where certain
# device-info parameters are keyed to the sub-generation.  These templates
# reduce the copypaste

a6xx_gen1_low = A6XXProps(
        reg_size_vec4 = 48,
        instr_cache_size = 64,
        indirect_draw_wfm_quirk = True,
        depth_bounds_require_depth_test_quirk = True,

        has_gmem_fast_clear = False,
        has_hw_multiview = False,
        has_sampler_minmax = False,
        has_fs_tex_prefetch = False,
        sysmem_per_ccu_color_cache_size = 8 * 1024,
        sysmem_per_ccu_depth_cache_size = 8 * 1024,
        gmem_ccu_color_cache_fraction = CCUColorCacheFraction.HALF.value,
        vs_max_inputs_count = 16,
        supports_double_threadsize = False,
    )

a6xx_gen1 = A6XXProps(
        reg_size_vec4 = 96,
        instr_cache_size = 64,
        indirect_draw_wfm_quirk = True,
        depth_bounds_require_depth_test_quirk = True,
    )

a6xx_gen2 = A6XXProps(
        reg_size_vec4 = 96,
        instr_cache_size = 64, # TODO
        supports_multiview_mask = True,
        has_z24uint_s8uint = True,
        indirect_draw_wfm_quirk = True,
        depth_bounds_require_depth_test_quirk = True, # TODO: check if true
        has_dp2acc = False, # TODO: check if true
        has_8bpp_ubwc = False,
    )

a6xx_gen3 = A6XXProps(
        reg_size_vec4 = 64,
        # Blob limits it to 128 but we hang with 128
        instr_cache_size = 127,
        supports_multiview_mask = True,
        has_z24uint_s8uint = True,
        tess_use_shared = True,
        storage_16bit = True,
        has_tex_filter_cubic = True,
        has_separate_chroma_filter = True,
        has_sample_locations = True,
        has_8bpp_ubwc = False,
        has_dp2acc = True,
        has_lrz_dir_tracking = True,
        enable_lrz_fast_clear = True,
        lrz_track_quirk = True,
        has_lrz_feedback = True,
        has_per_view_viewport = True,
        has_scalar_alu = True,
        has_early_preamble = True,
        prede_nop_quirk = True,
    )

a6xx_gen4 = A6XXProps(
        reg_size_vec4 = 64,
        # Blob limits it to 128 but we hang with 128
        instr_cache_size = 127,
        supports_multiview_mask = True,
        has_z24uint_s8uint = True,
        tess_use_shared = True,
        storage_16bit = True,
        has_tex_filter_cubic = True,
        has_separate_chroma_filter = True,
        has_sample_locations = True,
        has_cp_reg_write = False,
        has_8bpp_ubwc = False,
        has_lpac = True,
        has_legacy_pipeline_shading_rate = True,
        has_getfiberid = True,
        has_movs = True,
        has_dp2acc = True,
        has_dp4acc = True,
        enable_lrz_fast_clear = True,
        has_lrz_dir_tracking = True,
        has_lrz_feedback = True,
        has_per_view_viewport = True,
        has_scalar_alu = True,
        has_isam_v = True,
        has_ssbo_imm_offsets = True,
        has_ubwc_linear_mipmap_fallback = True,
        # TODO: there seems to be a quirk where at least rcp can't be in an
        # early preamble. a660 at least is affected.
        #has_early_preamble = True,
        prede_nop_quirk = True,
        predtf_nop_quirk = True,
        has_sad = True,
    )

add_gpus([
        GPUId(605), # TODO: Test it, based only on libwrapfake dumps
        GPUId(608), # TODO: Test it, based only on libwrapfake dumps
        GPUId(610),
        GPUId(612), # TODO: Test it, based only on libwrapfake dumps
    ], A6xxGPUInfo(
        CHIP.A6XX,
        [a6xx_base, a6xx_gen1_low],
        num_ccu = 1,
        tile_align_w = 32,
        tile_align_h = 16,
        num_vsc_pipes = 16,
        cs_shared_mem_size = 16 * 1024,
        wave_granularity = 1,
        fibers_per_sp = 128 * 16,
        highest_bank_bit = 13,
        ubwc_swizzle = 0x7,
        macrotile_mode = 0,
        magic_regs = dict(
            PC_POWER_CNTL = 0,
            TPL1_DBG_ECO_CNTL = 0,
            GRAS_DBG_ECO_CNTL = 0,
            SP_CHICKEN_BITS = 0,
            UCHE_CLIENT_PF = 0x00000004,
            PC_MODE_CNTL = 0xf,
            SP_DBG_ECO_CNTL = 0x0,
            RB_DBG_ECO_CNTL = 0x04100000,
            RB_DBG_ECO_CNTL_blit = 0x04100000,
            HLSQ_DBG_ECO_CNTL = 0,
            RB_UNKNOWN_8E01 = 0x00000001,
            VPC_DBG_ECO_CNTL = 0x0,
            UCHE_UNKNOWN_0E12 = 0x10000000,
        ),
    ))

add_gpus([
        GPUId(615),
        GPUId(616),
        GPUId(618),
        GPUId(619),
    ], A6xxGPUInfo(
        CHIP.A6XX,
        [a6xx_base, a6xx_gen1],
        num_ccu = 1,
        tile_align_w = 32,
        tile_align_h = 32,
        num_vsc_pipes = 32,
        cs_shared_mem_size = 32 * 1024,
        wave_granularity = 2,
        fibers_per_sp = 128 * 16,
        highest_bank_bit = 14,
        macrotile_mode = 0,
        magic_regs = dict(
            PC_POWER_CNTL = 0,
            TPL1_DBG_ECO_CNTL = 0x00108000,
            GRAS_DBG_ECO_CNTL = 0x00000880,
            SP_CHICKEN_BITS = 0x00000430,
            UCHE_CLIENT_PF = 0x00000004,
            PC_MODE_CNTL = 0x1f,
            SP_DBG_ECO_CNTL = 0x0,
            RB_DBG_ECO_CNTL = 0x04100000,
            RB_DBG_ECO_CNTL_blit = 0x04100000,
            HLSQ_DBG_ECO_CNTL = 0x00080000,
            RB_UNKNOWN_8E01 = 0x00000001,
            VPC_DBG_ECO_CNTL = 0x0,
            UCHE_UNKNOWN_0E12 = 0x00000001
        )
    ))

add_gpus([
        GPUId(620),
    ], A6xxGPUInfo(
        CHIP.A6XX,
        [a6xx_base, a6xx_gen1],
        num_ccu = 1,
        tile_align_w = 32,
        tile_align_h = 16,
        num_vsc_pipes = 32,
        cs_shared_mem_size = 32 * 1024,
        wave_granularity = 2,
        fibers_per_sp = 128 * 16,
        magic_regs = dict(
            PC_POWER_CNTL = 0,
            TPL1_DBG_ECO_CNTL = 0x01008000,
            GRAS_DBG_ECO_CNTL = 0x0,
            SP_CHICKEN_BITS = 0x00000400,
            UCHE_CLIENT_PF = 0x00000004,
            PC_MODE_CNTL = 0x1f,
            SP_DBG_ECO_CNTL = 0x01000000,
            RB_DBG_ECO_CNTL = 0x04100000,
            RB_DBG_ECO_CNTL_blit = 0x04100000,
            HLSQ_DBG_ECO_CNTL = 0x0,
            RB_UNKNOWN_8E01 = 0x0,
            VPC_DBG_ECO_CNTL = 0x02000000,
            UCHE_UNKNOWN_0E12 = 0x00000001
        )
    ))

add_gpus([
        GPUId(chip_id=0xffff06020100, name="FD621"),
        GPUId(chip_id=0xffff06020300, name="Adreno623"),
    ], A6xxGPUInfo(
        CHIP.A6XX,
        [a6xx_base, a6xx_gen3, A6XXProps(lrz_track_quirk = False)],
        num_ccu = 2,
        tile_align_w = 96,
        tile_align_h = 16,
        num_vsc_pipes = 32,
        cs_shared_mem_size = 32 * 1024,
        wave_granularity = 2,
        fibers_per_sp = 128 * 2 * 16,
        magic_regs = dict(
            PC_POWER_CNTL = 0,
            # this seems to be a chicken bit that fixes cubic filtering:
            TPL1_DBG_ECO_CNTL = 0x01008000,
            GRAS_DBG_ECO_CNTL = 0x0,
            SP_CHICKEN_BITS = 0x00001400,
            # UCHE_CLIENT_PF = 0x00000004,
            PC_MODE_CNTL = 0x1f,
            SP_DBG_ECO_CNTL = 0x03000000,
            RB_DBG_ECO_CNTL = 0x04100000,
            RB_DBG_ECO_CNTL_blit = 0x04100000,
            HLSQ_DBG_ECO_CNTL = 0x0,
            RB_UNKNOWN_8E01 = 0x0,
            VPC_DBG_ECO_CNTL = 0x02000000,
            UCHE_UNKNOWN_0E12 = 0x00000001
        )
    ))

add_gpus([
        GPUId(630),
    ], A6xxGPUInfo(
        CHIP.A6XX,
        [a6xx_base, a6xx_gen1],
        num_ccu = 2,
        tile_align_w = 32,
        tile_align_h = 16,
        num_vsc_pipes = 32,
        cs_shared_mem_size = 32 * 1024,
        wave_granularity = 2,
        fibers_per_sp = 128 * 16,
        highest_bank_bit = 15,
        macrotile_mode = 0,
        magic_regs = dict(
            PC_POWER_CNTL = 1,
            TPL1_DBG_ECO_CNTL = 0x00108000,
            GRAS_DBG_ECO_CNTL = 0x00000880,
            SP_CHICKEN_BITS = 0x00001430,
            UCHE_CLIENT_PF = 0x00000004,
            PC_MODE_CNTL = 0x1f,
            SP_DBG_ECO_CNTL = 0x0,
            RB_DBG_ECO_CNTL = 0x04100000,
            RB_DBG_ECO_CNTL_blit = 0x05100000,
            HLSQ_DBG_ECO_CNTL = 0x00080000,
            RB_UNKNOWN_8E01 = 0x00000001,
            VPC_DBG_ECO_CNTL = 0x0,
            UCHE_UNKNOWN_0E12 = 0x10000001
        )
    ))

add_gpus([
        GPUId(640),
    ], A6xxGPUInfo(
        CHIP.A6XX,
        [a6xx_base, a6xx_gen2],
        num_ccu = 2,
        tile_align_w = 32,
        tile_align_h = 16,
        num_vsc_pipes = 32,
        cs_shared_mem_size = 32 * 1024,
        wave_granularity = 2,
        fibers_per_sp = 128 * 4 * 16,
        highest_bank_bit = 15,
        macrotile_mode = 0,
        magic_regs = dict(
            PC_POWER_CNTL = 1,
            TPL1_DBG_ECO_CNTL = 0x00008000,
            GRAS_DBG_ECO_CNTL = 0x0,
            SP_CHICKEN_BITS = 0x00000420,
            UCHE_CLIENT_PF = 0x00000004,
            PC_MODE_CNTL = 0x1f,
            SP_DBG_ECO_CNTL = 0x0,
            RB_DBG_ECO_CNTL = 0x04100000,
            RB_DBG_ECO_CNTL_blit = 0x04100000,
            HLSQ_DBG_ECO_CNTL = 0x0,
            RB_UNKNOWN_8E01 = 0x00000001,
            VPC_DBG_ECO_CNTL = 0x02000000,
            UCHE_UNKNOWN_0E12 = 0x00000001
        )
    ))

add_gpus([
        GPUId(680),
    ], A6xxGPUInfo(
        CHIP.A6XX,
        [a6xx_base, a6xx_gen2],
        num_ccu = 4,
        tile_align_w = 64,
        tile_align_h = 32,
        num_vsc_pipes = 32,
        cs_shared_mem_size = 32 * 1024,
        wave_granularity = 2,
        fibers_per_sp = 128 * 4 * 16,
        highest_bank_bit = 15,
        macrotile_mode = 0,
        magic_regs = dict(
            PC_POWER_CNTL = 3,
            TPL1_DBG_ECO_CNTL = 0x00108000,
            GRAS_DBG_ECO_CNTL = 0x0,
            SP_CHICKEN_BITS = 0x00001430,
            UCHE_CLIENT_PF = 0x00000004,
            PC_MODE_CNTL = 0x1f,
            SP_DBG_ECO_CNTL = 0x0,
            RB_DBG_ECO_CNTL = 0x04100000,
            RB_DBG_ECO_CNTL_blit = 0x04100000,
            HLSQ_DBG_ECO_CNTL = 0x0,
            RB_UNKNOWN_8E01 = 0x00000001,
            VPC_DBG_ECO_CNTL = 0x02000000,
            UCHE_UNKNOWN_0E12 = 0x00000001
        )
    ))

add_gpus([
        GPUId(650),
    ], A6xxGPUInfo(
        CHIP.A6XX,
        [a6xx_base, a6xx_gen3],
        num_ccu = 3,
        tile_align_w = 96,
        tile_align_h = 16,
        num_vsc_pipes = 32,
        cs_shared_mem_size = 32 * 1024,
        wave_granularity = 2,
        fibers_per_sp = 128 * 2 * 16,
        highest_bank_bit = 16,
        magic_regs = dict(
            PC_POWER_CNTL = 2,
            # this seems to be a chicken bit that fixes cubic filtering:
            TPL1_DBG_ECO_CNTL = 0x01008000,
            GRAS_DBG_ECO_CNTL = 0x0,
            SP_CHICKEN_BITS = 0x00001400,
            UCHE_CLIENT_PF = 0x00000004,
            PC_MODE_CNTL = 0x1f,
            SP_DBG_ECO_CNTL = 0x01000000,
            RB_DBG_ECO_CNTL = 0x04100000,
            RB_DBG_ECO_CNTL_blit = 0x04100000,
            HLSQ_DBG_ECO_CNTL = 0x0,
            RB_UNKNOWN_8E01 = 0x0,
            VPC_DBG_ECO_CNTL = 0x02000000,
            UCHE_UNKNOWN_0E12 = 0x00000001
        )
    ))

add_gpus([
        # These are all speedbins/variants of A635
        GPUId(chip_id=0x00be06030500, name="Adreno 8c Gen 3"),
        GPUId(chip_id=0x007506030500, name="Adreno 7c+ Gen 3"),
        GPUId(chip_id=0x006006030500, name="Adreno 7c+ Gen 3 Lite"),
        GPUId(chip_id=0x00ac06030500, name="FD643"), # e.g. QCM6490, Fairphone 5
        # fallback wildcard entry should be last:
        GPUId(chip_id=0xffff06030500, name="Adreno 7c+ Gen 3"),
    ], A6xxGPUInfo(
        CHIP.A6XX,
        [a6xx_base, a6xx_gen4],
        num_ccu = 2,
        tile_align_w = 32,
        tile_align_h = 16,
        num_vsc_pipes = 32,
        cs_shared_mem_size = 32 * 1024,
        wave_granularity = 2,
        fibers_per_sp = 128 * 2 * 16,
        highest_bank_bit = 14,
        magic_regs = dict(
            PC_POWER_CNTL = 1,
            TPL1_DBG_ECO_CNTL = 0x05008000,
            GRAS_DBG_ECO_CNTL = 0x0,
            SP_CHICKEN_BITS = 0x00001400,
            UCHE_CLIENT_PF = 0x00000084,
            PC_MODE_CNTL = 0x1f,
            SP_DBG_ECO_CNTL = 0x00000006,
            RB_DBG_ECO_CNTL = 0x04100000,
            RB_DBG_ECO_CNTL_blit = 0x04100000,
            HLSQ_DBG_ECO_CNTL = 0x0,
            RB_UNKNOWN_8E01 = 0x0,
            VPC_DBG_ECO_CNTL = 0x02000000,
            UCHE_UNKNOWN_0E12 = 0x00000001
        )
    ))

add_gpus([
        GPUId(660),
    ], A6xxGPUInfo(
        CHIP.A6XX,
        [a6xx_base, a6xx_gen4],
        num_ccu = 3,
        tile_align_w = 96,
        tile_align_h = 16,
        num_vsc_pipes = 32,
        cs_shared_mem_size = 32 * 1024,
        wave_granularity = 2,
        fibers_per_sp = 128 * 2 * 16,
        highest_bank_bit = 16,
        magic_regs = dict(
            PC_POWER_CNTL = 2,
            TPL1_DBG_ECO_CNTL = 0x05008000,
            GRAS_DBG_ECO_CNTL = 0x0,
            SP_CHICKEN_BITS = 0x00001400,
            UCHE_CLIENT_PF = 0x00000084,
            PC_MODE_CNTL = 0x1f,
            SP_DBG_ECO_CNTL = 0x01000000,
            RB_DBG_ECO_CNTL = 0x04100000,
            RB_DBG_ECO_CNTL_blit = 0x04100000,
            HLSQ_DBG_ECO_CNTL = 0x0,
            RB_UNKNOWN_8E01 = 0x0,
            VPC_DBG_ECO_CNTL = 0x02000000,
            UCHE_UNKNOWN_0E12 = 0x00000001
        )
    ))

add_gpus([
        GPUId(chip_id=0x6060201, name="FD644"), # Called A662 in kgsl
        GPUId(chip_id=0xffff06060300, name="FD663"),
    ], A6xxGPUInfo(
        CHIP.A6XX,
        [a6xx_base, a6xx_gen4],
        num_ccu = 3,
        tile_align_w = 96,
        tile_align_h = 16,
        num_vsc_pipes = 32,
        cs_shared_mem_size = 32 * 1024,
        wave_granularity = 2,
        fibers_per_sp = 128 * 4 * 16,
        magic_regs = dict(
            PC_POWER_CNTL = 2,
            TPL1_DBG_ECO_CNTL = 0x05008000,
            GRAS_DBG_ECO_CNTL = 0x0,
            SP_CHICKEN_BITS = 0x00001400,
            UCHE_CLIENT_PF = 0x00000084,
            PC_MODE_CNTL = 0x1f,
            SP_DBG_ECO_CNTL = 0x6,
            RB_DBG_ECO_CNTL = 0x04100000,
            RB_DBG_ECO_CNTL_blit = 0x04100000,
            HLSQ_DBG_ECO_CNTL = 0x0,
            RB_UNKNOWN_8E01 = 0x0,
            VPC_DBG_ECO_CNTL = 0x02000000,
            UCHE_UNKNOWN_0E12 = 0x00000001
        )
    ))

add_gpus([
        GPUId(690),
        GPUId(chip_id=0xffff06090000, name="FD690"), # Default no-speedbin fallback
    ], A6xxGPUInfo(
        CHIP.A6XX,
        [a6xx_base, a6xx_gen4, A6XXProps(broken_ds_ubwc_quirk = True)],
        num_ccu = 8,
        tile_align_w = 64,
        tile_align_h = 32,
        num_vsc_pipes = 32,
        cs_shared_mem_size = 32 * 1024,
        wave_granularity = 2,
        fibers_per_sp = 128 * 2 * 16,
        highest_bank_bit = 16,
        magic_regs = dict(
            PC_POWER_CNTL = 7,
            TPL1_DBG_ECO_CNTL = 0x04c00000,
            GRAS_DBG_ECO_CNTL = 0x0,
            SP_CHICKEN_BITS = 0x00001400,
            UCHE_CLIENT_PF = 0x00000084,
            PC_MODE_CNTL = 0x1f,
            SP_DBG_ECO_CNTL = 0x1200000,
            RB_DBG_ECO_CNTL = 0x100000,
            RB_DBG_ECO_CNTL_blit = 0x00100000,  # ???
            HLSQ_DBG_ECO_CNTL = 0x0,
            RB_UNKNOWN_8E01 = 0x0,
            VPC_DBG_ECO_CNTL = 0x2000400,
            UCHE_UNKNOWN_0E12 = 0x00000001
        ),
        raw_magic_regs = [
            [A6XXRegs.REG_A6XX_SP_UNKNOWN_AAF2, 0x00c00000],
        ],
    ))

add_gpus([
        GPUId(702), # KGSL
        GPUId(chip_id=0x00b207002000, name="FD702"), # QRB2210 RB1
        GPUId(chip_id=0xffff07002000, name="FD702"), # Default no-speedbin fallback
    ], A6xxGPUInfo(
        CHIP.A6XX, # NOT a mistake!
        [a6xx_base, a6xx_gen1_low, A6XXProps(
            has_cp_reg_write = False,
            has_gmem_fast_clear = True,
            sysmem_per_ccu_depth_cache_size = 8 * 1024, # ??????
            sysmem_per_ccu_color_cache_size = 8 * 1024, # ??????
            gmem_ccu_color_cache_fraction = CCUColorCacheFraction.HALF.value,
            supports_double_threadsize = True,
            prim_alloc_threshold = 0x1,
            storage_16bit = True,
            is_a702 = True,
            )
        ],
        num_ccu = 1,
        tile_align_w = 32,
        tile_align_h = 16,
        num_vsc_pipes = 16,
        cs_shared_mem_size = 16 * 1024,
        wave_granularity = 1,
        fibers_per_sp = 128 * 16,
        threadsize_base = 16,
        max_waves = 16,
        # has_early_preamble = True,  # for VS/FS but not CS?
        magic_regs = dict(
            PC_POWER_CNTL = 0,
            TPL1_DBG_ECO_CNTL = 0x8000,
            GRAS_DBG_ECO_CNTL = 0,
            SP_CHICKEN_BITS = 0x1400,
            UCHE_CLIENT_PF = 0x84,
            PC_MODE_CNTL = 0xf,
            SP_DBG_ECO_CNTL = 0x0,
            RB_DBG_ECO_CNTL = 0x100000,
            RB_DBG_ECO_CNTL_blit = 0x100000,
            HLSQ_DBG_ECO_CNTL = 0x02000000,
            RB_UNKNOWN_8E01 = 0x1,
            VPC_DBG_ECO_CNTL = 0x0,
            UCHE_UNKNOWN_0E12 = 0x1,
        ),
    ))

# Based on a6xx_base + a6xx_gen4
a7xx_base = A6XXProps(
        has_gmem_fast_clear = True,
        has_hw_multiview = True,
        has_fs_tex_prefetch = True,
        has_sampler_minmax = True,

        supports_double_threadsize = True,

        sysmem_per_ccu_depth_cache_size = 256 * 1024,
        sysmem_per_ccu_color_cache_size = 64 * 1024,
        gmem_ccu_color_cache_fraction = CCUColorCacheFraction.EIGHTH.value,

        prim_alloc_threshold = 0x7,
        vs_max_inputs_count = 32,
        max_sets = 8,

        reg_size_vec4 = 96,
        # Blob limits it to 128 but we hang with 128
        instr_cache_size = 127,
        supports_multiview_mask = True,
        has_z24uint_s8uint = True,
        tess_use_shared = True,
        storage_16bit = True,
        has_tex_filter_cubic = True,
        has_separate_chroma_filter = True,
        has_sample_locations = True,
        has_lpac = True,
        has_getfiberid = True,
        has_movs = True,
        has_dp2acc = True,
        has_dp4acc = True,
        enable_lrz_fast_clear = True,
        has_lrz_dir_tracking = True,
        has_lrz_feedback = True,
        has_per_view_viewport = True,
        line_width_min = 1.0,
        line_width_max = 127.5,
        has_scalar_alu = True,
        has_coherent_ubwc_flag_caches = True,
        has_isam_v = True,
        has_ssbo_imm_offsets = True,
        has_early_preamble = True,
        has_attachment_shading_rate = True,
        has_ubwc_linear_mipmap_fallback = True,
        prede_nop_quirk = True,
        predtf_nop_quirk = True,
        has_sad = True,
        has_bin_mask = True,
    )

a7xx_gen1 = A7XXProps(
        supports_uav_ubwc = True,
        fs_must_have_non_zero_constlen_quirk = True,
        enable_tp_ubwc_flag_hint = True,
        reading_shading_rate_requires_smask_quirk = True,
    )

a7xx_gen2 = A7XXProps(
        stsc_duplication_quirk = True,
        has_event_write_sample_count = True,
        ubwc_unorm_snorm_int_compatible = True,
        supports_uav_ubwc = True,
        fs_must_have_non_zero_constlen_quirk = True,
        # Most devices with a740 have blob v6xx which doesn't have
        # this hint set. Match them for better compatibility by default.
        enable_tp_ubwc_flag_hint = False,
        has_64b_ssbo_atomics = True,
        has_primitive_shading_rate = True,
        reading_shading_rate_requires_smask_quirk = True,
        has_ray_intersection = True,
    )

a7xx_gen3 = A7XXProps(
        has_event_write_sample_count = True,
        load_inline_uniforms_via_preamble_ldgk = True,
        load_shader_consts_via_preamble = True,
        has_gmem_vpc_attr_buf = True,
        sysmem_vpc_attr_buf_size = 0x20000,
        gmem_vpc_attr_buf_size = 0xc000,
        ubwc_unorm_snorm_int_compatible = True,
        supports_uav_ubwc = True,
        has_generic_clear = True,
        r8g8_faulty_fast_clear_quirk = True,
        gs_vpc_adjacency_quirk = True,
        storage_8bit = True,
        ubwc_all_formats_compatible = True,
        has_compliant_dp4acc = True,
        ubwc_coherency_quirk = True,
        has_persistent_counter = True,
        has_64b_ssbo_atomics = True,
        has_primitive_shading_rate = True,
        has_ray_intersection = True,
        has_sw_fuse = True,
        has_rt_workaround = True,
        has_alias_rt = True,
        has_abs_bin_mask = True,
        new_control_regs = True,
    )

a730_magic_regs = dict(
        TPL1_DBG_ECO_CNTL = 0x1000000,
        GRAS_DBG_ECO_CNTL = 0x800,
        SP_CHICKEN_BITS = 0x1440,
        UCHE_CLIENT_PF = 0x00000084,
        PC_MODE_CNTL = 0x0000003f, # 0x00001f1f in some tests
        SP_DBG_ECO_CNTL = 0x10000000,
        RB_DBG_ECO_CNTL = 0x00000000,
        RB_DBG_ECO_CNTL_blit = 0x00000000,  # is it even needed?
        RB_UNKNOWN_8E01 = 0x0,
        VPC_DBG_ECO_CNTL = 0x02000000,
        UCHE_UNKNOWN_0E12 = 0x3200000,

        RB_CCU_DBG_ECO_CNTL = 0x02080000,
    )

a730_raw_magic_regs = [
        [A6XXRegs.REG_A6XX_UCHE_CACHE_WAYS, 0x00840004],
        [A6XXRegs.REG_A6XX_TPL1_DBG_ECO_CNTL1, 0x00040724],

        [A6XXRegs.REG_A7XX_SP_CHICKEN_BITS_1, 0x00002400],
        [A6XXRegs.REG_A7XX_SP_CHICKEN_BITS_2, 0x00000000],
        [A6XXRegs.REG_A7XX_SP_CHICKEN_BITS_3, 0x00000000],
        [A6XXRegs.REG_A7XX_UCHE_UNKNOWN_0E10, 0x00000000],
        [A6XXRegs.REG_A7XX_UCHE_UNKNOWN_0E11, 0x00000040],
        [A6XXRegs.REG_A7XX_SP_HLSQ_DBG_ECO_CNTL, 0x00008000],
        [A6XXRegs.REG_A6XX_PC_DBG_ECO_CNTL, 0x20080000],
        [A6XXRegs.REG_A7XX_PC_UNKNOWN_9E24, 0x21fc7f00],
        [A6XXRegs.REG_A7XX_VFD_DBG_ECO_CNTL, 0x00000000],
        [A6XXRegs.REG_A7XX_SP_UNKNOWN_AE06, 0x00000000],
        [A6XXRegs.REG_A7XX_SP_UNKNOWN_AE6A, 0x00000000],
        [A6XXRegs.REG_A7XX_SP_UNKNOWN_AE6B, 0x00000080],
        [A6XXRegs.REG_A7XX_SP_UNKNOWN_AE73, 0x00000000],
        [A6XXRegs.REG_A7XX_SP_UNKNOWN_AB02, 0x00000000],
        [A6XXRegs.REG_A7XX_SP_UNKNOWN_AB01, 0x00000000],
        [A6XXRegs.REG_A7XX_SP_UNKNOWN_AB22, 0x00000000],
        [A6XXRegs.REG_A7XX_SP_UNKNOWN_B310, 0x00000000],

        [A6XXRegs.REG_A7XX_GRAS_UNKNOWN_8120, 0x09510840],
        [A6XXRegs.REG_A7XX_GRAS_UNKNOWN_8121, 0x00000a62],

        [A6XXRegs.REG_A7XX_SP_UNKNOWN_0CE2,   0x00000000],
        [A6XXRegs.REG_A7XX_SP_UNKNOWN_0CE2+1, 0x00000000],
        [A6XXRegs.REG_A7XX_SP_UNKNOWN_0CE4,   0x00000000],
        [A6XXRegs.REG_A7XX_SP_UNKNOWN_0CE4+1, 0x00000000],
        [A6XXRegs.REG_A7XX_SP_UNKNOWN_0CE6,   0x00000000],
        [A6XXRegs.REG_A7XX_SP_UNKNOWN_0CE6+1, 0x00000000],

        [A6XXRegs.REG_A7XX_GRAS_UNKNOWN_80A7, 0x00000000],

        [A6XXRegs.REG_A7XX_RB_UNKNOWN_8E79,   0x00000000],
        [A6XXRegs.REG_A7XX_RB_UNKNOWN_8899,   0x00000000],
        [A6XXRegs.REG_A7XX_RB_UNKNOWN_88F5,   0x00000000],
    ]

a740_magic_regs = dict(
        # PC_POWER_CNTL = 7,
        TPL1_DBG_ECO_CNTL = 0x11100000,
        GRAS_DBG_ECO_CNTL = 0x00004800,
        SP_CHICKEN_BITS = 0x10001400,
        UCHE_CLIENT_PF = 0x00000084,
        # Blob uses 0x1f or 0x1f1f, however these values cause vertices
        # corruption in some tests.
        PC_MODE_CNTL = 0x0000003f,
        SP_DBG_ECO_CNTL = 0x10000000,
        RB_DBG_ECO_CNTL = 0x00000000,
        RB_DBG_ECO_CNTL_blit = 0x00000000,  # is it even needed?
        # HLSQ_DBG_ECO_CNTL = 0x0,
        RB_UNKNOWN_8E01 = 0x0,
        VPC_DBG_ECO_CNTL = 0x02000000,
        UCHE_UNKNOWN_0E12 = 0x00000000,

        RB_CCU_DBG_ECO_CNTL = 0x02080000,
    )

a740_raw_magic_regs = [
        [A6XXRegs.REG_A6XX_UCHE_CACHE_WAYS, 0x00040004],
        [A6XXRegs.REG_A6XX_TPL1_DBG_ECO_CNTL1, 0x00040724],

        [A6XXRegs.REG_A7XX_SP_CHICKEN_BITS_1, 0x00000400],
        [A6XXRegs.REG_A7XX_SP_CHICKEN_BITS_2, 0x00430800],
        [A6XXRegs.REG_A7XX_SP_CHICKEN_BITS_3, 0x00000000],
        [A6XXRegs.REG_A7XX_UCHE_UNKNOWN_0E10, 0x00000000],
        [A6XXRegs.REG_A7XX_UCHE_UNKNOWN_0E11, 0x00000000],
        [A6XXRegs.REG_A7XX_SP_HLSQ_DBG_ECO_CNTL, 0x00000000],
        [A6XXRegs.REG_A6XX_PC_DBG_ECO_CNTL, 0x00100000],
        [A6XXRegs.REG_A7XX_PC_UNKNOWN_9E24, 0x21585600],
        [A6XXRegs.REG_A7XX_VFD_DBG_ECO_CNTL, 0x00008000],
        [A6XXRegs.REG_A7XX_SP_UNKNOWN_AE06, 0x00000000],
        [A6XXRegs.REG_A7XX_SP_UNKNOWN_AE6A, 0x00000000],
        [A6XXRegs.REG_A7XX_SP_UNKNOWN_AE6B, 0x00000080],
        [A6XXRegs.REG_A7XX_SP_UNKNOWN_AE73, 0x00000000],
        [A6XXRegs.REG_A7XX_SP_UNKNOWN_AB02, 0x00000000],
        [A6XXRegs.REG_A7XX_SP_UNKNOWN_AB01, 0x00000000],
        [A6XXRegs.REG_A7XX_SP_UNKNOWN_AB22, 0x00000000],
        [A6XXRegs.REG_A7XX_SP_UNKNOWN_B310, 0x00000000],

        [A6XXRegs.REG_A7XX_GRAS_UNKNOWN_8120, 0x09510840],
        [A6XXRegs.REG_A7XX_GRAS_UNKNOWN_8121, 0x00000a62],

        [A6XXRegs.REG_A7XX_GRAS_UNKNOWN_8009, 0x00000000],
        [A6XXRegs.REG_A7XX_GRAS_UNKNOWN_800A, 0x00000000],
        [A6XXRegs.REG_A7XX_GRAS_UNKNOWN_800B, 0x00000000],
        [A6XXRegs.REG_A7XX_GRAS_UNKNOWN_800C, 0x00000000],

        [A6XXRegs.REG_A7XX_SP_UNKNOWN_0CE2,   0x00000000],
        [A6XXRegs.REG_A7XX_SP_UNKNOWN_0CE2+1, 0x00000000],
        [A6XXRegs.REG_A7XX_SP_UNKNOWN_0CE4,   0x00000000],
        [A6XXRegs.REG_A7XX_SP_UNKNOWN_0CE4+1, 0x00000000],
        [A6XXRegs.REG_A7XX_SP_UNKNOWN_0CE6,   0x00000000],
        [A6XXRegs.REG_A7XX_SP_UNKNOWN_0CE6+1, 0x00000000],

        [A6XXRegs.REG_A7XX_GRAS_UNKNOWN_80A7, 0x00000000],

        [A6XXRegs.REG_A7XX_RB_UNKNOWN_8E79,   0x00000000],
        [A6XXRegs.REG_A7XX_RB_UNKNOWN_8899,   0x00000000],
        [A6XXRegs.REG_A7XX_RB_UNKNOWN_88F5,   0x00000000],
        [A6XXRegs.REG_A7XX_RB_UNKNOWN_8C34,   0x00000000],

        [A6XXRegs.REG_A7XX_GRAS_UNKNOWN_8008, 0x00000000],
    ]

add_gpus([
        # These are named as Adreno730v3 or Adreno725v1.
        GPUId(chip_id=0x07030002, name="FD725"),
        GPUId(chip_id=0xffff07030002, name="FD725"),
    ], A6xxGPUInfo(
        CHIP.A7XX,
        [a7xx_base, a7xx_gen1, A7XXProps(cmdbuf_start_a725_quirk = True)],
        num_ccu = 4,
        tile_align_w = 64,
        tile_align_h = 32,
        num_vsc_pipes = 32,
        cs_shared_mem_size = 32 * 1024,
        wave_granularity = 2,
        fibers_per_sp = 128 * 2 * 16,
        highest_bank_bit = 16,
        magic_regs = a730_magic_regs,
        raw_magic_regs = a730_raw_magic_regs,
    ))

add_gpus([
        GPUId(chip_id=0x07030001, name="FD730"), # KGSL, no speedbin data
        GPUId(chip_id=0xffff07030001, name="FD730"), # Default no-speedbin fallback
    ], A6xxGPUInfo(
        CHIP.A7XX,
        [a7xx_base, a7xx_gen1],
        num_ccu = 4,
        tile_align_w = 64,
        tile_align_h = 32,
        num_vsc_pipes = 32,
        cs_shared_mem_size = 32 * 1024,
        wave_granularity = 2,
        fibers_per_sp = 128 * 2 * 16,
        highest_bank_bit = 16,
        magic_regs = a730_magic_regs,
        raw_magic_regs = a730_raw_magic_regs,
    ))

add_gpus([
        GPUId(chip_id=0xffff43030c00, name="Adreno X1-45"),
        GPUId(chip_id=0x43030B00, name="FD735")
    ], A6xxGPUInfo(
        CHIP.A7XX,
        [a7xx_base, a7xx_gen2, A7XXProps(enable_tp_ubwc_flag_hint = True)],
        num_ccu = 3,
        tile_align_w = 96,
        tile_align_h = 32,
        num_vsc_pipes = 32,
        cs_shared_mem_size = 32 * 1024,
        wave_granularity = 2,
        fibers_per_sp = 128 * 2 * 16,
        magic_regs = dict(
            TPL1_DBG_ECO_CNTL = 0x11100000,
            GRAS_DBG_ECO_CNTL = 0x00004800,
            SP_CHICKEN_BITS = 0x10001400,
            UCHE_CLIENT_PF = 0x00000084,
            PC_MODE_CNTL = 0x0000001f,
            SP_DBG_ECO_CNTL = 0x10000000,
            RB_DBG_ECO_CNTL = 0x00000001,
            RB_DBG_ECO_CNTL_blit = 0x00000001,  # is it even needed?
            RB_UNKNOWN_8E01 = 0x0,
            VPC_DBG_ECO_CNTL = 0x02000000,
            UCHE_UNKNOWN_0E12 = 0x00000000,

            RB_CCU_DBG_ECO_CNTL = 0x02080000,
        ),
        raw_magic_regs = [
            [A6XXRegs.REG_A6XX_UCHE_CACHE_WAYS, 0x00000000],
            [A6XXRegs.REG_A6XX_TPL1_DBG_ECO_CNTL1, 0x00040724],

            [A6XXRegs.REG_A7XX_SP_CHICKEN_BITS_1, 0x00000400],
            [A6XXRegs.REG_A7XX_SP_CHICKEN_BITS_2, 0x00430800],
            [A6XXRegs.REG_A7XX_SP_CHICKEN_BITS_3, 0x00000000],
            [A6XXRegs.REG_A7XX_UCHE_UNKNOWN_0E10, 0x00000000],
            [A6XXRegs.REG_A7XX_UCHE_UNKNOWN_0E11, 0x00000000],
            [A6XXRegs.REG_A7XX_SP_HLSQ_DBG_ECO_CNTL, 0x00000000],
            [A6XXRegs.REG_A6XX_PC_DBG_ECO_CNTL, 0x00100000],
            [A6XXRegs.REG_A7XX_PC_UNKNOWN_9E24, 0x01585600],
            [A6XXRegs.REG_A7XX_VFD_DBG_ECO_CNTL, 0x00008000],
            [A6XXRegs.REG_A7XX_SP_UNKNOWN_AE06, 0x00000000],
            [A6XXRegs.REG_A7XX_SP_UNKNOWN_AE6A, 0x00000000],
            [A6XXRegs.REG_A7XX_SP_UNKNOWN_AE6B, 0x00000080],
            [A6XXRegs.REG_A7XX_SP_UNKNOWN_AE73, 0x00000000],
            [A6XXRegs.REG_A7XX_SP_UNKNOWN_AB02, 0x00000000],
            [A6XXRegs.REG_A7XX_SP_UNKNOWN_AB01, 0x00000000],
            [A6XXRegs.REG_A7XX_SP_UNKNOWN_AB22, 0x00000000],
            [A6XXRegs.REG_A7XX_SP_UNKNOWN_B310, 0x00000000],

            [A6XXRegs.REG_A7XX_GRAS_UNKNOWN_8120, 0x09510840],
            [A6XXRegs.REG_A7XX_GRAS_UNKNOWN_8121, 0x00000a62],

            [A6XXRegs.REG_A7XX_GRAS_UNKNOWN_8009, 0x00000000],
            [A6XXRegs.REG_A7XX_GRAS_UNKNOWN_800A, 0x00000000],
            [A6XXRegs.REG_A7XX_GRAS_UNKNOWN_800B, 0x00000000],
            [A6XXRegs.REG_A7XX_GRAS_UNKNOWN_800C, 0x00000000],

            [A6XXRegs.REG_A7XX_SP_UNKNOWN_0CE2,   0x00000000],
            [A6XXRegs.REG_A7XX_SP_UNKNOWN_0CE2+1, 0x00000000],
            [A6XXRegs.REG_A7XX_SP_UNKNOWN_0CE4,   0x00000000],
            [A6XXRegs.REG_A7XX_SP_UNKNOWN_0CE4+1, 0x00000000],
            [A6XXRegs.REG_A7XX_SP_UNKNOWN_0CE6,   0x00000000],
            [A6XXRegs.REG_A7XX_SP_UNKNOWN_0CE6+1, 0x00000000],

            [A6XXRegs.REG_A7XX_GRAS_UNKNOWN_80A7, 0x00000000],

            [A6XXRegs.REG_A7XX_RB_UNKNOWN_8E79,   0x00000000],
            [A6XXRegs.REG_A7XX_RB_UNKNOWN_8899,   0x00000000],
            [A6XXRegs.REG_A7XX_RB_UNKNOWN_88F5,   0x00000000],
            [A6XXRegs.REG_A7XX_RB_UNKNOWN_8C34,   0x00000000],

            [A6XXRegs.REG_A7XX_GRAS_UNKNOWN_8008, 0x00000000],
        ],
    ))

add_gpus([
        GPUId(740), # Deprecated, used for dev kernels.
        GPUId(chip_id=0x43050a01, name="FD740"), # KGSL, no speedbin data
        GPUId(chip_id=0xffff43050a01, name="FD740"), # Default no-speedbin fallback
        GPUId(chip_id=0xffff43050c01, name="Adreno X1-85"),
    ], A6xxGPUInfo(
        CHIP.A7XX,
        [a7xx_base, a7xx_gen2],
        num_ccu = 6,
        tile_align_w = 96,
        tile_align_h = 32,
        num_vsc_pipes = 32,
        cs_shared_mem_size = 32 * 1024,
        wave_granularity = 2,
        fibers_per_sp = 128 * 2 * 16,
        highest_bank_bit = 16,
        magic_regs = a740_magic_regs,
        raw_magic_regs = a740_raw_magic_regs,
    ))

# Values from blob v676.0
add_gpus([
        GPUId(chip_id=0x43050a00, name="FDA32"), # Adreno A32 (G3x Gen 2)
        GPUId(chip_id=0xffff43050a00, name="FDA32"),
    ], A6xxGPUInfo(
        CHIP.A7XX,
        [a7xx_base, a7xx_gen2, A7XXProps(cmdbuf_start_a725_quirk = True)],
        num_ccu = 6,
        tile_align_w = 96,
        tile_align_h = 32,
        num_vsc_pipes = 32,
        cs_shared_mem_size = 32 * 1024,
        wave_granularity = 2,
        fibers_per_sp = 128 * 2 * 16,
        magic_regs = a740_magic_regs,
        raw_magic_regs = [
            [A6XXRegs.REG_A6XX_UCHE_CACHE_WAYS, 0x00040004],
            [A6XXRegs.REG_A6XX_TPL1_DBG_ECO_CNTL1, 0x00000700],

            [A6XXRegs.REG_A7XX_SP_CHICKEN_BITS_1, 0x00000400],
            [A6XXRegs.REG_A7XX_SP_CHICKEN_BITS_2, 0x00430820],
            [A6XXRegs.REG_A7XX_SP_CHICKEN_BITS_3, 0x00000000],
            [A6XXRegs.REG_A7XX_UCHE_UNKNOWN_0E10, 0x00000000],
            [A6XXRegs.REG_A7XX_UCHE_UNKNOWN_0E11, 0x00000080],
            [A6XXRegs.REG_A7XX_SP_HLSQ_DBG_ECO_CNTL, 0x00000000],
            [A6XXRegs.REG_A6XX_PC_DBG_ECO_CNTL, 0x00100000],
            [A6XXRegs.REG_A7XX_PC_UNKNOWN_9E24, 0x21585600],
            [A6XXRegs.REG_A7XX_VFD_DBG_ECO_CNTL, 0x00008000],
            [A6XXRegs.REG_A7XX_SP_UNKNOWN_AE06, 0x00000000],
            [A6XXRegs.REG_A7XX_SP_UNKNOWN_AE6A, 0x00000000],
            [A6XXRegs.REG_A7XX_SP_UNKNOWN_AE6B, 0x00000080],
            [A6XXRegs.REG_A7XX_SP_UNKNOWN_AE73, 0x00000000],
            [A6XXRegs.REG_A7XX_SP_UNKNOWN_AB02, 0x00000000],
            [A6XXRegs.REG_A7XX_SP_UNKNOWN_AB01, 0x00000000],
            [A6XXRegs.REG_A7XX_SP_UNKNOWN_AB22, 0x00000000],
            [A6XXRegs.REG_A7XX_SP_UNKNOWN_B310, 0x00000000],

            [A6XXRegs.REG_A7XX_GRAS_UNKNOWN_8120, 0x09510840],
            [A6XXRegs.REG_A7XX_GRAS_UNKNOWN_8121, 0x00000a62],

            [A6XXRegs.REG_A7XX_GRAS_UNKNOWN_8009, 0x00000000],
            [A6XXRegs.REG_A7XX_GRAS_UNKNOWN_800A, 0x00000000],
            [A6XXRegs.REG_A7XX_GRAS_UNKNOWN_800B, 0x00000000],
            [A6XXRegs.REG_A7XX_GRAS_UNKNOWN_800C, 0x00000000],

            [A6XXRegs.REG_A7XX_SP_UNKNOWN_0CE2,   0x00000000],
            [A6XXRegs.REG_A7XX_SP_UNKNOWN_0CE2+1, 0x00000000],
            [A6XXRegs.REG_A7XX_SP_UNKNOWN_0CE4,   0x00000000],
            [A6XXRegs.REG_A7XX_SP_UNKNOWN_0CE4+1, 0x00000000],
            [A6XXRegs.REG_A7XX_SP_UNKNOWN_0CE6,   0x00000000],
            [A6XXRegs.REG_A7XX_SP_UNKNOWN_0CE6+1, 0x00000000],

            [A6XXRegs.REG_A7XX_GRAS_UNKNOWN_80A7, 0x00000000],

            [A6XXRegs.REG_A7XX_RB_UNKNOWN_8E79,   0x00000000],
            [A6XXRegs.REG_A7XX_RB_UNKNOWN_8899,   0x00000000],
            [A6XXRegs.REG_A7XX_RB_UNKNOWN_88F5,   0x00000000],
        ],
    ))

add_gpus([
        GPUId(chip_id=0x43050b00, name="FD740v3"), # Quest 3
        GPUId(chip_id=0xffff43050b00, name="FD740v3"),
    ], A6xxGPUInfo(
        CHIP.A7XX,
        [a7xx_base, a7xx_gen2, A7XXProps(enable_tp_ubwc_flag_hint = True)],
        num_ccu = 6,
        tile_align_w = 96,
        tile_align_h = 32,
        num_vsc_pipes = 32,
        cs_shared_mem_size = 32 * 1024,
        wave_granularity = 2,
        fibers_per_sp = 128 * 2 * 16,
        magic_regs = dict(
            # PC_POWER_CNTL = 7,
            TPL1_DBG_ECO_CNTL = 0x11100000,
            GRAS_DBG_ECO_CNTL = 0x00004800,
            SP_CHICKEN_BITS = 0x10001400,
            UCHE_CLIENT_PF = 0x00000084,
            # Blob uses 0x1f or 0x1f1f, however these values cause vertices
            # corruption in some tests.
            PC_MODE_CNTL = 0x0000003f,
            SP_DBG_ECO_CNTL = 0x10000000,
            RB_DBG_ECO_CNTL = 0x00000001,
            RB_DBG_ECO_CNTL_blit = 0x00000000,  # is it even needed?
            # HLSQ_DBG_ECO_CNTL = 0x0,
            RB_UNKNOWN_8E01 = 0x0,
            VPC_DBG_ECO_CNTL = 0x02000000,
            UCHE_UNKNOWN_0E12 = 0x00000000,

            RB_CCU_DBG_ECO_CNTL = 0x02080000,
        ),
        raw_magic_regs = a740_raw_magic_regs,
    ))

add_gpus([
        GPUId(chip_id=0x43051401, name="FD750"), # KGSL, no speedbin data
        GPUId(chip_id=0xffff43051401, name="FD750"), # Default no-speedbin fallback
    ], A6xxGPUInfo(
        CHIP.A7XX,
        [a7xx_base, a7xx_gen3],
        num_ccu = 6,
        tile_align_w = 96,
        tile_align_h = 32,
        num_vsc_pipes = 32,
        cs_shared_mem_size = 32 * 1024,
        wave_granularity = 2,
        fibers_per_sp = 128 * 2 * 16,
        highest_bank_bit = 16,
        magic_regs = dict(
            TPL1_DBG_ECO_CNTL = 0x11100000,
            GRAS_DBG_ECO_CNTL = 0x00004800,
            SP_CHICKEN_BITS = 0x10000400,
            PC_MODE_CNTL = 0x00003f1f,
            SP_DBG_ECO_CNTL = 0x10000000,
            RB_DBG_ECO_CNTL = 0x00000001,
            RB_DBG_ECO_CNTL_blit = 0x00000001,
            RB_UNKNOWN_8E01 = 0x0,
            VPC_DBG_ECO_CNTL = 0x02000000,
            UCHE_UNKNOWN_0E12 = 0x40000000,

            RB_CCU_DBG_ECO_CNTL = 0x02082000,
        ),
        raw_magic_regs = [
            [A6XXRegs.REG_A6XX_UCHE_CACHE_WAYS, 0x00000000],
            [A6XXRegs.REG_A7XX_UCHE_UNKNOWN_0E10, 0x00000000],
            [A6XXRegs.REG_A7XX_UCHE_UNKNOWN_0E11, 0x00000080],
            [A6XXRegs.REG_A7XX_SP_CHICKEN_BITS_1, 0x00000000],
            [A6XXRegs.REG_A7XX_SP_CHICKEN_BITS_2, 0x00431800],
            [A6XXRegs.REG_A7XX_SP_CHICKEN_BITS_3, 0x00800000],
            [A6XXRegs.REG_A7XX_SP_HLSQ_DBG_ECO_CNTL, 0x00000000],
            [A6XXRegs.REG_A6XX_PC_DBG_ECO_CNTL, 0x00100000],
            [A6XXRegs.REG_A7XX_PC_UNKNOWN_9E24, 0x01585600],
            [A6XXRegs.REG_A7XX_VFD_DBG_ECO_CNTL, 0x00008000],
            [A6XXRegs.REG_A7XX_SP_UNKNOWN_AE06, 0x00000000],
            [A6XXRegs.REG_A7XX_SP_UNKNOWN_AE6A, 0x00000000],
            [A6XXRegs.REG_A7XX_SP_UNKNOWN_AE6B, 0x00000080],
            [A6XXRegs.REG_A7XX_SP_UNKNOWN_AE73, 0x00000000],
            [A6XXRegs.REG_A7XX_SP_UNKNOWN_AB02, 0x00000000],
            [A6XXRegs.REG_A7XX_SP_UNKNOWN_AB01, 0x00000000],
            [A6XXRegs.REG_A7XX_SP_UNKNOWN_AB22, 0x00000000],
            [A6XXRegs.REG_A7XX_SP_UNKNOWN_B310, 0x00000000],
            [A6XXRegs.REG_A7XX_GRAS_UNKNOWN_8120, 0x09510840],
            [A6XXRegs.REG_A7XX_GRAS_UNKNOWN_8121, 0x00000a62],
            [A6XXRegs.REG_A7XX_GRAS_UNKNOWN_8009, 0x00000000],
            [A6XXRegs.REG_A7XX_GRAS_UNKNOWN_800A, 0x00000000],
            [A6XXRegs.REG_A7XX_GRAS_UNKNOWN_800B, 0x00000000],
            [A6XXRegs.REG_A7XX_GRAS_UNKNOWN_800C, 0x00000000],

            [A6XXRegs.REG_A7XX_SP_UNKNOWN_0CE2,   0x00000000],
            [A6XXRegs.REG_A7XX_SP_UNKNOWN_0CE2+1, 0x00000000],
            [A6XXRegs.REG_A7XX_SP_UNKNOWN_0CE4,   0x00000000],
            [A6XXRegs.REG_A7XX_SP_UNKNOWN_0CE4+1, 0x00000000],
            [A6XXRegs.REG_A7XX_SP_UNKNOWN_0CE6,   0x00000000],
            [A6XXRegs.REG_A7XX_SP_UNKNOWN_0CE6+1, 0x00000000],

            [A6XXRegs.REG_A7XX_GRAS_UNKNOWN_80A7, 0x00000000],

            [A6XXRegs.REG_A7XX_RB_UNKNOWN_8899,   0x00000000],
            [A6XXRegs.REG_A7XX_RB_UNKNOWN_88F5,   0x00000000],
            [A6XXRegs.REG_A7XX_RB_UNKNOWN_8C34,   0x00000000],

            [A6XXRegs.REG_A7XX_GRAS_UNKNOWN_8008, 0x00000000],

            [0x930a, 0],
            [0x960a, 1],
            [A6XXRegs.REG_A7XX_SP_PS_OUTPUT_CONST_CNTL, 0],
            [A6XXRegs.REG_A7XX_SP_PS_OUTPUT_CONST_MASK, 0],
        ],
    ))

template = """\
/* Copyright © 2021 Google, Inc.
 *
 * SPDX-License-Identifier: MIT
 */

#include "freedreno_dev_info.h"
#include "util/u_debug.h"
#include "util/log.h"

#include <stdlib.h>

/* Map python to C: */
#define True true
#define False false

%for info in s.gpu_infos:
static const struct fd_dev_info __info${s.info_index(info)} = ${str(info)};
%endfor

static const struct fd_dev_rec fd_dev_recs[] = {
%for id, info in s.gpus.items():
   { {${id.gpu_id}, ${hex(id.chip_id)}}, "${id.name}", &__info${s.info_index(info)} },
%endfor
};

void
fd_dev_info_apply_dbg_options(struct fd_dev_info *info)
{
    const char *env = debug_get_option("FD_DEV_FEATURES", NULL);
    if (!env || !*env)
        return;

    char *features = strdup(env);
    char *feature, *feature_end;
    feature = strtok_r(features, ":", &feature_end);
    while (feature != NULL) {
        char *name, *name_end;
        name = strtok_r(feature, "=", &name_end);

        if (!name) {
            mesa_loge("Invalid feature \\"%s\\" in FD_DEV_FEATURES", feature);
            exit(1);
        }

        char *value = strtok_r(NULL, "=", &name_end);

        feature = strtok_r(NULL, ":", &feature_end);

%for (prop, gen), val in unique_props.items():
  <%
    if isinstance(val, bool):
        parse_value = "debug_parse_bool_option"
    else:
        parse_value = "debug_parse_num_option"
  %>
        if (strcmp(name, "${prop}") == 0) {
            info->${gen}.${prop} = ${parse_value}(value, info->${gen}.${prop});
            continue;
        }
%endfor

        mesa_loge("Invalid feature \\"%s\\" in FD_DEV_FEATURES", name);
        exit(1);
    }

    free(features);
}
"""

print(Template(template).render(s=s, unique_props=A6XXProps.unique_props))

