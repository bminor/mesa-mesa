# Copyright © 2024 Imagination Technologies Ltd.
# SPDX-License-Identifier: MIT

from pco_pygen_common import *

OP_PHASE = enum_type('op_phase', [
   ('ctrl', 0),
   ('0', 0),
   ('1', 1),
   ('2', 2),
   ('2_pck', 2),
   ('2_tst', 3),
   ('2_mov', 4),
   ('backend', 5),
])

# Common field types.

## Basic types.
F_BOOL = field_type('bool', BaseType.bool, 1)
F_UINT1 = field_type('uint1', BaseType.uint, 1)
F_UINT2 = field_type('uint2', BaseType.uint, 2)
F_UINT3 = field_type('uint3', BaseType.uint, 3)
F_UINT4 = field_type('uint4', BaseType.uint, 4)
F_UINT5 = field_type('uint5', BaseType.uint, 5)
F_UINT6 = field_type('uint6', BaseType.uint, 6)
F_UINT7 = field_type('uint7', BaseType.uint, 7)
F_UINT8 = field_type('uint8', BaseType.uint, 8)
F_UINT11 = field_type('uint11', BaseType.uint, 11)
F_UINT16 = field_type('uint16', BaseType.uint, 16)
F_UINT32 = field_type('uint32', BaseType.uint, 32)

## [1..4] -> [0..3]
F_UINT2_POS_INC = field_type('uint2_pos_inc', BaseType.uint, 2, dec_bits=3, check='{0} >= 1 && {0} <= 4', encode='{} - 1')
## [1..16] -> [1..15,0]
F_UINT4_POS_WRAP = field_type('uint4_pos_wrap', BaseType.uint, 4, dec_bits=5, check='{0} >= 1 && {0} <= 16', encode='{} % 16')
## 32-bit offset; lowest bit == 0b0.
F_OFFSET31 = field_type('offset31', BaseType.uint, 31, dec_bits=32, check='!({} & 0b1)', encode='{} >> 1')
## 8-bit value; lowest 2 bits == 0b00.
F_UINT6MUL4 = field_type('uint6mul4', BaseType.uint, 6, dec_bits=8, check='!({} & 0b11)', encode='{} >> 2')

# Instruction group header definitions.
F_OPORG = field_enum_type(
name='oporg', num_bits=3,
elems=[
   ('p0', 0b000),
   ('p2', 0b001),
   ('be', 0b010),
   ('p0_p1', 0b011),
   ('p0_p2', 0b100),
   ('p0_p1_p2', 0b101),
   ('p0_p2_be', 0b110),
   ('p0_p1_p2_be', 0b111),
])

F_OPCNT = field_enum_type(
name='opcnt', num_bits=3,
elems=[
   ('p0', 0b001),
   ('p1', 0b010),
   ('p2', 0b100),
], is_bitset=True)

F_CC = field_enum_type(
name='cc', num_bits=2,
elems=[
   ('e1_zx', 0b00),
   ('e1_z1', 0b01),
   ('ex_zx', 0b10),
   ('e1_z0', 0b11),
])

F_CC1 = field_enum_subtype(name='cc1', parent=F_CC, num_bits=1)

F_ALUTYPE = field_enum_type(
name='alutype', num_bits=2,
elems=[
   ('main', 0b00),
   ('bitwise', 0b10),
   ('control', 0b11),
])

F_CTRLOP = field_enum_type(
name='ctrlop', num_bits=4,
elems=[
   ('b', 0b0000),
   ('lapc', 0b0001),
   ('savl', 0b0010),
   ('cnd', 0b0011),
   ('wop', 0b0100),
   ('wdf', 0b0101),
   ('mutex', 0b0110),
   ('nop', 0b0111),
   ('itrsmp', 0b1000),
   ('sbo', 0b1011),
])

I_IGRP_HDR = bit_set(
name='igrp_hdr',
pieces=[
   ('da', (0, '7:4')),
   ('length', (0, '3:0')),
   ('ext', (1, '7')),
   ('oporg', (1, '6:4')),
   ('opcnt', (1, '6:4')),
   ('olchk', (1, '3')),
   ('w1p', (1, '2')),
   ('w0p', (1, '1')),
   ('cc', (1, '0')),

   ('end', (2, '7')),
   ('alutype', (2, '6:5')),
   ('rsvd', (2, '4')),
   ('atom', (2, '3')),
   ('rpt', (2, '2:1')),
   ('ccext', (2, '0')),

   ('miscctl', (2, '7')),
   ('ctrlop', (2, '4:1')),
],
fields=[
   ('da', (F_UINT4, ['da'])),
   ('length', (F_UINT4_POS_WRAP, ['length'])),
   ('ext', (F_BOOL, ['ext'])),
   ('oporg', (F_OPORG, ['oporg'])),
   ('opcnt', (F_OPCNT, ['opcnt'])),
   ('olchk', (F_BOOL, ['olchk'])),
   ('w1p', (F_BOOL, ['w1p'])),
   ('w0p', (F_BOOL, ['w0p'])),
   ('cc1', (F_CC1, ['cc'])),

   ('end', (F_BOOL, ['end'])),
   ('alutype', (F_ALUTYPE, ['alutype'])),
   ('rsvd', (F_UINT1, ['rsvd'], 0)),
   ('atom', (F_BOOL, ['atom'])),
   ('rpt', (F_UINT2_POS_INC, ['rpt'])),
   ('cc', (F_CC, ['ccext', 'cc'])),

   ('miscctl', (F_UINT1, ['miscctl'])),
   ('ctrlop', (F_CTRLOP, ['ctrlop'])),
])

I_IGRP_HDR_MAIN_BRIEF = bit_struct(
name='main_brief',
bit_set=I_IGRP_HDR,
field_mappings=[
   'da',
   'length',
   ('ext', 'ext', False),
   'oporg',
   'olchk',
   'w1p',
   'w0p',
   ('cc', 'cc1'),
])

I_IGRP_HDR_MAIN = bit_struct(
name='main',
bit_set=I_IGRP_HDR,
field_mappings=[
   'da',
   'length',
   ('ext', 'ext', True),
   'oporg',
   'olchk',
   'w1p',
   'w0p',
   'cc',
   'end',
   ('alutype', 'alutype', 'main'),
   'rsvd',
   'atom',
   'rpt',
])

I_IGRP_HDR_BITWISE = bit_struct(
name='bitwise',
bit_set=I_IGRP_HDR,
field_mappings=[
   'da',
   'length',
   ('ext', 'ext', True),
   'opcnt',
   'olchk',
   'w1p',
   'w0p',
   'cc',
   'end',
   ('alutype', 'alutype', 'bitwise'),
   'rsvd',
   'atom',
   'rpt',
])

I_IGRP_HDR_CONTROL = bit_struct(
name='control',
bit_set=I_IGRP_HDR,
field_mappings=[
   'da',
   'length',
   ('ext', 'ext', True),
   ('opcnt', 'opcnt', 0),
   'olchk',
   'w1p',
   'w0p',
   'cc',
   'miscctl',
   ('alutype', 'alutype', 'control'),
   'ctrlop',
])

