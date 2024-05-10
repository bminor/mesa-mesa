# Copyright © 2024 Imagination Technologies Ltd.
# SPDX-License-Identifier: MIT

from enum import Enum, auto

_ = None

prefix = 'pco'

def val_fits_in_bits(val, num_bits):
   return val < pow(2, num_bits)

class BaseType(Enum):
   bool = auto()
   uint = auto()
   enum = auto()

class Type(object):
   def __init__(self, name, base_type, num_bits, dec_bits, check, encode, nzdefault, print_early, enum):
      self.name = name
      self.base_type = base_type
      self.num_bits = num_bits
      self.dec_bits = dec_bits
      self.check = check
      self.encode = encode
      self.nzdefault = nzdefault
      self.print_early = print_early
      self.enum = enum

types = {}

def type(name, base_type, num_bits=None, dec_bits=None, check=None, encode=None, nzdefault=None, print_early=False, enum=None):
   assert name not in types.keys(), f'Duplicate type "{name}".'

   if base_type == BaseType.bool:
      _name = 'bool'
   elif base_type == BaseType.uint:
      _name = 'unsigned'
   elif base_type == BaseType.enum:
      _name = f'enum {prefix}_{name}'
   else:
      assert False, f'Invalid base type for type {name}.'

   t = Type(_name, base_type, num_bits, dec_bits, check, encode, nzdefault, print_early, enum)
   types[name] = t
   return t

# Enum types.
class EnumType(object):
   def __init__(self, name, elems, valid, unique_count, is_bitset, parent):
      self.name = name
      self.elems = elems
      self.valid = valid
      self.unique_count = unique_count
      self.is_bitset = is_bitset
      self.parent = parent

enums = {}
def enum_type(name, elems, is_bitset=False, num_bits=None, *args, **kwargs):
   assert name not in enums.keys(), f'Duplicate enum "{name}".'

   _elems = {}
   _valid_vals = set()
   _valid_valmask = 0
   next_value = 0
   for e in elems:
      if isinstance(e, str):
         elem = e
         value = (1 << next_value) if is_bitset else next_value
         next_value += 1
         string = elem
      else:
         assert isinstance(e, tuple) and len(e) > 1
         elem = e[0]
         if isinstance(e[1], str) and len(e) == 2:
            value = (1 << next_value) if is_bitset else next_value
            next_value += 1
            string = e[1]
         elif isinstance(e[1], int):
            value = e[1]
            string = e[2] if len(e) == 3 else elem
         else:
            assert False, f'Invalid defintion for element "{elem}" in elem "{name}".'

      assert isinstance(elem, str) and isinstance(value, int) and isinstance(string, str)

      assert not num_bits or val_fits_in_bits(value, num_bits), f'Element "{elem}" in elem "{name}" with value "{value}" does not fit into {num_bits} bits.'
      if is_bitset:
         _valid_valmask |= value
      else:
         _valid_vals.add(value)

      assert elem not in _elems.keys(), f'Duplicate element "{elem}" in enum "".'
      cname = f'{prefix}_{name}_{elem}'.upper()
      _elems[elem] = (cname, value, string)

   _name = f'{prefix}_{name}'
   _valid = _valid_valmask if is_bitset else _valid_vals
   _unique_count = bin(_valid_valmask).count('1') if is_bitset else len(_valid_vals)
   enum = EnumType(_name, _elems, _valid, _unique_count, is_bitset, parent=None)
   enums[name] = enum

   return type(name, BaseType.enum, num_bits, *args, **kwargs, enum=enum)

def enum_subtype(name, parent, num_bits):
   assert name not in enums.keys(), f'Duplicate enum "{name}".'

   assert parent.enum is not None
   assert not parent.enum.is_bitset
   assert parent.num_bits is not None and parent.num_bits > num_bits

   _name = f'{prefix}_{name}'
   enum = EnumType(_name, None, None, None, False, parent)
   enums[name] = enum
   return type(name, BaseType.enum, num_bits, enum=enum)

# Type specializations.

