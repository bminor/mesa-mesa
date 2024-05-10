# Copyright © 2024 Imagination Technologies Ltd.
# SPDX-License-Identifier: MIT

from enum import Enum, auto

_ = None

prefix = 'pco'

class BaseType(Enum):
   bool = auto()
   uint = auto()
   enum = auto()

class Type(object):
   def __init__(self, name, base_type, num_bits, dec_bits, check, encode, nzdefault, print_early):
      self.name = name
      self.base_type = base_type
      self.num_bits = num_bits
      self.dec_bits = dec_bits
      self.check = check
      self.encode = encode
      self.nzdefault = nzdefault
      self.print_early = print_early

types = {}

def type(name, base_type, num_bits=None, dec_bits=None, check=None, encode=None, nzdefault=None, print_early=False):
   assert name not in types.keys(), f'Duplicate type "{name}".'
   t = Type(name, base_type, num_bits, dec_bits, check, encode, nzdefault, print_early)
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
def val_fits_in_bits(val, num_bits):
   return val < pow(2, num_bits)

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
   enums[name] = EnumType(_name, _elems, _valid, _unique_count, is_bitset, parent=None)

   return type(name, BaseType.enum, num_bits, *args, **kwargs)

def enum_subtype(name, parent, num_bits):
   assert name not in enums.keys(), f'Duplicate enum "{name}".'

   assert parent.name in enums.keys()
   enum_parent = enums[parent.name]
   assert not enum_parent.is_bitset
   assert parent.num_bits is not None and parent.num_bits > num_bits

   _name = f'{prefix}_{name}'
   enums[name] = EnumType(_name, None, None, None, False, enum_parent)
   return type(name, BaseType.enum, num_bits)

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
