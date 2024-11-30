# Copyright © 2024 Imagination Technologies Ltd.
# SPDX-License-Identifier: MIT

from pco_isa import *
from pco_ops import *

# Enum mappings.

class EnumMap(object):
   def __init__(self, name, type_from, type_to, mappings, both_bitsets, pass_zero_val):
      self.name = name
      self.type_from = type_from
      self.type_to = type_to
      self.mappings = mappings
      self.both_bitsets = both_bitsets
      self.pass_zero_val = pass_zero_val

enum_maps = {}

def enum_map(enum_from, enum_to, mappings, pass_zero=None):
   # Only allow passing zero through if this is a bitset.
   assert pass_zero is None or enum_from.enum.is_bitset

   key = (enum_from, enum_to)
   assert key not in enum_maps.keys(), f'Duplicate enum mapping for "{enum_from.tname}" to "{enum_to.tname}".'

   assert enum_from.base_type == BaseType.enum
   assert enum_to.base_type == BaseType.enum

   # Ensure the validity of the enum_from elements.
   assert set([from_elem for from_elem, to_elem in mappings]).issubset(set(enum_from.enum.elems.keys())), f'Invalid enum_from spec in enum mapping for "{enum_from.tname}" to "{enum_to.tname}".'

   # Ensure the validity of the enum_to elements.
   assert set([to_elem for from_elem, to_elem in mappings]).issubset(set(enum_to.enum.elems.keys())), f'Invalid enum_to spec in enum mapping for "{enum_from.tname}" to "{enum_to.tname}".'

   _mappings = []
   for elem_from, elem_to in mappings:
      _mappings.append((enum_from.enum.elems[elem_from].cname, enum_to.enum.elems[elem_to].cname))

   pass_zero_val = None
   if pass_zero is not None:
      if isinstance(pass_zero, int):
         pass_zero_val = str(pass_zero)
      elif isinstance(pass_zero, str):
         assert pass_zero in enum_to.enum.elems.keys()
         pass_zero_val = enum_to.enum.elems[pass_zero].cname
      elif isinstance(pass_zero, list):
         assert all(elem_to in enum_to.enum.elems.keys() for elem_to in pass_zero)
         pass_zero_val = ' | '.join([enum_to.enum.elems[elem_to].cname for elem_to in pass_zero])
      else:
         assert False

   name = f'{prefix}_map_{enum_from.tname}_to_{enum_to.tname}'
   both_bitsets = enum_from.enum.is_bitset and enum_to.enum.is_bitset
   enum_maps[key] = EnumMap(name, enum_from.name, enum_to.name, _mappings, both_bitsets, pass_zero_val)

enum_map(OM_EXEC_CND.t, F_CC, [
   ('e1_zx', 'e1_zx'),
   ('e1_z1', 'e1_z1'),
   ('ex_zx', 'ex_zx'),
   ('e1_z0', 'e1_z0'),
])

enum_map(REG_CLASS, F_REGBANK, [
   ('temp', 'temp'),
   ('vtxin', 'vtxin'),
   ('coeff', 'coeff'),
   ('shared', 'shared'),
   ('index', 'idx0'),
   ('spec', 'special'),
   ('intern', 'special'),
   ('const', 'special'),
   ('pixout', 'special'),
   ('global', 'special'),
   ('slot', 'special'),
])

enum_map(REG_CLASS, F_IDXBANK, [
   ('temp', 'temp'),
   ('vtxin', 'vtxin'),
   ('coeff', 'coeff'),
   ('shared', 'shared'),
   ('index', 'idx'),
   ('pixout', 'pixout'),
])

enum_map(IO, F_IS0_SEL, [
   ('s0', 's0'),
   ('s1', 's1'),
   ('s2', 's2'),
   ('s3', 's3'),
   ('s4', 's4'),
   ('s5', 's5'),
])

enum_map(IO, F_IS1_SEL, [
   ('ft0', 'ft0'),
   ('fte', 'fte'),
])

enum_map(IO, F_IS2_SEL, [
   ('ft1', 'ft1'),
   ('fte', 'fte'),
])

enum_map(IO, F_IS3_SEL, [
   ('ft0', 'ft0'),
   ('ft1', 'ft1'),
   ('fte', 'fte'),
])

enum_map(IO, F_IS4_SEL, [
   ('ft0', 'ft0'),
   ('ft1', 'ft1'),
   ('ft2', 'ft2'),
   ('fte', 'fte'),
])

enum_map(IO, F_IS5_SEL, [
   ('ft0', 'ft0'),
   ('ft1', 'ft1'),
   ('ft2', 'ft2'),
   ('fte', 'fte'),
])

enum_map(OM_ITR_MODE.t, F_ITER_MODE, [
   ('pixel', 'pixel'),
   ('sample', 'sample'),
   ('centroid', 'centroid'),
])