# Upper/lower source definitions.
F_IS0_SEL = field_enum_type(
name='is0_sel', num_bits=3,
elems=[
   ('s0', 0b000),
   ('s3', 0b001),
   ('s4', 0b010),
   ('s5', 0b011),
   ('s1', 0b100),
   ('s2', 0b101),
])

F_IS0_SEL2 = field_enum_subtype(name='is0_sel2', parent=F_IS0_SEL, num_bits=2)

F_REGBANK = field_enum_type(
name='regbank', num_bits=3,
elems=[
   ('special', 0b000),
   ('temp', 0b001),
   ('vtxin', 0b010),
   ('coeff', 0b011),
   ('shared', 0b100),
   ('coeff_alt', 0b101),
   ('idx0', 0b110),
   ('idx1', 0b111),
])

F_IDXBANK = field_enum_type(
name='idxbank', num_bits=3,
elems=[
   ('temp', 0b000),
   ('vtxin', 0b001),
   ('coeff', 0b010),
   ('shared', 0b011),
   ('idx', 0b101),
   ('coeff_alt', 0b110),
   ('pixout', 0b111),
])

F_REGBANK2 = field_enum_subtype(name='regbank2', parent=F_REGBANK, num_bits=2)
F_REGBANK1 = field_enum_subtype(name='regbank1', parent=F_REGBANK, num_bits=1)

I_SRC = bit_set(
name='src',
pieces=[
   ('ext0', (0, '7')),
   ('sbA_0_b0', (0, '6')),
   ('sA_5_0_b0', (0, '5:0')),

   ('sel', (1, '7')),
   ('ext1', (1, '6')),

   ('mux_1_0_b1', (1, '5:4')),
   ('sbA_2_1_b1', (1, '3:2')),
   ('sA_7_6_b1', (1, '1:0')),

   ('sbB_0_b1', (1, '5')),
   ('sB_4_0_b1', (1, '4:0')),

   ('rsvd2', (2, '7:3')),
   ('sA_10_8_b2', (2, '2:0')),

   ('ext2', (2, '7')),
   ('mux_1_0_b2', (2, '6:5')),
   ('sbA_1_b2', (2, '4')),
   ('sbB_1_b2', (2, '3')),
   ('sA_6_b2', (2, '2')),
   ('sB_6_5_b2', (2, '1:0')),

   ('sA_10_8_b3', (3, '7:5')),
   ('mux_2_b3', (3, '4')),
   ('sbA_2_b3', (3, '3')),
   ('rsvd3', (3, '2')),
   ('sA_7_b3', (3, '1')),
   ('sB_7_b3', (3, '0')),

   ('sbC_1_0_b3', (3, '7:6')),
   ('sC_5_0_b3', (3, '5:0')),

   ('sbC_2_b4', (4, '7')),
   ('sC_7_6_b4', (4, '6:5')),
   ('mux_2_b4', (4, '4')),
   ('sbA_2_b4', (4, '3')),
   ('ext4', (4, '2')),
   ('sA_7_b4', (4, '1')),
   ('sB_7_b4', (4, '0')),

   ('rsvd5', (5, '7:6')),
   ('sC_10_8_b5', (5, '5:3')),
   ('sA_10_8_b5', (5, '2:0')),
],
fields=[
   ('ext0', (F_BOOL, ['ext0'])),
   ('sbA_1bit_b0', (F_REGBANK1, ['sbA_0_b0'])),
   ('sA_6bit_b0', (F_UINT6, ['sA_5_0_b0'])),

   ('sel', (F_BOOL, ['sel'])),
   ('ext1', (F_BOOL, ['ext1'])),
   ('mux_2bit_b1', (F_IS0_SEL2, ['mux_1_0_b1'])),
   ('sbA_3bit_b1', (F_REGBANK, ['sbA_2_1_b1', 'sbA_0_b0'])),
   ('sA_11bit_b2', (F_UINT11, ['sA_10_8_b2', 'sA_7_6_b1', 'sA_5_0_b0'])),
   ('rsvd2', (F_UINT5, ['rsvd2'], 0)),

   ('sbB_1bit_b1', (F_REGBANK1, ['sbB_0_b1'])),
   ('sB_5bit_b1', (F_UINT5, ['sB_4_0_b1'])),

   ('ext2', (F_BOOL, ['ext2'])),
   ('mux_2bit_b2', (F_IS0_SEL2, ['mux_1_0_b2'])),
   ('sbA_2bit_b2', (F_REGBANK2, ['sbA_1_b2', 'sbA_0_b0'])),
   ('sbB_2bit_b2', (F_REGBANK2, ['sbB_1_b2', 'sbB_0_b1'])),
   ('sA_7bit_b2', (F_UINT7, ['sA_6_b2', 'sA_5_0_b0'])),
   ('sB_7bit_b2', (F_UINT7, ['sB_6_5_b2', 'sB_4_0_b1'])),

   ('sA_11bit_b3', (F_UINT11, ['sA_10_8_b3', 'sA_7_b3', 'sA_6_b2', 'sA_5_0_b0'])),
   ('mux_3bit_b3', (F_IS0_SEL, ['mux_2_b3', 'mux_1_0_b2'])),
   ('sbA_3bit_b3', (F_REGBANK, ['sbA_2_b3', 'sbA_1_b2', 'sbA_0_b0'])),
   ('rsvd3', (F_UINT1, ['rsvd3'], 0)),
   ('sB_8bit_b3', (F_UINT8, ['sB_7_b3', 'sB_6_5_b2', 'sB_4_0_b1'])),

   ('sbC_2bit_b3', (F_REGBANK2, ['sbC_1_0_b3'])),
   ('sC_6bit_b3', (F_UINT6, ['sC_5_0_b3'])),

   ('rsvd4', (F_UINT1, ['sbC_2_b4'], 0)),
   ('sbC_3bit_b4', (F_REGBANK, ['sbC_2_b4', 'sbC_1_0_b3'])),
   ('sC_8bit_b4', (F_UINT8, ['sC_7_6_b4', 'sC_5_0_b3'])),
   ('mux_3bit_b4', (F_IS0_SEL, ['mux_2_b4', 'mux_1_0_b2'])),
   ('sbA_3bit_b4', (F_REGBANK, ['sbA_2_b4', 'sbA_1_b2', 'sbA_0_b0'])),
   ('ext4', (F_BOOL, ['ext4'])),
   ('sA_8bit_b4', (F_UINT8, ['sA_7_b4', 'sA_6_b2', 'sA_5_0_b0'])),
   ('sB_8bit_b4', (F_UINT8, ['sB_7_b4', 'sB_6_5_b2', 'sB_4_0_b1'])),

   ('rsvd5', (F_UINT2, ['rsvd5'], 0)),
   ('rsvd5_', (F_UINT3, ['sC_10_8_b5'], 0)),
   ('sC_11bit_b5', (F_UINT11, ['sC_10_8_b5', 'sC_7_6_b4', 'sC_5_0_b3'])),
   ('sA_11bit_b5', (F_UINT11, ['sA_10_8_b5', 'sA_7_b4', 'sA_6_b2', 'sA_5_0_b0'])),
])

