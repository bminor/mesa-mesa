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
