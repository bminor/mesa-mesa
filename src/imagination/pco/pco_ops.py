# Copyright © 2024 Imagination Technologies Ltd.
# SPDX-License-Identifier: MIT

from pco_pygen_common import *

MOD_TYPE = enum_type('mod_type', [
   'bool',
   'uint',
   'enum',
])

REF_TYPE = enum_type('ref_type', [
   'null',
   'ssa',
   'reg',
   'idx_reg',
   'imm',
   'io',
   'pred',
   'drc',
])

# Ref mods.
RM_ONEMINUS = ref_mod('oneminus', BaseType.bool)
RM_CLAMP = ref_mod('clamp', BaseType.bool)
RM_ABS = ref_mod('abs', BaseType.bool)
RM_NEG = ref_mod('neg', BaseType.bool)
RM_FLR = ref_mod('flr', BaseType.bool)

RM_ELEM = ref_mod_enum('elem', [
   'e0',
   'e1',
   'e2',
   'e3',
], is_bitset=True)

RM_DTYPE = ref_mod_enum('dtype', [
   ('any', ''),
   ('unsigned', 'u'),
   ('signed', 'i'),
   ('float', 'f'),
])

RM_BITS = ref_mod_enum('bits', [
   ('any', ''),
   ('1', '1'),
   ('8', '8'),
   ('16', '16'),
   ('32', '32'),
   ('64', '64'),
])

RM_REG_CLASS = ref_mod_enum('reg_class', [
   ('virt', '$'),
   ('temp', 'r'),
   ('vtxin', 'vi'),
   ('coeff', 'cf'),
   ('shared', 'sh'),
   ('index', 'idx'),
   ('spec', 'sr'),
   ('intern', 'i'),
   ('const', 'sc'),
   ('pixout', 'po'),
   ('global', 'g'),
   ('slot', 'sl'),
])

RM_IO = ref_mod_enum('io', [
   ('s0', 's0'),
   ('s1', 's1'),
   ('s2', 's2'),

   ('s3', 's3'),
   ('s4', 's4'),
   ('s5', 's5'),

   ('w0', 'w0'),
   ('w1', 'w1'),

   ('is0', 'is0'),
   ('is1', 'is1'),
   ('is2', 'is2'),
   ('is3', 'is3'),
   ('is4', 'is4'),
   ('is5', 'is5'),

   ('ft0', 'ft0'),
   ('ft0h', 'ft0h'),
   ('ft1', 'ft1'),
   ('ft2', 'ft2'),
   ('fte', 'fte'),

   ('ft1_invert', '~ft1'),
   ('ft3', 'ft3'),
   ('ft4', 'ft4'),
   ('ft5', 'ft5'),

   ('ftt', 'ftt'),

   ('cout', 'cout'),
])

RM_PRED = ref_mod_enum('pred', [
   ('pe', 'pe'),
   ('p0', 'p0'),

   ('always', 'if(1)'),
   ('p0_true', 'if(p0)'),
   ('never', 'if(0)'),
   ('p0_false', 'if(!p0)'),
])

RM_DRC = ref_mod_enum('drc', [
   ('pending', '?'),
   ('0', '0'),
   ('1', '1'),
])

