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
F_UINT2_POS_INC = field_type('uint2_pos_inc', BaseType.uint, 2, dec_bits=3, check='val >= 1 && val <= 4', encode='val - 1')
## [1..16] -> [1..15,0]
F_UINT4_POS_WRAP = field_type('uint4_pos_wrap', BaseType.uint, 4, dec_bits=5, check='val >= 1 && val <= 16', encode='val % 16')
## 32-bit offset; lowest bit == 0b0.
F_OFFSET31 = field_type('offset31', BaseType.uint, 31, dec_bits=32, check='!(val & 0b1)', encode='val >> 1')
## 8-bit value; lowest 2 bits == 0b00.
F_UINT6MUL4 = field_type('uint6mul4', BaseType.uint, 6, dec_bits=8, check='!(val & 0b11)', encode='val >> 2')

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