field_types = {}
field_enum_types = {}
def field_type(name, *args, **kwargs):
   assert name not in field_types.keys(), f'Duplicate field type "{name}".'
   t = type(name, *args, **kwargs)
   field_types[name] = t
   assert len(field_types) <= 64, 'Too many field types!'
   return t

def field_enum_type(name, *args, **kwargs):
   assert name not in field_types.keys() and name not in field_enum_types.keys(), f'Duplicate field enum type "{name}".'
   t = enum_type(name, *args, **kwargs)
   field_types[name] = t
   field_enum_types[name] = enums[name]
   assert len(field_types) <= 64, 'Too many field types!'
   return t

def field_enum_subtype(name, *args, **kwargs):
   assert name not in field_types.keys() and name not in field_enum_types.keys(), f'Duplicate field enum (sub)type "{name}".'
   t = enum_subtype(name, *args, **kwargs)
   field_types[name] = t
   field_enum_types[name] = enums[name]
   assert len(field_types) <= 64, 'Too many field types!'
   return t

op_mods = {}
op_mod_enums = {}
def op_mod(name, *args, **kwargs):
   assert name not in op_mods.keys(), f'Duplicate op mod "{name}".'
   t = type(name, *args, **kwargs)
   op_mods[name] = t
   assert len(op_mods) <= 64, 'Too many op mods!'
   return t

def op_mod_enum(name, *args, **kwargs):
   assert name not in op_mods.keys() and name not in op_mod_enums.keys(), f'Duplicate op mod enum "{name}".'
   t = enum_type(name, *args, **kwargs)
   op_mods[name] = t
   op_mod_enums[name] = enums[name]
   assert len(op_mods) <= 64, 'Too many op mods!'
   return t

ref_mods = {}
ref_mod_enums = {}
def ref_mod(name, *args, **kwargs):
   assert name not in ref_mods.keys(), f'Duplicate ref mod "{name}".'
   t = type(name, *args, **kwargs)
   ref_mods[name] = t
   assert len(ref_mods) <= 64, 'Too many ref mods!'
   return t

def ref_mod_enum(name, *args, **kwargs):
   assert name not in ref_mods.keys() and name not in ref_mod_enums.keys(), f'Duplicate ref mod enum "{name}".'
   t = enum_type(name, *args, **kwargs)
   ref_mods[name] = t
   ref_mod_enums[name] = enums[name]
   assert len(ref_mods) <= 64, 'Too many ref mods!'
   return t

# Bit encoding definition helpers.

class BitPiece(object):
   def __init__(self, name, byte, hi_bit, lo_bit, num_bits):
      self.name = name
      self.byte = byte
      self.hi_bit = hi_bit
      self.lo_bit = lo_bit
      self.num_bits = num_bits

def bit_piece(name, byte, bit_range):
   assert bit_range.count(':') <= 1, f'Invalid bit range specification in bit piece {name}.'
   is_one_bit = not bit_range.count(':')

   split_range = [bit_range, bit_range] if is_one_bit else bit_range.split(':', 1)
   (hi_bit, lo_bit) = list(map(int, split_range))
   assert hi_bit < 8 and hi_bit >= 0 and lo_bit < 8 and lo_bit >= 0 and hi_bit >= lo_bit

   _num_bits = hi_bit - lo_bit + 1
   return BitPiece(name, byte, hi_bit, lo_bit, _num_bits)

class BitField(object):
   def __init__(self, name, field_type, pieces, reserved):
      self.name = name
      self.field_type = field_type
      self.pieces = pieces
      self.reserved = reserved

def bit_field(name, bit_set_pieces, field_type, pieces, reserved=None):
   _pieces = [bit_set_pieces[p] for p in pieces]

   total_bits = sum([p.num_bits for p in _pieces])
   assert total_bits == field_type.num_bits, f'Expected {field_type.num_bits}, got {total_bits} in bit field {name}.'

   if reserved is not None:
      assert val_fits_in_bits(reserved, total_bits), f'Reserved value for bit field {name} is too large.'

   return BitField(name, field_type, _pieces, reserved)

class BitSet(object):
   def __init__(self, name, pieces, fields):
      self.name = name
      self.pieces = pieces
      self.fields = fields
      self.bit_structs = {}
      self.variants = []

