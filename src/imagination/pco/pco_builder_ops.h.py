# Copyright © 2024 Imagination Technologies Ltd.
# SPDX-License-Identifier: MIT

from mako.template import Template
from pco_ops import *

template = """/*
 * Copyright © 2024 Imagination Technologies Ltd.
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef PCO_BUILDER_OPS_H
#define PCO_BUILDER_OPS_H

/**
 * \\file pco_builder_ops.h
 *
 * \\brief PCO op building functions.
 */

#include "pco_internal.h"
#include "pco_common.h"
#include "pco_ops.h"
#include "util/macros.h"

#include <assert.h>
#include <stdbool.h>

/** Op mod getting/setting. */
static inline
bool pco_instr_has_mod(const pco_instr *instr, enum pco_op_mod mod)
{
   assert(mod < _PCO_OP_MOD_COUNT);
   return (pco_op_info[instr->op].mods & (1ULL << mod)) != 0;
}

static inline
void pco_instr_set_mod(pco_instr *instr, enum pco_op_mod mod, uint32_t val)
{
   assert(mod < _PCO_OP_MOD_COUNT);
   unsigned mod_index = pco_op_info[instr->op].mod_map[mod];
   assert(mod_index > 0);
   instr->mod[mod_index - 1] = val;
}

static inline
uint32_t pco_instr_get_mod(pco_instr *instr, enum pco_op_mod mod)
{
   assert(mod < _PCO_OP_MOD_COUNT);
   unsigned mod_index = pco_op_info[instr->op].mod_map[mod];
   assert(mod_index > 0);
   return instr->mod[mod_index - 1];
}

% for op_mod in op_mods.values():
static inline
bool pco_instr_has_${op_mod.t.tname}(const pco_instr *instr)
{
   return pco_instr_has_mod(instr, ${op_mod.cname});
}

static inline
void pco_instr_set_${op_mod.t.tname}(pco_instr *instr, ${op_mod.t.name} val)
{
   return pco_instr_set_mod(instr, ${op_mod.cname}, val);
}

static inline
${op_mod.t.name} pco_instr_get_${op_mod.t.tname}(pco_instr *instr)
{
   return pco_instr_get_mod(instr, ${op_mod.cname});
}

% endfor
% for op in ops.values():
   % if bool(op.op_mods):
struct ${op.bname}_mods {
      % for op_mod in op.op_mods:
  ${op_mod.t.name} ${op_mod.t.tname};
      % endfor
};
   % endif
#define ${op.bname}(${op.builder_params[1]}${op.builder_params[2]}) _${op.bname}(${op.builder_params[1]}${op.builder_params[3]})
static
pco_instr *_${op.bname}(${op.builder_params[0]})
{
   pco_instr *instr = pco_instr_create(pco_cursor_func(b->cursor),
                                       ${op.cname.upper()},
                                       ${op.num_dests},
                                       ${op.num_srcs});

   % if op.has_target_cf_node:
   instr->target_cf_node = target_cf_node;
   % endif
   % for d in range(op.num_dests):
   instr->dest[${d}] = dest${d};
   % endfor
   % for s in range(op.num_srcs):
   instr->src[${s}] = src${s};
   % endfor

   % for op_mod in op.op_mods:
      % if op_mod.t.nzdefault is None:
   pco_instr_set_${op_mod.t.tname}(instr, mods.${op_mod.t.tname});
      % else:
   pco_instr_set_${op_mod.t.tname}(instr, !mods.${op_mod.t.tname} ? ${op_mod.t.nzdefault} : mods.${op_mod.t.tname});
      % endif
   % endfor

   pco_builder_insert_instr(b, instr);
   return instr;
}

% endfor
#endif /* PCO_BUILDER_OPS_H */"""

def main():
   print(Template(template).render(ops=ops, op_mods=op_mods))

if __name__ == '__main__':
   main()