# Lower sources.
I_ONE_LO_1B6I = bit_struct(
name='1lo_1b6i',
bit_set=I_SRC,
field_mappings=[
   ('ext0', 'ext0', 0),

   ('sb0', 'sbA_1bit_b0'),
   ('s0', 'sA_6bit_b0'),
], data=(1, 6, 0, 0, 0, 0, 0))

I_ONE_LO_3B11I_2M = bit_struct(
name='1lo_3b11i_2m',
bit_set=I_SRC,
field_mappings=[
   ('ext0', 'ext0', 1),
   ('sel', 'sel', 0),
   ('ext1', 'ext1', 0),
   ('rsvd2', 'rsvd2'),

   ('sb0', 'sbA_3bit_b1'),
   ('s0', 'sA_11bit_b2'),
   ('is0', 'mux_2bit_b1'),
], data=(3, 11, 0, 0, 0, 0, 2))

I_TWO_LO_1B6I_1B5I = bit_struct(
name='2lo_1b6i_1b5i',
bit_set=I_SRC,
field_mappings=[
   ('ext0', 'ext0', 1),
   ('sel', 'sel', 1),
   ('ext1', 'ext1', 0),

   ('sb0', 'sbA_1bit_b0'),
   ('s0', 'sA_6bit_b0'),
   ('sb1', 'sbB_1bit_b1'),
   ('s1', 'sB_5bit_b1'),
], data=(1, 6, 1, 5, 0, 0, 0))

I_TWO_LO_2B7I_2B7I_2M = bit_struct(
name='2lo_2b7i_2b7i_2m',
bit_set=I_SRC,
field_mappings=[
   ('ext0', 'ext0', 1),
   ('sel', 'sel', 1),
   ('ext1', 'ext1', 1),
   ('ext2', 'ext2', 0),

   ('sb0', 'sbA_2bit_b2'),
   ('s0', 'sA_7bit_b2'),
   ('sb1', 'sbB_2bit_b2'),
   ('s1', 'sB_7bit_b2'),
   ('is0', 'mux_2bit_b2'),
], data=(2, 7, 2, 7, 0, 0, 2))

I_TWO_LO_3B11I_2B8I_3M = bit_struct(
name='2lo_3b11i_2b8i_3m',
bit_set=I_SRC,
field_mappings=[
   ('ext0', 'ext0', 1),
   ('sel', 'sel', 1),
   ('ext1', 'ext1', 1),
   ('ext2', 'ext2', 1),
   ('rsvd3', 'rsvd3'),

   ('sb0', 'sbA_3bit_b3'),
   ('s0', 'sA_11bit_b3'),
   ('sb1', 'sbB_2bit_b2'),
   ('s1', 'sB_8bit_b3'),
   ('is0', 'mux_3bit_b3'),
], data=(3, 11, 2, 8, 0, 0, 3))

I_THREE_LO_2B7I_2B7I_2B6I_2M = bit_struct(
name='3lo_2b7i_2b7i_2b6i_2m',
bit_set=I_SRC,
field_mappings=[
   ('ext0', 'ext0', 1),
   ('sel', 'sel', 0),
   ('ext1', 'ext1', 1),
   ('ext2', 'ext2', 0),

   ('sb0', 'sbA_2bit_b2'),
   ('s0', 'sA_7bit_b2'),
   ('sb1', 'sbB_2bit_b2'),
   ('s1', 'sB_7bit_b2'),
   ('sb2', 'sbC_2bit_b3'),
   ('s2', 'sC_6bit_b3'),
   ('is0', 'mux_2bit_b2'),
], data=(2, 7, 2, 7, 2, 6, 2))

I_THREE_LO_3B8I_2B8I_3B8I_3M = bit_struct(
name='3lo_3b8i_2b8i_3b8i_3m',
bit_set=I_SRC,
field_mappings=[
   ('ext0', 'ext0', 1),
   ('sel', 'sel', 0),
   ('ext1', 'ext1', 1),
   ('ext2', 'ext2', 1),
   ('ext4', 'ext4', 0),

   ('sb0', 'sbA_3bit_b4'),
   ('s0', 'sA_8bit_b4'),
   ('sb1', 'sbB_2bit_b2'),
   ('s1', 'sB_8bit_b4'),
   ('sb2', 'sbC_3bit_b4'),
   ('s2', 'sC_8bit_b4'),
   ('is0', 'mux_3bit_b4'),
], data=(3, 8, 2, 8, 3, 8, 3))

I_THREE_LO_3B11I_2B8I_3B11I_3M = bit_struct(
name='3lo_3b11i_2b8i_3b11i_3m',
bit_set=I_SRC,
field_mappings=[
   ('ext0', 'ext0', 1),
   ('sel', 'sel', 0),
   ('ext1', 'ext1', 1),
   ('ext2', 'ext2', 1),
   ('ext4', 'ext4', 1),
   ('rsvd5', 'rsvd5'),

   ('sb0', 'sbA_3bit_b4'),
   ('s0', 'sA_11bit_b5'),
   ('sb1', 'sbB_2bit_b2'),
   ('s1', 'sB_8bit_b4'),
   ('sb2', 'sbC_3bit_b4'),
   ('s2', 'sC_11bit_b5'),
   ('is0', 'mux_3bit_b4'),
], data=(3, 11, 2, 8, 3, 11, 3))

# Upper sources.
I_ONE_UP_1B6I = bit_struct(
name='1up_1b6i',
bit_set=I_SRC,
field_mappings=[
   ('ext0', 'ext0', 0),

   ('sb3', 'sbA_1bit_b0'),
   ('s3', 'sA_6bit_b0'),
], data=(1, 6, 0, 0, 0, 0))

I_ONE_UP_3B11I = bit_struct(
name='1up_3b11i',
bit_set=I_SRC,
field_mappings=[
   ('ext0', 'ext0', 1),
   ('sel', 'sel', 0),
   ('ext1', 'ext1', 0),
   ('rsvd1', 'mux_2bit_b1', 0),
   ('rsvd2', 'rsvd2'),

   ('sb3', 'sbA_3bit_b1'),
   ('s3', 'sA_11bit_b2'),
], data=(3, 11, 0, 0, 0, 0))

I_TWO_UP_1B6I_1B5I = bit_struct(
name='2up_1b6i_1b5i',
bit_set=I_SRC,
field_mappings=[
   ('ext0', 'ext0', 1),
   ('sel', 'sel', 1),
   ('ext1', 'ext1', 0),

   ('sb3', 'sbA_1bit_b0'),
   ('s3', 'sA_6bit_b0'),
   ('sb4', 'sbB_1bit_b1'),
   ('s4', 'sB_5bit_b1'),
], data=(1, 6, 1, 5, 0, 0))