# Op mods.
OM_EXEC_CND = op_mod_enum('exec_cnd', [
   ('e1_zx', ''),
   ('e1_z1', 'if(p0)'),
   ('ex_zx', '(ignorepe)'),
   ('e1_z0', 'if(!p0)'),
], print_early=True)
OM_RPT = op_mod('rpt', BaseType.uint, print_early=True, nzdefault=1)
OM_ATOM = op_mod('atom', BaseType.bool)
OM_END = op_mod('end', BaseType.bool)
OM_OLCHK = op_mod('olchk', BaseType.bool)
OM_SAT = op_mod('sat', BaseType.bool)
OM_LP = op_mod('lp', BaseType.bool)
OM_SCALE = op_mod('scale', BaseType.bool)
OM_ROUNDZERO = op_mod('roundzero', BaseType.bool)
OM_S = op_mod('s', BaseType.bool)
OM_TST_OP_MAIN = op_mod_enum('tst_op_main', [
   ('zero', 'z'),
   ('gzero', 'gz'),
   ('gezero', 'gez'),
   ('carry', 'c'),
   ('equal', 'e'),
   ('greater', 'g'),
   ('gequal', 'ge'),
   ('notequal', 'ne'),
   ('less', 'l'),
   ('lequal', 'le'),
])
OM_TST_OP_BITWISE = op_mod_enum('tst_op_bitwise', [
   ('zero', 'z'),
   ('nonzero', 'nz'),
])
OM_SIGNPOS = op_mod_enum('signpos', [
   'twb',
   'pwb',
   'mtb',
   'ftb',
])
OM_DIM = op_mod('dim', BaseType.uint)
OM_PROJ = op_mod('proj', BaseType.bool)
OM_FCNORM = op_mod('fcnorm', BaseType.bool)
OM_NNCOORDS = op_mod('nncoords', BaseType.bool)
OM_LOD_MODE = op_mod_enum('lod_mode', [
   ('normal', ''),
   ('bias', '.bias'),
   ('replace', '.replace'),
   ('gradient', '.gradient'),
])
OM_PPLOD = op_mod('pplod', BaseType.bool)
OM_TAO = op_mod('tao', BaseType.bool)
OM_SOO = op_mod('soo', BaseType.bool)
OM_SNO = op_mod('sno', BaseType.bool)
OM_WRT = op_mod('wrt', BaseType.bool)
OM_SB_MODE = op_mod_enum('sb_mode', [
   ('none', ''),
   ('data', '.data'),
   ('info', '.info'),
   ('both', '.both'),
])
OM_ARRAY = op_mod('array', BaseType.bool)
OM_INTEGER = op_mod('integer', BaseType.bool)
OM_SCHEDSWAP = op_mod('schedswap', BaseType.bool)
OM_F16 = op_mod('f16', BaseType.bool)
OM_TILED = op_mod('tiled', BaseType.bool)
OM_FREEP = op_mod('freep', BaseType.bool)
OM_SM = op_mod('sm', BaseType.bool)
OM_SAVMSK_MODE = op_mod_enum('savmsk_mode', [
   'vm',
   'icm',
   'icmoc',
   'icmi',
   'caxy',
])
OM_ATOM_OP = op_mod_enum('atom_op', [
   'add',
   'sub',
   'xchg',
   'umin',
   'imin',
   'umax',
   'imax',
   'and',
   'or',
   'xor',
])
OM_MCU_CACHE_MODE_LD = op_mod_enum('mcu_cache_mode_ld', [
   ('normal', ''),
   ('bypass', '.bypass'),
   ('force_line_fill', '.forcelinefill'),
])
OM_MCU_CACHE_MODE_ST = op_mod_enum('mcu_cache_mode_st', [
   ('write_through', '.writethrough'),
   ('write_back', '.writeback'),
   ('lazy_write_back', '.lazywriteback'),
])
OM_BRANCH_CND = op_mod_enum('branch_cnd', [
   ('exec_cond', ''),
   ('allinst', '.allinst'),
   ('anyinst', '.anyinst'),
])
OM_LINK = op_mod('link', BaseType.bool)
OM_PCK_FMT = op_mod_enum('pck_fmt', [
   'u8888',
   's8888',
   'o8888',
   'u1616',
   's1616',
   'o1616',
   'u32',
   's32',
   'u1010102',
   's1010102',
   'u111110',
   's111110',
   'f111110',
   'f16f16',
   'f32',
   'cov',
   'u565u565',
   'd24s8',
   's8d24',
   'f32_mask',
   '2f10f10f10',
   's8888ogl',
   's1616ogl',
   'zero',
   'one',
])
OM_PHASE2END = op_mod('phase2end', BaseType.bool)

# HW ops
O_NOP = hw_op('nop', [OM_EXEC_CND, OM_END])

O_FADD = hw_op('fadd', [OM_LP, OM_SAT], 1, 2, [], [[RM_ABS, RM_NEG, RM_FLR], [RM_ABS]])
