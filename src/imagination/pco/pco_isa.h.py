# Copyright © 2024 Imagination Technologies Ltd.
# SPDX-License-Identifier: MIT

from mako.template import Template
from pco_isa import *

template = """/*
 * Copyright © 2024 Imagination Technologies Ltd.
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef PCO_ISA_H
#define PCO_ISA_H

/**
 * \\file pco_isa.h
 *
 * \\brief PCO ISA definitions.
 */

#include "pco_common.h"
#include "util/macros.h"

#include <stdbool.h>

% for _bit_set_name, bit_set in bit_sets.items():
/** ${_bit_set_name} */
   % for bit_struct in bit_set.bit_structs.values():
struct ${bit_struct.name} {
      % for type, field, bits in bit_struct.struct_fields.values():
   ${type} ${field} : ${bits};
      % endfor
};

   % endfor
% endfor
#endif /* PCO_ISA_H */"""

def main():
   print(Template(template).render(bit_sets=bit_sets))

if __name__ == '__main__':
   main()