I_TWO_UP_2B7I_2B7I = bit_struct(
name='2up_2b7i_2b7i',
bit_set=I_SRC,
field_mappings=[
   ('ext0', 'ext0', 1),
   ('sel', 'sel', 1),
   ('ext1', 'ext1', 1),
   ('ext2', 'ext2', 0),
   ('rsvd2', 'mux_2bit_b2', 0),

   ('sb3', 'sbA_2bit_b2'),
   ('s3', 'sA_7bit_b2'),
   ('sb4', 'sbB_2bit_b2'),
   ('s4', 'sB_7bit_b2'),
], data=(2, 7, 2, 7, 0, 0))

I_TWO_UP_3B11I_2B8I = bit_struct(
name='2up_3b11i_2b8i',
bit_set=I_SRC,
field_mappings=[
   ('ext0', 'ext0', 1),
   ('sel', 'sel', 1),
   ('ext1', 'ext1', 1),
   ('ext2', 'ext2', 1),
   ('rsvd3', 'rsvd3'),
   ('rsvd3_', 'mux_3bit_b3', 0),

   ('sb3', 'sbA_3bit_b3'),
   ('s3', 'sA_11bit_b3'),
   ('sb4', 'sbB_2bit_b2'),
   ('s4', 'sB_8bit_b3'),
], data=(3, 11, 2, 8, 0, 0))

I_THREE_UP_2B7I_2B7I_2B6I = bit_struct(
name='3up_2b7i_2b7i_2b6i',
bit_set=I_SRC,
field_mappings=[
   ('ext0', 'ext0', 1),
   ('sel', 'sel', 0),
   ('ext1', 'ext1', 1),
   ('ext2', 'ext2', 0),
   ('rsvd2', 'mux_2bit_b2', 0),

   ('sb3', 'sbA_2bit_b2'),
   ('s3', 'sA_7bit_b2'),
   ('sb4', 'sbB_2bit_b2'),
   ('s4', 'sB_7bit_b2'),
   ('sb5', 'sbC_2bit_b3'),
   ('s5', 'sC_6bit_b3'),
], data=(2, 7, 2, 7, 2, 6))

I_THREE_UP_3B8I_2B8I_2B8I = bit_struct(
name='3up_3b8i_2b8i_2b8i',
bit_set=I_SRC,
field_mappings=[
   ('ext0', 'ext0', 1),
   ('sel', 'sel', 0),
   ('ext1', 'ext1', 1),
   ('ext2', 'ext2', 1),
   ('ext4', 'ext4', 0),
   ('rsvd4', 'mux_3bit_b4', 0),
   ('rsvd4_', 'rsvd4'),

   ('sb3', 'sbA_3bit_b4'),
   ('s3', 'sA_8bit_b4'),
   ('sb4', 'sbB_2bit_b2'),
   ('s4', 'sB_8bit_b4'),
   ('sb5', 'sbC_2bit_b3'),
   ('s5', 'sC_8bit_b4'),
], data=(3, 8, 2, 8, 2, 8))

I_THREE_UP_3B11I_2B8I_2B8I = bit_struct(
name='3up_3b11i_2b8i_2b8i',
bit_set=I_SRC,
field_mappings=[
   ('ext0', 'ext0', 1),
   ('sel', 'sel', 0),
   ('ext1', 'ext1', 1),
   ('ext2', 'ext2', 1),
   ('ext4', 'ext4', 1),
   ('rsvd4', 'mux_3bit_b4', 0),
   ('rsvd4_', 'rsvd4'),
   ('rsvd5', 'rsvd5'),
   ('rsvd5_', 'rsvd5_'),

   ('sb3', 'sbA_3bit_b4'),
   ('s3', 'sA_11bit_b5'),
   ('sb4', 'sbB_2bit_b2'),
   ('s4', 'sB_8bit_b4'),
   ('sb5', 'sbC_2bit_b3'),
   ('s5', 'sC_8bit_b4'),
], data=(3, 11, 2, 8, 2, 8))

# Internal source selector definitions.
F_IS5_SEL = field_enum_type(
name='is5_sel', num_bits=2,
elems=[
   ('ft0', 0b00),
   ('ft1', 0b01),
   ('ft2', 0b10),
   ('fte', 0b11),
])

F_IS4_SEL = field_enum_type(
name='is4_sel', num_bits=2,
elems=[
   ('ft0', 0b00),
   ('ft1', 0b01),
   ('ft2', 0b10),
   ('fte', 0b11),
])

F_IS3_SEL = field_enum_type(
name='is3_sel', num_bits=2,
elems=[
   ('ft0', 0b00),
   ('ft1', 0b01),
   ('fte', 0b11),
])

F_IS2_SEL = field_enum_type(
name='is2_sel', num_bits=1,
elems=[
   ('ft1', 0b0),
   ('fte', 0b1),
])

F_IS1_SEL = field_enum_type(
name='is1_sel', num_bits=1,
elems=[
   ('ft0', 0b0),
   ('fte', 0b1),
])

I__ISS = bit_set(
name='iss',
pieces=[
   ('is5', (0, '7:6')),
   ('is4', (0, '5:4')),
   ('is3', (0, '3:2')),
   ('is2', (0, '1')),
   ('is1', (0, '0')),
],
fields=[
   ('is5', (F_IS5_SEL, ['is5'])),
   ('is4', (F_IS4_SEL, ['is4'])),
   ('is3', (F_IS3_SEL, ['is3'])),
   ('is2', (F_IS2_SEL, ['is2'])),
   ('is1', (F_IS1_SEL, ['is1'])),
])

I_ISS = bit_struct(
name='iss',
bit_set=I__ISS,
field_mappings=[
   ('is5', 'is5'),
   ('is4', 'is4'),
   ('is3', 'is3'),
   ('is2', 'is2'),
   ('is1', 'is1'),
])

# Destination definitions.
I_DST = bit_set(
name='dst',
pieces=[
   ('ext0', (0, '7')),
   ('dbN_0_b0', (0, '6')),
   ('dN_5_0_b0', (0, '5:0')),

   ('rsvd1', (1, '7')),
   ('dN_10_8_b1', (1, '6:4')),
   ('dbN_2_1_b1', (1, '3:2')),
   ('dN_7_6_b1', (1, '1:0')),

   ('db0_0_b0', (0, '7')),
   ('d0_6_0_b0', (0, '6:0')),

   ('ext1', (1, '7')),
   ('db1_0_b1', (1, '6')),
   ('d1_5_0_b1', (1, '5:0')),

   ('ext2', (2, '7')),
   ('db1_2_1_b2', (2, '6:5')),
   ('d1_7_6_b2', (2, '4:3')),
   ('db0_2_1_b2', (2, '2:1')),
   ('d0_7_b2', (2, '0')),

   ('rsvd3', (3, '7:6')),
   ('d1_10_8_b3', (3, '5:3')),
   ('d0_10_8_b3', (3, '2:0')),
],
fields=[
   ('ext0', (F_BOOL, ['ext0'])),
   ('dbN_1bit_b0', (F_REGBANK1, ['dbN_0_b0'])),
   ('dN_6bit_b0', (F_UINT6, ['dN_5_0_b0'])),

   ('rsvd1', (F_UINT1, ['rsvd1'], 0)),
   ('dbN_3bit_b1', (F_REGBANK, ['dbN_2_1_b1', 'dbN_0_b0'])),
   ('dN_11bit_b1', (F_UINT11, ['dN_10_8_b1', 'dN_7_6_b1', 'dN_5_0_b0'])),

   ('db0_1bit_b0', (F_REGBANK1, ['db0_0_b0'])),
   ('d0_7bit_b0', (F_UINT7, ['d0_6_0_b0'])),

   ('ext1', (F_BOOL, ['ext1'])),
   ('db1_1bit_b1', (F_REGBANK1, ['db1_0_b1'])),
   ('d1_6bit_b1', (F_UINT6, ['d1_5_0_b1'])),

   ('ext2', (F_BOOL, ['ext2'])),
   ('db0_3bit_b2', (F_REGBANK, ['db0_2_1_b2', 'db0_0_b0'])),
   ('d0_8bit_b2', (F_UINT8, ['d0_7_b2', 'd0_6_0_b0'])),
   ('db1_3bit_b2', (F_REGBANK, ['db1_2_1_b2', 'db1_0_b1'])),
   ('d1_8bit_b2', (F_UINT8, ['d1_7_6_b2', 'd1_5_0_b1'])),

   ('rsvd3', (F_UINT2, ['rsvd3'], 0)),
   ('d0_11bit_b3', (F_UINT11, ['d0_10_8_b3', 'd0_7_b2', 'd0_6_0_b0'])),
   ('d1_11bit_b3', (F_UINT11, ['d1_10_8_b3', 'd1_7_6_b2', 'd1_5_0_b1'])),
])

