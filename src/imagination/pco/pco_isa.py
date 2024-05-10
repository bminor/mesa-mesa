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