enum_map(OM_PCK_FMT.t, F_PCK_FORMAT, [
   ('u8888', 'u8888'),
   ('s8888', 's8888'),
   ('o8888', 'o8888'),
   ('u1616', 'u1616'),
   ('s1616', 's1616'),
   ('o1616', 'o1616'),
   ('u32', 'u32'),
   ('s32', 's32'),
   ('u1010102', 'u1010102'),
   ('s1010102', 's1010102'),
   ('u111110', 'u111110'),
   ('s111110', 's111110'),
   ('f111110', 'f111110'),
   ('f16f16', 'f16f16'),
   ('f32', 'f32'),
   ('cov', 'cov'),
   ('u565u565', 'u565u565'),
   ('d24s8', 'd24s8'),
   ('s8d24', 's8d24'),
   ('f32_mask', 'f32_mask'),
   ('2f10f10f10', '2f10f10f10'),
   ('s8888ogl', 's8888ogl'),
   ('s1616ogl', 's1616ogl'),
   ('zero', 'zero'),
   ('one', 'one'),
])

enum_map(OM_SCHED.t, F_SCHED_CTRL, [
   ('none', 'none'),
   ('swap', 'swap'),
   ('wdf', 'wdf'),
])

enum_map(OM_TST_OP_MAIN.t, F_TST_OP, [
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

enum_map(OM_TST_TYPE_MAIN.t, F_TST_TYPE, [
   ('f32', 'f32'),
   ('u16', 'u16'),
   ('s16', 's16'),
   ('u8', 'u8'),
   ('s8', 's8'),
   ('u32', 'u32'),
   ('s32', 's32'),
])

enum_map(RM_ELEM.t, F_MASKW0, [
   ('e0', 'e0'),
   ('e1', 'e1'),
   ('e2', 'e2'),
   ('e3', 'e3'),
], pass_zero=['e0', 'e1', 'e2', 'e3'])

encode_maps = {}
group_maps = {}

# Instruction encoding mapping.
class EncodeMap(object):
   def __init__(self, name, cop_name, variants):
      self.name = name
      self.cop_name = cop_name
      self.variants = variants

def encode_map(op, encodings):
   assert op.bname not in encode_maps.keys(), f'Duplicate encode mapping for op "{op.name}".'

   encode_variants = []
   default_variant = encodings[0]
   first_variant = encodings[1] if len(encodings) > 1 else None

   for encoding_variant in encodings:
      isa_op, isa_op_fields, *_ = encoding_variant

      # Ensure we're speccing everything in the op fields.
      assert set(isa_op.struct_fields.keys()) == set([field for field, val_spec in isa_op_fields]), f'Invalid field spec in isa op "{isa_op.bsname}" mapping for op "{op.name}".'

      variant = isa_op.name.upper()
      encode_variant = f'{isa_op.name}_encode({{0}}'

      for isa_op_field, val_spec in isa_op_fields:
         struct_field = isa_op.struct_fields[isa_op_field]
         encode_variant += f', .{isa_op_field} = '
         if isinstance(val_spec, bool):
            encode_variant += str(val_spec).lower()
         elif isinstance(val_spec, int):
            encode_variant += str(val_spec)
         elif isinstance(val_spec, str):
            assert struct_field.type.base_type == BaseType.enum

            enum = struct_field.type.enum
            assert enum.parent is None
            assert val_spec in enum.elems.keys(), f'Invalid enum element "{val_spec}" in field "{isa_op_field}" in isa op "{isa_op.bsname}" mapping for op "{op.name}".'

            encode_variant += enum.elems[val_spec].cname
         elif isinstance(val_spec, OpMod):
            assert val_spec in op.op_mods, f'Op mod "{val_spec.t.tname}" was specified but not valid in isa op "{isa_op.bsname}" mapping for op "{op.name}".'

            if struct_field.type.base_type == BaseType.enum:
               enum_map_key = (val_spec.t, struct_field.type)
               assert enum_map_key in enum_maps.keys(), f'Op mod enum "{val_spec.t.tname}" was specified but no mapping from enum "{struct_field.type.tname}" was found in mapping for op "{op.name}".'
               enum_mapping = enum_maps[enum_map_key]
               encode_variant += f'{enum_mapping.name}(pco_instr_get_mod({{1}}, {val_spec.cname}))'
            else:
               encode_variant += f'pco_instr_get_mod({{1}}, {val_spec.cname})'

         elif isinstance(val_spec, tuple):
            mod, origin = val_spec
            if isinstance(mod, RefMod):
               # This is different from op mods, in that enum maps are *optional*.
               enum_map_key = (mod.t, struct_field.type)
               if struct_field.type.base_type == BaseType.enum and enum_map_key in enum_maps.keys():
                  enum_mapping = enum_maps[enum_map_key]
                  encode_variant += f'{enum_mapping.name}({{1}}->{origin}.{mod.t.tname})'
               else:
                  encode_variant += f'{{1}}->{origin}.{mod.t.tname}'

            elif isinstance(mod, str):
               encode_variant += f'{mod}({{1}}->{origin})'
            else:
               assert False
         elif isinstance(val_spec, dict):
            assert len(val_spec) == 1
            _bin_op, mod_list = next(iter(val_spec.items()))

            # No point doing a binop on a single item.
            assert len(mod_list) > 1

            # Only op mods supported for now.
            assert all(isinstance(mod, OpMod) for mod in mod_list)
            assert all(mod in op.op_mods for mod in mod_list)

            # Make sure mods are all bools, can't do binops on other types.
            assert all(mod.t.base_type == BaseType.bool for mod in mod_list)

            if _bin_op == 'or':
               bin_op = '||'
            elif _bin_op == 'and':
               bin_op = '&&'
            else:
               assert False

            encode_variant += f' {bin_op} '.join([f'pco_instr_get_mod({{1}}, {mod.cname})' for mod in mod_list])
         else:
            assert False, f'Invalid value spec for field "{isa_op_field}" in isa op "{isa_op.bsname}" mapping for op "{op.name}".'
      encode_variant += ')'

      conds_variant = ''
      if encoding_variant != default_variant:
         conds_variant = 'if (' if encoding_variant == first_variant else 'else if ('

         isa_op_conds = encoding_variant[2]
         for i, isa_op_cond in enumerate(isa_op_conds):
            if i > 0:
               conds_variant += ' && '

            if isinstance(isa_op_cond, tuple) and len(isa_op_cond) == 3:
               mod, origin, cond = isa_op_cond
               assert isinstance(mod, RefMod)
               conds_variant += f'{{1}}->{origin}.{mod.t.tname} {cond}'
            elif isinstance(isa_op_cond, tuple) and len(isa_op_cond) == 2:
               mod, cond = isa_op_cond
               assert isinstance(mod, OpMod)
               assert mod in op.op_mods
               conds_variant += f'pco_instr_get_mod({{1}}, {mod.cname}) {cond}'
            else:
               assert False
         conds_variant += ')'

      encode_variants.append((variant, encode_variant, conds_variant))

   name = op.bname
   cop_name = op.cname.upper()
   encode_maps[name] = EncodeMap(name, cop_name, encode_variants)

# Instruction group mapping.
class GroupMap(object):
   def __init__(self, name, cop_name, mapping_sets):
      self.name = name
      self.mapping_sets = mapping_sets
      self.cop_name = cop_name

def group_map(op, hdr, enc_ops, srcs=[], iss=[], dests=[]):
   assert op.bname not in group_maps.keys(), f'Duplicate group mapping for op "{op.name}".'
   assert op.op_type != 'hw_direct', f'HW direct op "{op.name}" cannot have a group mapping.'

   is_self = len(enc_ops) == 1 and enc_ops[0][1] == op

   mapping_sets = []

   hdr_variant, hdr_fields = hdr

   # Ensure we're speccing everything in the header except length and da (need to be set later).
   assert set(hdr_variant.struct_fields.keys()) == set([hdr_field for hdr_field, val_spec in hdr_fields] + ['length', 'da']), f'Invalid field spec in hdr mapping for op "{op.name}".'

   # Add alutype setting after the check above, as it's actually a fixed value.
   hdr_fields.append(('alutype', hdr_variant.bsname))

   # Emit header mappings.
   hdr_mappings = []
   for hdr_field, val_spec in hdr_fields:
      field = hdr_variant.bit_set.fields[hdr_field]
      if isinstance(val_spec, bool):
         value = str(val_spec).lower()
         hdr_mappings.append(f'{{}}->hdr.{hdr_field} = {str(val_spec).lower()};')
      elif isinstance(val_spec, int):
         value = val_spec
         hdr_mappings.append(f'{{}}->hdr.{hdr_field} = {val_spec};')
      elif isinstance(val_spec, str):
         assert field.field_type.base_type == BaseType.enum

         enum = field.field_type.enum
         assert enum.parent is None
         assert val_spec in enum.elems.keys(), f'Invalid enum element "{val_spec}" in field "{hdr_field}" in hdr mapping for op "{op.name}".'

         hdr_mappings.append(f'{{}}->hdr.{hdr_field} = {enum.elems[val_spec].cname};')
      elif isinstance(val_spec, OpMod):
         assert val_spec in op.op_mods, f'Op mod "{val_spec.t.tname}" was specified but not valid in hdr mapping for op "{op.name}".'

         if field.field_type.base_type == BaseType.enum:
            enum_map_key = (val_spec.t, field.field_type)
            assert enum_map_key in enum_maps.keys(), f'Op mod enum "{val_spec.t.tname}" was specified but no mapping from enum "{field.field_type.tname}" was found in hdr mapping for op "{op.name}".'
            enum_mapping = enum_maps[enum_map_key]
            hdr_mappings.append(f'{{}}->hdr.{hdr_field} = {enum_mapping.name}(pco_instr_get_mod({{}}, {val_spec.cname}));')
         else:
            hdr_mappings.append(f'{{}}->hdr.{hdr_field} = pco_instr_get_mod({{}}, {val_spec.cname});')

         if val_spec.t.unset and is_self:
            reset_val = 0 if val_spec.t.nzdefault is None else val_spec.t.nzdefault
            hdr_mappings.append(f'pco_instr_set_mod({{1}}, {val_spec.cname}, {reset_val});')
      elif isinstance(val_spec, tuple):
         mod, origin = val_spec
         assert isinstance(mod, str) and isinstance(origin, str)
         hdr_mappings.append(f'{{}}->hdr.{hdr_field} = {mod}({{}}->{origin});')
      elif isinstance(val_spec, list):
         assert field.field_type.base_type == BaseType.enum

         enum = field.field_type.enum
         assert enum.parent is None
         assert enum.is_bitset

         assert all(elem in enum.elems.keys() for elem in val_spec), f'Invalid enum element "{elem}" in field "{hdr_field}" in hdr mapping for op "{op.name}".'

         val = ' | ' .join(enum.elems[elem].cname for elem in val_spec)
         hdr_mappings.append(f'{{}}->hdr.{hdr_field} = {val};')
      else:
         assert False, f'Invalid value spec for field "{hdr_field}" in hdr mapping for op "{op.name}".'

   assert bool(hdr_mappings)
   mapping_sets.append(hdr_mappings);

   # Emit instruction op mappings.
   op_mappings = []
   if is_self:
      assert len(enc_ops[0]) == 2
      _phase = enc_ops[0][0]
      assert _phase in OP_PHASE.enum.elems.keys()
      phase = OP_PHASE.enum.elems[_phase].cname

      op_mappings.append(f'list_del(&{{1}}->link);')
      op_mappings.append(f'{{}}->instrs[{phase}] = {{}};')
      op_mappings.append(f'ralloc_steal(igrp, {{1}});')
   else:
      for _phase, enc_op, *_enc_spec in enc_ops:
         assert _phase in OP_PHASE.enum.elems.keys()
         phase = OP_PHASE.enum.elems[_phase].cname

         assert enc_op.bname in encode_maps.keys(), f'Op "{enc_op.name}" used in group mapping for "{op.name}" has no encode mapping defined.'

         enc_dests = _enc_spec[0] if len(_enc_spec) > 0 else []
         enc_srcs = _enc_spec[1] if len(_enc_spec) > 1 else []
         enc_op_mods = _enc_spec[2] if len(_enc_spec) > 2 else []

         enc_mapping = f'{{0}}->instrs[{phase}] = {enc_op.bname}_({{1}}->parent_func'

         for _ref in enc_dests + enc_srcs:
            if _ref in IO.enum.elems.keys():
               io = IO.enum.elems[_ref]
               enc_mapping += f', pco_ref_io({io.cname})'
            elif _ref == '_':
               enc_mapping += f', pco_ref_null()'
            elif len(_ref) == 2:
               mod_func, ref = _ref
               enc_mapping += f', {mod_func}({{1}}->{ref})'
            else:
               enc_mapping += f', {{1}}->{_ref}'

         for op_mod, _val in enc_op_mods:
            assert isinstance(op_mod, OpMod)
            assert op_mod in enc_op.op_mods

            if isinstance(_val, str):
               assert op_mod.t.base_type == BaseType.enum
               enum = op_mod.t.enum
               assert enum.parent is None
               assert _val in enum.elems.keys()
               val = enum.elems[_val].cname
            elif isinstance(_val, bool):
               assert op_mod.t.base_type == BaseType.bool
               val = str(_val).lower()
            elif isinstance(_val, int):
               assert op_mod.t.base_type == BaseType.uint
               val = str(_val)
            elif isinstance(_val, OpMod):
               assert op_mod in op.op_mods
               val = f'pco_instr_get_mod({{1}}, {_val.cname})'
            else:
               assert False

            enc_mapping += f', .{op_mod.t.tname} = {val}'

         enc_mapping += ');'

         op_mappings.append(enc_mapping)

      op_mappings.append(f'pco_instr_delete({{1}});')

   assert bool(op_mappings)
   mapping_sets.append(op_mappings);

   # Emit source mappings.
   src_mappings = []
   for src, *_spec in srcs:

      if len(_spec) == 2:
         (_phase, val_spec), _io = _spec

         assert _phase in OP_PHASE.enum.elems.keys()
         phase = OP_PHASE.enum.elems[_phase].cname

         if _io in IO.enum.elems.keys():
            io = IO.enum.elems[_io].cname
         else:
            assert isinstance(_io, str)
            io = _io

         src_mappings.append(f'{{0}}->srcs.{src} = {{0}}->instrs[{phase}]->{val_spec};')
         src_mappings.append(f'{{0}}->instrs[{phase}]->{val_spec} = pco_ref_io({io});')
         src_mappings.append(f'pco_ref_xfer_mods(&{{0}}->instrs[{phase}]->{val_spec}, &{{0}}->srcs.{src}, true);')
      elif len(_spec) == 1:
         val = _spec[0]
         assert isinstance(val, str)
         src_mappings.append(f'{{0}}->srcs.{src} = {val};')
      else:
         assert False

   if bool(src_mappings):
      mapping_sets.append(src_mappings);

   # Emit iss mappings.
   iss_mappings = []
   for iss, _io in iss:
      if _io in IO.enum.elems.keys():
         io = IO.enum.elems[_io].cname
      else:
         assert isinstance(_io, str)
         io = _io

      iss_mappings.append(f'{{}}->iss.{iss} = pco_ref_io({io});')

   if bool(iss_mappings):
      mapping_sets.append(iss_mappings);

   # Emit destination mappings.
   dest_mappings = []
   for dest, (_phase, val_spec), _io in dests:
      assert _phase in OP_PHASE.enum.elems.keys()
      phase = OP_PHASE.enum.elems[_phase].cname

      if _io in IO.enum.elems.keys():
         io = IO.enum.elems[_io].cname
      else:
         assert isinstance(_io, str)
         io = _io

      dest_mappings.append(f'{{0}}->dests.{dest} = {{0}}->instrs[{phase}]->{val_spec};')
      dest_mappings.append(f'{{0}}->instrs[{phase}]->{val_spec} = pco_ref_io({io});')
      dest_mappings.append(f'pco_ref_xfer_mods(&{{0}}->instrs[{phase}]->{val_spec}, &{{0}}->dests.{dest}, true);')

   if bool(dest_mappings):
      mapping_sets.append(dest_mappings);

   # Emit variant mappings.
   variant_mappings = []
   for _phase, enc_op, *_ in enc_ops:
      assert _phase in OP_PHASE.enum.elems.keys()
      phase = OP_PHASE.enum.elems[_phase].cname
      variant_mappings.append(f'{{0}}->variant.instr[{phase}].{hdr_variant.bsname} = {enc_op.bname}_variant({{0}}->instrs[{phase}]);')

   assert bool(variant_mappings)
   mapping_sets.append(variant_mappings);

   name = op.bname
   cop_name = op.cname.upper()
   group_maps[name] = GroupMap(name, cop_name, mapping_sets)

# Encode mappings.
encode_map(O_FADD,
   encodings=[
      (I_FADD, [
         ('sat', OM_SAT),
         ('s0neg', (RM_NEG, 'src[0]')),
         ('s0abs', (RM_ABS, 'src[0]')),
         ('s1abs', (RM_ABS, 'src[1]')),
         ('s0flr', (RM_FLR, 'src[0]'))
      ])
   ]
)

encode_map(O_FMUL,
   encodings=[
      (I_FMUL, [
         ('sat', OM_SAT),
         ('s0neg', (RM_NEG, 'src[0]')),
         ('s0abs', (RM_ABS, 'src[0]')),
         ('s1abs', (RM_ABS, 'src[1]')),
         ('s0flr', (RM_FLR, 'src[0]'))
      ])
   ]
)

encode_map(O_FMAD,
   encodings=[
      (I_FMAD_EXT, [
         ('s0neg', (RM_NEG, 'src[0]')),
         ('s0abs', (RM_ABS, 'src[0]')),
         ('s2neg', (RM_NEG, 'src[2]')),
         ('sat', OM_SAT),

         ('lp', OM_LP),
         ('s1abs', (RM_ABS, 'src[1]')),
         ('s1neg', (RM_NEG, 'src[1]')),
         ('s2flr', (RM_FLR, 'src[2]')),
         ('s2abs', (RM_ABS, 'src[2]'))
      ]),
      (I_FMAD, [
         ('s0neg', (RM_NEG, 'src[0]')),
         ('s0abs', (RM_ABS, 'src[0]')),
         ('s2neg', (RM_NEG, 'src[2]')),
         ('sat', OM_SAT)
      ], [
         (OM_LP, '== false'),
         (RM_ABS, 'src[1]', '== false'),
         (RM_NEG, 'src[1]', '== false'),
         (RM_FLR, 'src[2]', '== false'),
         (RM_ABS, 'src[2]', '== false')
      ])
   ]
)

encode_map(O_FRCP,
   encodings=[
      (I_SNGL_EXT, [
         ('sngl_op', 'rcp'),
         ('s0neg', (RM_NEG, 'src[0]')),
         ('s0abs', (RM_ABS, 'src[0]'))
      ]),
      (I_SNGL, [('sngl_op', 'rcp')], [
         (RM_NEG, 'src[0]', '== false'),
         (RM_ABS, 'src[0]', '== false')
      ])
   ]
)

encode_map(O_MBYP,
   encodings=[
      (I_SNGL_EXT, [
         ('sngl_op', 'byp'),
         ('s0neg', (RM_NEG, 'src[0]')),
         ('s0abs', (RM_ABS, 'src[0]'))
      ]),
      (I_SNGL, [('sngl_op', 'byp')], [
         (RM_NEG, 'src[0]', '== false'),
         (RM_ABS, 'src[0]', '== false')
      ])
   ]
)

encode_map(O_PCK,
   encodings=[
      (I_PCK, [
         ('prog', False),
         ('rtz', OM_ROUNDZERO),
         ('scale', OM_SCALE),
         ('pck_format', OM_PCK_FMT)
      ])
   ]
)

encode_map(O_TST,
   encodings=[
      (I_TST_EXT, [
         ('tst_op', OM_TST_OP_MAIN),
         ('pwen', ('!pco_ref_is_null', 'dest[1]')),
         ('type', OM_TST_TYPE_MAIN),
         ('p2end', OM_PHASE2END),
         ('elem', (RM_ELEM, 'src[0]'))
      ]),
      (I_TST, [
         ('tst_op', OM_TST_OP_MAIN),
         ('pwen', ('!pco_ref_is_null', 'dest[1]')),
      ], [
         (OM_TST_OP_MAIN, '<= PCO_TST_OP_MAIN_NOTEQUAL'),
         (OM_TST_TYPE_MAIN, '== PCO_TST_TYPE_MAIN_F32'),
         (OM_PHASE2END, '== false'),
         (OM_TST_TYPE_MAIN, '== PCO_TST_TYPE_MAIN_F32'),
         (RM_ELEM, 'src[0]', '== 0'),
         (RM_ELEM, 'src[1]', '== 0'),
      ])
   ]
)

encode_map(O_MOVC,
   encodings=[
      (I_MOVC_EXT, [
         ('movw0', ('pco_ref_get_movw01', 'src[1]')),
         ('movw1', ('pco_ref_get_movw01', 'src[3]')),
         ('maskw0', (RM_ELEM, 'dest[0]')),
         ('aw', False),
         ('p2end', OM_PHASE2END)
      ]),
      (I_MOVC, [
         ('movw0', ('pco_ref_get_movw01', 'src[1]')),
         ('movw1', ('pco_ref_get_movw01', 'src[3]')),
      ], [
         (RM_ELEM, 'dest[0]', '== 0b1111'),
         (OM_PHASE2END, '== false'),
      ])
   ]
)

encode_map(O_UVSW_WRITE,
   encodings=[
      (I_UVSW_WRITE_IMM, [
         ('dsel', 'w0'),
         ('imm_addr', ('pco_ref_get_imm', 'src[1]'))
      ])
   ]
)

encode_map(O_UVSW_EMIT, encodings=[(I_UVSW_EMIT, [])])
encode_map(O_UVSW_ENDTASK, encodings=[(I_UVSW_ENDTASK, [])])
encode_map(O_UVSW_EMIT_ENDTASK, encodings=[(I_UVSW_EMIT_ENDTASK, [])])

encode_map(O_UVSW_WRITE_EMIT_ENDTASK,
   encodings=[
      (I_UVSW_WRITE_EMIT_ENDTASK_IMM, [
         ('dsel', 'w0'),
         ('imm_addr', ('pco_ref_get_imm', 'src[1]'))
      ])
   ]
)

encode_map(O_FITR,
   encodings=[
      (I_FITR, [
         ('p', False),
         ('drc', ('pco_ref_get_drc', 'src[0]')),
         ('iter_mode', OM_ITR_MODE),
         ('sat', OM_SAT),
         ('count', ('pco_ref_get_imm', 'src[2]'))
      ])
   ]
)

encode_map(O_FITRP,
   encodings=[
      (I_FITR, [
         ('p', True),
         ('drc', ('pco_ref_get_drc', 'src[0]')),
         ('iter_mode', OM_ITR_MODE),
         ('sat', OM_SAT),
         ('count', ('pco_ref_get_imm', 'src[3]'))
      ])
   ]
)

encode_map(O_BBYP0BM_IMM32,
   encodings=[
      (I_PHASE0_IMM32, [
         ('count_src', 's2'),
         ('count_op', 'byp'),
         ('shift1_op', 'byp'),
         ('imm32', ('pco_ref_get_imm', 'src[1]'))
      ])
   ]
)

encode_map(O_WOP, encodings=[(I_WOP, [])])
encode_map(O_WDF, encodings=[(I_WDF, [])])
encode_map(O_NOP, encodings=[(I_NOP, [])])

encode_map(O_DITR,
   encodings=[
      (I_DITR, [
         ('dest', ('pco_ref_get_temp', 'dest[0]')),

         ('coff', ('pco_ref_get_coeff', 'src[1]')),
         ('p', 'none'),

         ('woff', 0),
         ('mode', OM_ITR_MODE),

         ('count', ('pco_ref_get_imm', 'src[2]')),
         ('coff_idx_ctrl', ('pco_ref_get_reg_idx_ctrl', 'src[1]')),
         ('woff_idx_ctrl', 'none'),

         ('f16', OM_F16),
         ('sched_ctrl', OM_SCHED),
         ('drc', ('pco_ref_get_drc', 'src[0]')),
         ('sat', OM_SAT)
      ])
   ]
)

encode_map(O_DITRP,
   encodings=[
      (I_DITR, [
         ('dest', ('pco_ref_get_temp', 'dest[0]')),

         ('coff', ('pco_ref_get_coeff', 'src[1]')),
         ('p', 'iter_mul'),

         ('woff', ('pco_ref_get_coeff', 'src[2]')),
         ('mode', OM_ITR_MODE),

         ('count', ('pco_ref_get_imm', 'src[3]')),
         ('coff_idx_ctrl', ('pco_ref_get_reg_idx_ctrl', 'src[1]')),
         ('woff_idx_ctrl', ('pco_ref_get_reg_idx_ctrl', 'src[2]')),

         ('f16', OM_F16),
         ('sched_ctrl', OM_SCHED),
         ('drc', ('pco_ref_get_drc', 'src[0]')),
         ('sat', OM_SAT)
      ])
   ]
)

# Group mappings.
group_map(O_FADD,
   hdr=(I_IGRP_HDR_MAIN, [
      ('oporg', 'p0'),
      ('olchk', OM_OLCHK),
      ('w1p', False),
      ('w0p', True),
      ('cc', OM_EXEC_CND),
      ('end', OM_END),
      ('atom', OM_ATOM),
      ('rpt', OM_RPT)
   ]),
   enc_ops=[('0', O_FADD)],
   srcs=[
      ('s[0]', ('0', 'src[0]'), 's0'),
      ('s[1]', ('0', 'src[1]'), 's1')
   ],
   iss=[('is[4]', 'ft0')],
   dests=[('w[0]', ('0', 'dest[0]'), 'ft0')]
)

group_map(O_FMUL,
   hdr=(I_IGRP_HDR_MAIN, [
      ('oporg', 'p0'),
      ('olchk', OM_OLCHK),
      ('w1p', False),
      ('w0p', True),
      ('cc', OM_EXEC_CND),
      ('end', OM_END),
      ('atom', OM_ATOM),
      ('rpt', OM_RPT)
   ]),
   enc_ops=[('0', O_FMUL)],
   srcs=[
      ('s[0]', ('0', 'src[0]'), 's0'),
      ('s[1]', ('0', 'src[1]'), 's1')
   ],
   iss=[('is[4]', 'ft0')],
   dests=[('w[0]', ('0', 'dest[0]'), 'ft0')]
)

group_map(O_FMAD,
   hdr=(I_IGRP_HDR_MAIN, [
      ('oporg', 'p0'),
      ('olchk', OM_OLCHK),
      ('w1p', False),
      ('w0p', True),
      ('cc', OM_EXEC_CND),
      ('end', OM_END),
      ('atom', OM_ATOM),
      ('rpt', OM_RPT)
   ]),
   enc_ops=[('0', O_FMAD)],
   srcs=[
      ('s[0]', ('0', 'src[0]'), 's0'),
      ('s[1]', ('0', 'src[1]'), 's1'),
      ('s[2]', ('0', 'src[2]'), 's2')
   ],
   iss=[('is[4]', 'ft0')],
   dests=[('w[0]', ('0', 'dest[0]'), 'ft0')]
)

group_map(O_FRCP,
   hdr=(I_IGRP_HDR_MAIN, [
      ('oporg', 'p0'),
      ('olchk', OM_OLCHK),
      ('w1p', False),
      ('w0p', True),
      ('cc', OM_EXEC_CND),
      ('end', OM_END),
      ('atom', OM_ATOM),
      ('rpt', OM_RPT)
   ]),
   enc_ops=[('0', O_FRCP)],
   srcs=[('s[0]', ('0', 'src[0]'), 's0')],
   iss=[('is[4]', 'ft0')],
   dests=[('w[0]', ('0', 'dest[0]'), 'ft0')]
)

group_map(O_MBYP,
   hdr=(I_IGRP_HDR_MAIN, [
      ('oporg', 'p0'),
      ('olchk', OM_OLCHK),
      ('w1p', False),
      ('w0p', True),
      ('cc', OM_EXEC_CND),
      ('end', OM_END),
      ('atom', OM_ATOM),
      ('rpt', OM_RPT)
   ]),
   enc_ops=[('0', O_MBYP)],
   srcs=[('s[0]', ('0', 'src[0]'), 's0')],
   iss=[('is[4]', 'ft0')],
   dests=[('w[0]', ('0', 'dest[0]'), 'ft0')]
)

group_map(O_PCK,
   hdr=(I_IGRP_HDR_MAIN, [
      ('oporg', 'p2'),
      ('olchk', OM_OLCHK),
      ('w1p', False),
      ('w0p', True),
      ('cc', OM_EXEC_CND),
      ('end', OM_END),
      ('atom', OM_ATOM),
      ('rpt', OM_RPT)
   ]),
   enc_ops=[('2_pck', O_PCK)],
   srcs=[('s[0]', ('2_pck', 'src[0]'), 'is3')],
   iss=[
      ('is[0]', 's0'),
      ('is[3]', 'fte'),
      ('is[4]', 'ft2')
   ],
   dests=[('w[0]', ('2_pck', 'dest[0]'), 'ft2')]
)

group_map(O_SCMP,
   hdr=(I_IGRP_HDR_MAIN, [
      ('oporg', 'p0_p1_p2'),
      ('olchk', OM_OLCHK),
      ('w1p', False),
      ('w0p', True),
      ('cc', OM_EXEC_CND),
      ('end', OM_END),
      ('atom', OM_ATOM),
      ('rpt', OM_RPT)
   ]),
   enc_ops=[
      ('0', O_MBYP, ['ft0'], ['src[0]']),
      ('1', O_MBYP, ['ft1'], ['src[1]']),
      ('2_tst', O_TST, ['ftt', '_'], ['is1', 'is2'], [(OM_TST_OP_MAIN, OM_TST_OP_MAIN), (OM_TST_TYPE_MAIN, 'f32')]),
      ('2_pck', O_PCK, ['ft2'], ['_'], [(OM_PCK_FMT, 'one')]),
      ('2_mov', O_MOVC, ['dest[0]', '_'], ['ftt', 'ft2', 'is4', '_', '_'])
   ],
   srcs=[
      ('s[0]', ('0', 'src[0]'), 's0'),
      ('s[1]', 'pco_zero'),
      ('s[3]', ('1', 'src[0]'), 's3'),
   ],
   iss=[
      ('is[0]', 's1'),
      ('is[1]', 'ft0'),
      ('is[2]', 'ft1'),
      ('is[4]', 'fte'),
   ],
   dests=[
      ('w[0]', ('2_mov', 'dest[0]'), 'w0'),
   ]
)

group_map(O_UVSW_WRITE,
   hdr=(I_IGRP_HDR_MAIN, [
      ('oporg', 'be'),
      ('olchk', False),
      ('w1p', False),
      ('w0p', False),
      ('cc', OM_EXEC_CND),
      ('end', False),
      ('atom', False),
      ('rpt', OM_RPT)
   ]),
   enc_ops=[('backend', O_UVSW_WRITE)],
   srcs=[('s[0]', ('backend', 'src[0]'), 'w0')],
   iss=[
      ('is[0]', 's0'),
      ('is[4]', 'fte')
   ]
)

group_map(O_UVSW_EMIT,
   hdr=(I_IGRP_HDR_MAIN, [
      ('oporg', 'be'),
      ('olchk', False),
      ('w1p', False),
      ('w0p', False),
      ('cc', OM_EXEC_CND),
      ('end', False),
      ('atom', False),
      ('rpt', 1)
   ]),
   enc_ops=[('backend', O_UVSW_EMIT)]
)

group_map(O_UVSW_ENDTASK,
   hdr=(I_IGRP_HDR_MAIN, [
      ('oporg', 'be'),
      ('olchk', False),
      ('w1p', False),
      ('w0p', False),
      ('cc', 'e1_zx'),
      ('end', OM_END),
      ('atom', False),
      ('rpt', 1)
   ]),
   enc_ops=[('backend', O_UVSW_ENDTASK)]
)

group_map(O_UVSW_EMIT_ENDTASK,
   hdr=(I_IGRP_HDR_MAIN, [
      ('oporg', 'be'),
      ('olchk', False),
      ('w1p', False),
      ('w0p', False),
      ('cc', 'e1_zx'),
      ('end', OM_END),
      ('atom', False),
      ('rpt', 1)
   ]),
   enc_ops=[('backend', O_UVSW_EMIT_ENDTASK)]
)

group_map(O_UVSW_WRITE_EMIT_ENDTASK,
   hdr=(I_IGRP_HDR_MAIN, [
      ('oporg', 'be'),
      ('olchk', False),
      ('w1p', False),
      ('w0p', False),
      ('cc', 'e1_zx'),
      ('end', OM_END),
      ('atom', False),
      ('rpt', 1)
   ]),
   enc_ops=[('backend', O_UVSW_WRITE_EMIT_ENDTASK)],
   srcs=[('s[0]', ('backend', 'src[0]'), 'w0')],
   iss=[
      ('is[0]', 's0'),
      ('is[4]', 'fte')
   ]
)

group_map(O_FITR,
   hdr=(I_IGRP_HDR_MAIN, [
      ('oporg', 'be'),
      ('olchk', OM_OLCHK),
      ('w1p', False),
      ('w0p', False),
      ('cc', OM_EXEC_CND),
      ('end', OM_END),
      ('atom', OM_ATOM),
      ('rpt', OM_RPT)
   ]),
   enc_ops=[('backend', O_FITR)],
   srcs=[
      ('s[0]', ('backend', 'src[1]'), 's0'),
      ('s[3]', ('backend', 'dest[0]'), 's3')
   ]
)

group_map(O_FITRP,
   hdr=(I_IGRP_HDR_MAIN, [
      ('oporg', 'be'),
      ('olchk', OM_OLCHK),
      ('w1p', False),
      ('w0p', False),
      ('cc', OM_EXEC_CND),
      ('end', OM_END),
      ('atom', OM_ATOM),
      ('rpt', OM_RPT)
   ]),
   enc_ops=[('backend', O_FITRP)],
   srcs=[
      ('s[0]', ('backend', 'src[1]'), 's0'),
      ('s[2]', ('backend', 'src[2]'), 's2'),
      ('s[3]', ('backend', 'dest[0]'), 's3')
   ]
)

group_map(O_MOVI32,
   hdr=(I_IGRP_HDR_BITWISE, [
      ('opcnt', 'p0'),
      ('olchk', OM_OLCHK),
      ('w1p', False),
      ('w0p', True),
      ('cc', OM_EXEC_CND),
      ('end', OM_END),
      ('atom', OM_ATOM),
      ('rpt', OM_RPT)
   ]),
   enc_ops=[('0', O_BBYP0BM_IMM32, ['ft0', 'dest[0]'], ['s0', 'src[0]'])],
   dests=[('w[0]', ('0', 'dest[1]'), 'ft1')]
)

group_map(O_WOP,
   hdr=(I_IGRP_HDR_CONTROL, [
      ('olchk', False),
      ('w1p', False),
      ('w0p', False),
      ('cc', 'e1_zx'),
      ('miscctl', 0),
      ('ctrlop', 'wop')
   ]),
   enc_ops=[('ctrl', O_WOP),]
)

group_map(O_WDF,
   hdr=(I_IGRP_HDR_CONTROL, [
      ('olchk', False),
      ('w1p', False),
      ('w0p', False),
      ('cc', 'e1_zx'),
      ('miscctl', ('pco_ref_get_drc', 'src[0]')),
      ('ctrlop', 'wdf')
   ]),
   enc_ops=[('ctrl', O_WDF)]
)

group_map(O_NOP,
   hdr=(I_IGRP_HDR_CONTROL, [
      ('olchk', False),
      ('w1p', False),
      ('w0p', False),
      ('cc', OM_EXEC_CND),
      ('miscctl', OM_END),
      ('ctrlop', 'nop')
   ]),
   enc_ops=[('ctrl', O_NOP)]
)

group_map(O_DITR,
   hdr=(I_IGRP_HDR_CONTROL, [
      ('olchk', False),
      ('w1p', False),
      ('w0p', False),
      ('cc', OM_EXEC_CND),
      ('miscctl', False),
      ('ctrlop', 'ditr')
   ]),
   enc_ops=[('ctrl', O_DITR)]
)

group_map(O_DITRP,
   hdr=(I_IGRP_HDR_CONTROL, [
      ('olchk', False),
      ('w1p', False),
      ('w0p', False),
      ('cc', OM_EXEC_CND),
      ('miscctl', False),
      ('ctrlop', 'ditr')
   ]),
   enc_ops=[('ctrl', O_DITRP)]
)