I_ONE_1B6I = bit_struct(
name='1_1b6i',
bit_set=I_DST,
field_mappings=[
   ('ext0', 'ext0', 0),

   ('dbN', 'dbN_1bit_b0'),
   ('dN', 'dN_6bit_b0'),
], data=(1, 6))

I_ONE_3B11I = bit_struct(
name='1_3b11i',
bit_set=I_DST,
field_mappings=[
   ('ext0', 'ext0', 1),
   ('rsvd1', 'rsvd1'),

   ('dbN', 'dbN_3bit_b1'),
   ('dN', 'dN_11bit_b1'),
], data=(3, 11))

I_TWO_1B7I_1B6I = bit_struct(
name='2_1b7i_1b6i',
bit_set=I_DST,
field_mappings=[
   ('ext1', 'ext1', 0),

   ('db0', 'db0_1bit_b0'),
   ('d0', 'd0_7bit_b0'),
   ('db1', 'db1_1bit_b1'),
   ('d1', 'd1_6bit_b1'),
], data=(1, 7, 1, 6))

I_TWO_3B8I_3B8I = bit_struct(
name='2_3b8i_3b8i',
bit_set=I_DST,
field_mappings=[
   ('ext1', 'ext1', 1),
   ('ext2', 'ext2', 0),

   ('db0', 'db0_3bit_b2'),
   ('d0', 'd0_8bit_b2'),
   ('db1', 'db1_3bit_b2'),
   ('d1', 'd1_8bit_b2'),
], data=(3, 8, 3, 8))

I_TWO_3B11I_3B11I = bit_struct(
name='2_3b11i_3b11i',
bit_set=I_DST,
field_mappings=[
   ('ext1', 'ext1', 1),
   ('ext2', 'ext2', 1),
   ('rsvd3', 'rsvd3'),

   ('db0', 'db0_3bit_b2'),
   ('d0', 'd0_11bit_b3'),
   ('db1', 'db1_3bit_b2'),
   ('d1', 'd1_11bit_b3'),
], data=(3, 11, 3, 11))

# Main ALU ops.
F_MAIN_OP = field_enum_type(
name='main_op', num_bits=3,
elems=[
   ('fadd', 0b000),
   ('fadd_lp', 0b001),
   ('fmul', 0b010),
   ('fmul_lp', 0b011),
   ('sngl', 0b100),
   ('int8_16', 0b101),
   ('fmad_movc', 0b110),
   ('int32_64_tst', 0b111),
])

F_SNGL_OP = field_enum_type(
name='sngl_op', num_bits=4,
elems=[
   ('rcp', 0b0000),
   ('rsq', 0b0001),
   ('log', 0b0010),
   ('exp', 0b0011),
   ('f16sop', 0b0100),
   ('logcn', 0b0101),
   ('gamma', 0b0110),
   ('byp', 0b0111),
   ('dsx', 0b1000),
   ('dsy', 0b1001),
   ('dsxf', 0b1010),
   ('dsyf', 0b1011),
   ('pck', 0b1100),
   ('red', 0b1101),
   ('sinc', 0b1110),
   ('arctanc', 0b1111),
])

F_RED_PART = field_enum_type(
name='red_part', num_bits=1,
elems=[
   ('a', 0b0),
   ('b', 0b1),
])

F_RED_TYPE = field_enum_type(
name='red_type', num_bits=1,
elems=[
   ('sin', 0b0),
   ('cos', 0b1),
])

F_GAMMA_OP = field_enum_type(
name='gamma_op', num_bits=1,
elems=[
   ('cmp', 0b0),
   ('exp', 0b1),
])

F_PCK_FORMAT = field_enum_type(
name='pck_format', num_bits=5,
elems=[
   ('u8888', 0b00000),
   ('s8888', 0b00001),
   ('o8888', 0b00010),
   ('u1616', 0b00011),
   ('s1616', 0b00100),
   ('o1616', 0b00101),
   ('u32', 0b00110),
   ('s32', 0b00111),
   ('u1010102', 0b01000),
   ('s1010102', 0b01001),
   ('u111110', 0b01010),
   ('s111110', 0b01011),
   ('f111110', 0b01100),
   ('f16f16', 0b01110),
   ('f32', 0b01111),
   ('cov', 0b10000),
   ('u565u565', 0b10001),
   ('d24s8', 0b10010),
   ('s8d24', 0b10011),
   ('f32_mask', 0b10100),
   ('2f10f10f10', 0b10101),
   ('s8888ogl', 0b10110),
   ('s1616ogl', 0b10111),
   ('zero', 0b11110),
   ('one', 0b11111),
])

F_INT8_16_OP = field_enum_type(
name='int8_16_op', num_bits=2,
elems=[
   ('add', 0b00),
   ('mul', 0b01),
   ('mad_0_1', 0b10),
   ('mad_2_3', 0b11),
])

F_INT8_16_FMT = field_enum_type(
name='int8_16_fmt', num_bits=1,
elems=[
   ('8bit', 0b0),
   ('16bit', 0b1),
])

F_S2CH = field_enum_type(
name='s2ch', num_bits=1,
elems=[
   ('elo', 0b0),
   ('ehi', 0b1),
])

F_S01CH = field_enum_type(
name='s01ch', num_bits=2,
elems=[
   ('e0', 0b00),
   ('e1', 0b01),
   ('e2', 0b10),
   ('e3', 0b11),
])

F_MOVW01 = field_enum_type(
name='movw01', num_bits=2,
elems=[
   ('ft0', 0b00),
   ('ft1', 0b01),
   ('ft2', 0b10),
   ('fte', 0b11),
])

