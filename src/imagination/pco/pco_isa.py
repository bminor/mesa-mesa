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
