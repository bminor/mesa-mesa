# Copyright © 2024 Imagination Technologies Ltd.
# SPDX-License-Identifier: MIT

from mako.template import Template
from pco_ops import *

template = """/*
 * Copyright © 2024 Imagination Technologies Ltd.
 *
 * SPDX-License-Identifier: MIT
 */

/**
 * \\file pco_info.c
 *
 * \\brief PCO info structures.
 */

#include "pco_common.h"
#include "pco_internal.h"
#include "pco_isa.h"
#include "pco_ops.h"

const struct pco_op_info pco_op_info[_PCO_OP_COUNT] = {
% for op in ops.values():
   [${op.cname.upper()}] = {
      .str = "${op.name}",
      .num_dests = ${op.num_dests},
      .num_srcs = ${op.num_srcs},
      .mods = ${op.cop_mods},
      .mod_map = {
% for mod, index in op.op_mod_map.items():
         [${mod}] = ${index},
% endfor
      },
      .dest_mods = {
% for index, cdest_mods in op.cdest_mods.items():
         [${index}] = ${cdest_mods},
% endfor
      },
      .src_mods = {
% for index, csrc_mods in op.csrc_mods.items():
         [${index}] = ${csrc_mods},
% endfor
      },
      .is_pseudo = ${str(op.is_pseudo).lower()},
      .has_target_cf_node = ${str(op.has_target_cf_node).lower()},
   },
% endfor
};

const struct pco_op_mod_info pco_op_mod_info[_PCO_OP_MOD_COUNT] = {
% for name, (mod, cname, ctype) in op_mods.items():
   [${cname}] = {
      .print_early = ${str(mod.print_early).lower()},
      .type = ${ctype},
   % if mod.base_type == BaseType.enum:
      .strs = (const char * []){
      % for enum_cname, value, string in mod.enum.elems.values():
         [${enum_cname}] = "${string}",
      % endfor
      },
   % else:
      .str = "${name}",
   % endif
   % if mod.nzdefault is not None:
      .nzdefault = ${mod.nzdefault},
   % endif
   },
% endfor
};

const struct pco_ref_mod_info pco_ref_mod_info[_PCO_REF_MOD_COUNT] = {
% for name, (mod, cname, ctype) in ref_mods.items():
   [${cname}] = {
      .type = ${ctype},
   % if mod.base_type == BaseType.enum:
      .is_bitset = ${str(mod.enum.is_bitset).lower()},
      .strs = (const char * []){
      % for enum_cname, value, string in mod.enum.elems.values():
         [${enum_cname}] = "${string}",
      % endfor
      },
   % else:
      .str = "${name}",
   % endif
   },
% endfor
};"""

def main():
   print(Template(template).render(BaseType=BaseType, ops=ops, op_mods=op_mods, ref_mods=ref_mods))

if __name__ == '__main__':
   main()