F_MASKW0 = field_enum_type(
name='maskw0', num_bits=4, is_bitset=True,
elems=[
   ('e0', 0b0001),
   ('e1', 0b0010),
   ('e2', 0b0100),
   ('e3', 0b1000),
   ('eall', 0b1111),
])

F_INT32_64_OP = field_enum_type(
name='int32_64_op', num_bits=2,
elems=[
   ('add6432', 0b00),
   ('add64', 0b01),
   ('madd32', 0b10),
   ('madd64', 0b11),
])

F_TST_OP = field_enum_type(
name='tst_op', num_bits=4,
elems=[
   ('z', 0b0000),
   ('gz', 0b0001),
   ('gez', 0b0010),
   ('c', 0b0011),
   ('e', 0b0100),
   ('g', 0b0101),
   ('ge', 0b0110),
   ('ne', 0b0111),
   ('l', 0b1000),
   ('le', 0b1001),
])

F_TST_OP3 = field_enum_subtype(name='tst_op3', parent=F_TST_OP, num_bits=3)

F_TST_TYPE = field_enum_type(
name='tst_type', num_bits=3,
elems=[
   ('f32', 0b000),
   ('u16', 0b001),
   ('s16', 0b010),
   ('u8', 0b011),
   ('s8', 0b100),
   ('u32', 0b101),
   ('s32', 0b110),
])

I_MAIN = bit_set(
name='main',
pieces=[
   ('main_op', (0, '7:5')),
   ('ext0', (0, '4')),

   # fadd/fadd.lp/fmul/fmul.lp
   ('sat_fam', (0, '4')),
   ('s0neg_fam', (0, '3')),
   ('s0abs_fam', (0, '2')),
   ('s1abs_fam', (0, '1')),
   ('s0flr_fam', (0, '0')),

   # sngl
   ('sngl_op', (0, '3:0')),

   ## RED
   ('red_part', (1, '7')),
   ('iter', (1, '6:4')),
   ('red_type', (1, '3')),
   ('pwen_red', (1, '2')),

   ## Gamma
   ('gammaop', (1, '2')),

   ## Common
   ('s0neg_sngl', (1, '1')),
   ('s0abs_sngl', (1, '0')),

   ## PCK/UPCK
   ('upck_elem', (1, '7:6')),
   ('scale_rtz', (1, '5')),

   ('prog', (1, '7')),
   ('rtz', (1, '6')),
   ('scale', (1, '5')),

   ('pck_format', (1, '4:0')),

   # int8_16
   ('int8_16_op', (0, '3:2')),
   ('s_i816', (0, '1')),
   ('f_i816', (0, '0')),

   ('s2ch', (1, '7')),
   ('rsvd1_i816', (1, '6')),
   ('s2neg_i816', (1, '5')),
   ('s2abs_i816', (1, '4')),
   ('s1abs_i816', (1, '3')),
   ('s0neg_i816', (1, '2')),
   ('s0abs_i816', (1, '1')),
   ('sat_i816', (1, '0')),

   ('rsvd2_i816', (2, '7:4')),
   ('s1ch', (2, '3:2')),
   ('s0ch', (2, '1:0')),

   # fmad
   ('s0neg_fma', (0, '3')),
   ('s0abs_fma', (0, '2')),
   ('s2neg_fma', (0, '1')),
   ('sat_fma', (0, '0')),

   ('rsvd1_fma', (1, '7:5')),
   ('lp_fma', (1, '4')),
   ('s1abs_fma', (1, '3')),
   ('s1neg_fma', (1, '2')),
   ('s2flr_fma', (1, '1')),
   ('s2abs_fma', (1, '0')),

   # int32_64
   ('s_i3264', (0, '3')),
   ('s2neg_i3264', (0, '2')),
   ('int32_64_op', (0, '1:0')),

   ('rsvd1_i3264', (1, '7')),
   ('cin_i3264', (1, '6')),
   ('s1neg_i3264', (1, '5')),
   ('s0neg_i3264', (1, '4')),
   ('rsvd1_i3264_', (1, '3')),
   ('s0abs_i3264', (1, '2')),
   ('s1abs_i3264', (1, '1')),
   ('s2abs_i3264', (1, '0')),

   # movc
   ('movw1', (0, '3:2')),
   ('movw0', (0, '1:0')),

   ('rsvd1_movc', (1, '7:6')),
   ('maskw0', (1, '5:2')),
   ('aw', (1, '1')),
   ('p2end_movc', (1, '0')),

   # tst
   ('tst_op_2_0', (0, '3:1')),
   ('pwen_tst', (0, '0')),

   ('tst_type', (1, '7:5')),
   ('p2end_tst', (1, '4')),
   ('tst_elem', (1, '3:2')),
   ('rsvd1_tst', (1, '1')),
   ('tst_op_3', (1, '0')),
],
fields=[
   ('main_op', (F_MAIN_OP, ['main_op'])),
   ('ext0', (F_BOOL, ['ext0'])),

   # fadd/fadd.lp/fmul/fmul.lp
   ('sat_fam', (F_BOOL, ['sat_fam'])),
   ('s0neg_fam', (F_BOOL, ['s0neg_fam'])),
   ('s0abs_fam', (F_BOOL, ['s0abs_fam'])),
   ('s1abs_fam', (F_BOOL, ['s1abs_fam'])),
   ('s0flr_fam', (F_BOOL, ['s0flr_fam'])),

   # sngl
   ('sngl_op', (F_SNGL_OP, ['sngl_op'])),

   ('rsvd1_sngl', (F_UINT5, ['red_part', 'iter', 'red_type'], 0)),
   ('rsvd1_sngl_', (F_UINT1, ['pwen_red'], 0)),

   ## RED
   ('red_part', (F_RED_PART, ['red_part'])),
   ('iter', (F_UINT3, ['iter'])),
   ('red_type', (F_RED_TYPE, ['red_type'])),
   ('pwen_red', (F_BOOL, ['pwen_red'])),

   ## Gamma
   ('gammaop', (F_GAMMA_OP, ['gammaop'])),

   ## Common
   ('s0neg_sngl', (F_BOOL, ['s0neg_sngl'])),
   ('s0abs_sngl', (F_BOOL, ['s0abs_sngl'])),

   ## PCK/UPCK
   ('upck_elem', (F_UINT2, ['upck_elem'])),
   ('scale_rtz', (F_BOOL, ['scale_rtz'])),

   ('prog', (F_BOOL, ['prog'])),
   ('rtz', (F_BOOL, ['rtz'])),
   ('scale', (F_BOOL, ['scale'])),

   ('pck_format', (F_PCK_FORMAT, ['pck_format'])),

   # int8_16
   ('int8_16_op', (F_INT8_16_OP, ['int8_16_op'])),
   ('s_i816', (F_BOOL, ['s_i816'])),
   ('f_i816', (F_INT8_16_FMT, ['f_i816'])),

   ('s2ch', (F_S2CH, ['s2ch'])),
   ('rsvd1_i816', (F_UINT1, ['rsvd1_i816'], 0)),
   ('s2neg_i816', (F_BOOL, ['s2neg_i816'])),
   ('s2abs_i816', (F_BOOL, ['s2abs_i816'])),
   ('s1abs_i816', (F_BOOL, ['s1abs_i816'])),
   ('s0neg_i816', (F_BOOL, ['s0neg_i816'])),
   ('s0abs_i816', (F_BOOL, ['s0abs_i816'])),
   ('sat_i816', (F_BOOL, ['sat_i816'])),

   ('rsvd2_i816', (F_UINT4, ['rsvd2_i816'], 0)),
   ('s1ch', (F_S01CH, ['s1ch'])),
   ('s0ch', (F_S01CH, ['s0ch'])),

   # fmad
   ('s0neg_fma', (F_BOOL, ['s0neg_fma'])),
   ('s0abs_fma', (F_BOOL, ['s0abs_fma'])),
   ('s2neg_fma', (F_BOOL, ['s2neg_fma'])),
   ('sat_fma', (F_BOOL, ['sat_fma'])),

   ('rsvd1_fma', (F_UINT3, ['rsvd1_fma'], 0)),
   ('lp_fma', (F_BOOL, ['lp_fma'])),
   ('s1abs_fma', (F_BOOL, ['s1abs_fma'])),
   ('s1neg_fma', (F_BOOL, ['s1neg_fma'])),
   ('s2flr_fma', (F_BOOL, ['s2flr_fma'])),
   ('s2abs_fma', (F_BOOL, ['s2abs_fma'])),

   # int32_64
   ('s_i3264', (F_BOOL, ['s_i3264'])),
   ('s2neg_i3264', (F_BOOL, ['s2neg_i3264'])),
   ('int32_64_op', (F_INT32_64_OP, ['int32_64_op'])),

   ('rsvd1_i3264', (F_UINT2, ['rsvd1_i3264_', 'rsvd1_i3264'], 0)),
   ('cin_i3264', (F_BOOL, ['cin_i3264'])),
   ('s1neg_i3264', (F_BOOL, ['s1neg_i3264'])),
   ('s0neg_i3264', (F_BOOL, ['s0neg_i3264'])),
   ('s0abs_i3264', (F_BOOL, ['s0abs_i3264'])),
   ('s1abs_i3264', (F_BOOL, ['s1abs_i3264'])),
   ('s2abs_i3264', (F_BOOL, ['s2abs_i3264'])),

   # movc
   ('movw1', (F_MOVW01, ['movw1'])),
   ('movw0', (F_MOVW01, ['movw0'])),

   ('rsvd1_movc', (F_UINT2, ['rsvd1_movc'], 0)),
   ('maskw0', (F_MASKW0, ['maskw0'])),
   ('aw', (F_BOOL, ['aw'])),
   ('p2end_movc', (F_BOOL, ['p2end_movc'])),

   # tst
   ('tst_op_3bit', (F_TST_OP3, ['tst_op_2_0'])),
   ('pwen_tst', (F_BOOL, ['pwen_tst'])),

   ('tst_type', (F_TST_TYPE, ['tst_type'])),
   ('p2end_tst', (F_BOOL, ['p2end_tst'])),
   ('tst_elem', (F_UINT2, ['tst_elem'])),
   ('rsvd1_tst', (F_UINT1, ['rsvd1_tst'], 0)),
   ('tst_op_4bit', (F_TST_OP, ['tst_op_3', 'tst_op_2_0'])),
])