bit_sets = {}

def bit_set(name, pieces, fields):
   assert name not in bit_sets.keys(), f'Duplicate bit set "{name}".'

   _pieces = {}
   for (piece, spec) in pieces:
      assert piece not in _pieces.keys(), f'Duplicate bit piece "{n}" in bit set "{name}".'
      _pieces[piece] = bit_piece(piece, *spec)

   _fields = {}
   for (field, spec) in fields:
      assert field not in _fields.keys(), f'Duplicate bit field "{n}" in bit set "{name}".'
      _fields[field] = bit_field(field, _pieces, *spec)

   _name = f'{prefix}_{name}'
   bs = BitSet(_name, _pieces, _fields)
   bit_sets[name] = bs
   return bs

class BitStruct(object):
   def __init__(self, name, struct_fields, num_bytes):
      self.name = name
      self.struct_fields = struct_fields
      self.num_bytes = num_bytes

def bit_struct(name, bit_set, field_mappings):
   assert name not in bit_set.bit_structs.keys(), f'Duplicate bit struct "{name}" in bit set "{bit_set.name}".'

   struct_fields = {}
   all_pieces = []
   total_bits = 0
   for mapping in field_mappings:
      if isinstance(mapping, str):
         struct_field = mapping
         _field = mapping
         fixed_value = None
      else:
         assert isinstance(mapping, tuple)
         struct_field, _field, *fixed_value = mapping
         assert len(fixed_value) == 0 or len(fixed_value) == 1
         fixed_value = None if len(fixed_value) == 0 else fixed_value[0]

      assert struct_field not in struct_fields.keys(), f'Duplicate struct field "{struct_field}" in bit struct "{name}".'
      assert _field in bit_set.fields.keys(), f'Field "{_field}" in mapping for struct field "{name}.{struct_field}" not defined in bit set "{bit_set.name}".'
      field = bit_set.fields[_field]
      field_type = field.field_type
      is_enum = field_type.base_type == BaseType.enum

      if fixed_value is not None:
         assert field.reserved is None, f'Fixed value for field mapping "{struct_field}" using field "{_field}" cannot overwrite its reserved value.'

         if is_enum and isinstance(fixed_value, str):
            enum = field_type.enum
            assert fixed_value in enum.elems.keys(), f'Fixed value for field mapping "{struct_field}" using field "{_field}" is not an element of enum {field_type.name}.'
            fixed_value = f'{prefix}_{field_type.name}_{fixed_value}'.upper()
         else:
            if isinstance(fixed_value, bool):
               fixed_value = int(fixed_value)

            assert isinstance(fixed_value, int)
            assert val_fits_in_bits(fixed_value, field_type.num_bits), f'Fixed value for field mapping "{struct_field}" using field "{_field}" is too large.'

      all_pieces.extend([(piece.lo_bit + (8 * piece.byte), piece.hi_bit + (8 * piece.byte), piece.name) for piece in field.pieces])
      total_bits += field_type.num_bits

      if field.reserved is None and fixed_value is None:
         # Use parent enum for struct fields.
         if is_enum and field_type.enum.parent is not None:
            field_type = field_type.enum.parent

         struct_field_bits = field_type.dec_bits if field_type.dec_bits is not None else field_type.num_bits
         struct_fields[struct_field] = (field_type.name, struct_field, struct_field_bits)

   # Check for overlapping pieces.
   for p0 in all_pieces:
      for p1 in all_pieces:
         if p0 == p1:
            continue
         assert p0[1] < p1[0] or p0[0] > p1[1], f'Pieces "{p0[2]}" and "{p1[2]}" overlap in bit struct "{name}".'

   # Check for byte-alignment.
   assert (total_bits % 8) == 0, f'Bit struct "{name}" has a non-byte-aligned number of bits ({total_bits}).'

   _name = f'{bit_set.name}_{name}'
   bs = BitStruct(_name, struct_fields, total_bits // 8)
   bit_set.bit_structs[name] = bs
   bit_set.variants.append(f'{bit_set.name}_{name}'.upper())

   return bs