I_FADD = bit_struct(
name='fadd',
bit_set=I_MAIN,
field_mappings=[
   ('main_op', 'main_op', 'fadd'),

   ('sat', 'sat_fam'),
   ('s0neg', 's0neg_fam'),
   ('s0abs', 's0abs_fam'),
   ('s1abs', 's1abs_fam'),
   ('s0flr', 's0flr_fam'),
])

I_FADD_LP = bit_struct(
name='fadd_lp',
bit_set=I_MAIN,
field_mappings=[
   ('main_op', 'main_op', 'fadd_lp'),

   ('sat', 'sat_fam'),
   ('s0neg', 's0neg_fam'),
   ('s0abs', 's0abs_fam'),
   ('s1abs', 's1abs_fam'),
   ('s0flr', 's0flr_fam'),
])

I_FMUL = bit_struct(
name='fmul',
bit_set=I_MAIN,
field_mappings=[
   ('main_op', 'main_op', 'fmul'),

   ('sat', 'sat_fam'),
   ('s0neg', 's0neg_fam'),
   ('s0abs', 's0abs_fam'),
   ('s1abs', 's1abs_fam'),
   ('s0flr', 's0flr_fam'),
])

I_FMUL_LP = bit_struct(
name='fmul_lp',
bit_set=I_MAIN,
field_mappings=[
   ('main_op', 'main_op', 'fmul_lp'),

   ('sat', 'sat_fam'),
   ('s0neg', 's0neg_fam'),
   ('s0abs', 's0abs_fam'),
   ('s1abs', 's1abs_fam'),
   ('s0flr', 's0flr_fam'),
])

# Covers FRCP, FRSQ, FLOG, FEXP, FLOGCN, BYP,
# FDSX, FDSY, FDSXF, FDSYF, FSINC, FARCTANC
I_SNGL = bit_struct(
name='sngl',
bit_set=I_MAIN,
field_mappings=[
   ('main_op', 'main_op', 'sngl'),
   ('ext0', 'ext0', 0),
   ('sngl_op', 'sngl_op'),
])

I_SNGL_EXT = bit_struct(
name='sngl_ext',
bit_set=I_MAIN,
field_mappings=[
   ('main_op', 'main_op', 'sngl'),
   ('ext0', 'ext0', 1),
   ('sngl_op', 'sngl_op'),

   ('rsvd1', 'rsvd1_sngl'),
   ('rsvd1_', 'rsvd1_sngl_'),
   ('s0neg', 's0neg_sngl'),
   ('s0abs', 's0abs_sngl'),
])

# F16SOP
# TODO

# GCMP
I_GCMP = bit_struct(
name='gcmp',
bit_set=I_MAIN,
field_mappings=[
   ('main_op', 'main_op', 'sngl'),
   ('ext0', 'ext0', 0),
   ('sngl_op', 'sngl_op', 'gamma'),
])

I_GCMP_EXT = bit_struct(
name='gcmp_ext',
bit_set=I_MAIN,
field_mappings=[
   ('main_op', 'main_op', 'sngl'),
   ('ext0', 'ext0', 1),
   ('sngl_op', 'sngl_op', 'gamma'),

   ('rsvd1', 'rsvd1_sngl'),
   ('gammaop', 'gammaop', 'cmp'),
   ('s0neg', 's0neg_sngl'),
   ('s0abs', 's0abs_sngl'),
])

# GEXP
I_GEXP = bit_struct(
name='gexp',
bit_set=I_MAIN,
field_mappings=[
   ('main_op', 'main_op', 'sngl'),
   ('ext0', 'ext0', 1),
   ('sngl_op', 'sngl_op', 'gamma'),

   ('rsvd1', 'rsvd1_sngl'),
   ('gammaop', 'gammaop', 'exp'),
   ('s0neg', 's0neg_sngl'),
   ('s0abs', 's0abs_sngl'),
])

# PCK
I_PCK = bit_struct(
name='pck',
bit_set=I_MAIN,
field_mappings=[
   ('main_op', 'main_op', 'sngl'),
   ('ext0', 'ext0', 1),
   ('sngl_op', 'sngl_op', 'pck'),

   ('prog', 'prog'),
   ('rtz', 'rtz'),
   ('scale', 'scale'),
   ('pck_format', 'pck_format'),
])

# UPCK
I_UPCK = bit_struct(
name='upck',
bit_set=I_MAIN,
field_mappings=[
   ('main_op', 'main_op', 'sngl'),
   ('ext0', 'ext0', 1),
   ('sngl_op', 'sngl_op', 'pck'),

   ('elem', 'upck_elem'),
   ('scale_rtz', 'scale_rtz'),
   ('pck_format', 'pck_format'),
])

# FRED
I_FRED = bit_struct(
name='fred',
bit_set=I_MAIN,
field_mappings=[
   ('main_op', 'main_op', 'sngl'),
   ('ext0', 'ext0', 1),
   ('sngl_op', 'sngl_op', 'red'),

   ('red_part', 'red_part'),
   ('iter', 'iter'),
   ('red_type', 'red_type'),
   ('pwen', 'pwen_red'),
   ('s0neg', 's0neg_sngl'),
   ('s0abs', 's0abs_sngl'),
])

I_INT8_16 = bit_struct(
name='int8_16',
bit_set=I_MAIN,
field_mappings=[
   ('main_op', 'main_op', 'int8_16'),
   ('ext0', 'ext0', 0),
   ('int8_16_op', 'int8_16_op'),
   ('s', 's_i816'),
   ('f', 'f_i816'),
])

I_INT8_16_EXT = bit_struct(
name='int8_16_ext',
bit_set=I_MAIN,
field_mappings=[
   ('main_op', 'main_op', 'int8_16'),
   ('ext0', 'ext0', 1),
   ('int8_16_op', 'int8_16_op'),
   ('s', 's_i816'),
   ('f', 'f_i816'),

   ('s2ch', 's2ch'),
   ('rsvd1', 'rsvd1_i816'),
   ('s2neg', 's2neg_i816'),
   ('s2abs', 's2abs_i816'),
   ('s1abs', 's1abs_i816'),
   ('s0neg', 's0neg_i816'),
   ('s0abs', 's0abs_i816'),
   ('sat', 'sat_i816'),
])

I_INT8_16_EXT_SEL = bit_struct(
name='int8_16_ext_sel',
bit_set=I_MAIN,
field_mappings=[
   ('main_op', 'main_op', 'int8_16'),
   ('ext0', 'ext0', 1),
   ('int8_16_op', 'int8_16_op'),
   ('s', 's_i816'),
   ('f', 'f_i816'),

   ('s2ch', 's2ch'),
   ('rsvd1', 'rsvd1_i816'),
   ('s2neg', 's2neg_i816'),
   ('s2abs', 's2abs_i816'),
   ('s1abs', 's1abs_i816'),
   ('s0neg', 's0neg_i816'),
   ('s0abs', 's0abs_i816'),
   ('sat', 'sat_i816'),

   ('rsvd2', 'rsvd2_i816'),
   ('s1ch', 's1ch'),
   ('s0ch', 's0ch'),
])

I_FMAD = bit_struct(
name='fmad',
bit_set=I_MAIN,
field_mappings=[
   ('main_op', 'main_op', 'fmad_movc'),
   ('ext0', 'ext0', 0),

   ('s0neg', 's0neg_fma'),
   ('s0abs', 's0abs_fma'),
   ('s2neg', 's2neg_fma'),
   ('sat', 'sat_fma'),
])

I_FMAD_EXT = bit_struct(
name='fmad_ext',
bit_set=I_MAIN,
field_mappings=[
   ('main_op', 'main_op', 'fmad_movc'),
   ('ext0', 'ext0', 1),

   ('s0neg', 's0neg_fma'),
   ('s0abs', 's0abs_fma'),
   ('s2neg', 's2neg_fma'),
   ('sat', 'sat_fma'),

   ('rsvd1', 'rsvd1_fma'),
   ('lp', 'lp_fma'),
   ('s1abs', 's1abs_fma'),
   ('s1neg', 's1neg_fma'),
   ('s2flr', 's2flr_fma'),
   ('s2abs', 's2abs_fma'),
])

I_INT32_64 = bit_struct(
name='int32_64',
bit_set=I_MAIN,
field_mappings=[
   ('main_op', 'main_op', 'int32_64_tst'),
   ('ext0', 'ext0', 0),

   ('s', 's_i3264'),
   ('s2neg', 's2neg_i3264'),
   ('int32_64_op', 'int32_64_op'),
])

I_INT32_64_EXT = bit_struct(
name='int32_64_ext',
bit_set=I_MAIN,
field_mappings=[
   ('main_op', 'main_op', 'int32_64_tst'),
   ('ext0', 'ext0', 1),

   ('s', 's_i3264'),
   ('s2neg', 's2neg_i3264'),
   ('int32_64_op', 'int32_64_op'),

   ('rsvd1', 'rsvd1_i3264'),

   ('cin', 'cin_i3264'),
   ('s1neg', 's1neg_i3264'),
   ('s0neg', 's0neg_i3264'),
   ('s0abs', 's0abs_i3264'),
   ('s1abs', 's1abs_i3264'),
   ('s2abs', 's2abs_i3264'),
])

I_MOVC = bit_struct(
name='movc',
bit_set=I_MAIN,
field_mappings=[
   ('main_op', 'main_op', 'fmad_movc'),
   ('ext0', 'ext0', 0),

   ('movw1', 'movw1'),
   ('movw0', 'movw0'),
])

I_MOVC_EXT = bit_struct(
name='movc_ext',
bit_set=I_MAIN,
field_mappings=[
   ('main_op', 'main_op', 'fmad_movc'),
   ('ext0', 'ext0', 1),

   ('movw1', 'movw1'),
   ('movw0', 'movw0'),

   ('rsvd1', 'rsvd1_movc'),
   ('maskw0', 'maskw0'),
   ('aw', 'aw'),
   ('p2end', 'p2end_movc'),
])

I_TST = bit_struct(
name='tst',
bit_set=I_MAIN,
field_mappings=[
   ('main_op', 'main_op', 'int32_64_tst'),
   ('ext0', 'ext0', 0),

   ('tst_op', 'tst_op_3bit'),
   ('pwen', 'pwen_tst'),
])

I_TST_EXT = bit_struct(
name='tst_ext',
bit_set=I_MAIN,
field_mappings=[
   ('main_op', 'main_op', 'int32_64_tst'),
   ('ext0', 'ext0', 1),

   ('tst_op', 'tst_op_4bit'),
   ('pwen', 'pwen_tst'),

   ('type', 'tst_type'),
   ('p2end', 'p2end_tst'),
   ('elem', 'tst_elem'),
   ('rsvd1', 'rsvd1_tst'),
])
