#! /usr/bin/env python3
#
# Copyright © 2024 Collabora Ltd. and Red Hat Inc.
# SPDX-License-Identifier: MIT

import argparse
import os.path
import re
import sys

from collections import namedtuple
from mako.template import Template

import util


TEMPLATE_RS = Template("""\
// Copyright © 2024 Collabora Ltd. and Red Hat Inc.
// SPDX-License-Identifier: MIT

// This file is generated by struct_parser.py. DO NOT EDIT!

#![allow(non_snake_case)]

use std::ops::Range;

% for s in structs:
    % for f in s.fields:
        % if f.stride:
#[inline]
pub const fn ${s.name}_${f.name}(i: usize) -> Range<usize> {
    (i * ${f.stride} + ${f.lo})..(i * ${f.stride} + ${f.hi + 1})
}
        % else:
pub const ${s.name}_${f.name}: Range<usize> = ${f.lo}..${f.hi + 1};
        % endif:
        % for e in f.enums:
pub const ${s.name}_${f.name}_${e.name}: u32 = ${e.value};
        % endfor
    % endfor
% endfor
""")

STRUCTS = [
    'SPHV3',
    'SPHV4',
    'TEXHEADV2',
    'TEXHEADV3',
    'TEXHEAD_BL',
    'TEXHEAD_1D',
    'TEXHEAD_PITCH',
    # This one goes last because it's a substring of the others
    'TEXHEAD',
    'TEXSAMP',
    'QMDV00_06',
    'QMDV01_06',
    'QMDV01_07',
    'QMDV02_01',
    'QMDV02_02',
    'QMDV02_03',
    'QMDV02_04',
    'QMDV03_00',
    'QMDV04_00',
    'QMDV05_00',
]

Enum = namedtuple('Enum', ['name', 'value'])

class Field(object):
    def __init__(self, name, lo, hi, stride=0):
        self.name = name
        self.lo = lo
        self.hi = hi
        self.stride = stride
        self.enums = []

    def add_enum(self, name, value):
        self.enums.append(Enum(name, value))

class Struct(object):
    def __init__(self, name):
        self.name = name
        self.fields = []

    def add_field(self, name, lo, hi, stride=0):
        self.fields.append(Field(name, lo, hi, stride))

DRF_RE = re.compile(r'(?P<hi>[0-9]+):(?P<lo>[0-9]+)')
FIELD_NAME_RE = re.compile(r'_?(?P<dw>[0-9]+)?_?(?P<name>.*)')
MW_RE = re.compile(r'MW\((?P<hi>[0-9]+):(?P<lo>[0-9]+)\)')
MW_ARR_RE = re.compile(r'MW\(\((?P<hi>\d+)\+\(i\)\*(?P<stride>\d+)\):\((?P<lo>[0-9]+)\+\(i\)\*(?P=stride)\)\)')

def parse_header(nvcl, file):
    structs = {}
    for line in file:
        line = line.strip().split()
        if not line:
            continue

        if line[0] != '#define':
            continue

        if not line[1].startswith(nvcl):
            continue

        name = line[1][(len(nvcl)+1):]

        struct = None
        for s in STRUCTS:
            if name.startswith(s):
                if s not in structs:
                    structs[s] = Struct(s)
                struct = structs[s]
                name = name[len(s):]
                break

        if struct is None:
            continue

        name_m = FIELD_NAME_RE.match(name)
        name = name_m.group('name')

        drf = DRF_RE.match(line[2])
        mw = MW_RE.match(line[2])
        mw_arr = MW_ARR_RE.match(line[2])
        if drf:
            dw = int(name_m.group('dw'))
            lo = int(drf.group('lo')) + dw * 32
            hi = int(drf.group('hi')) + dw * 32
            struct.add_field(name, lo, hi)
        elif mw:
            lo = int(mw.group('lo'))
            hi = int(mw.group('hi'))
            struct.add_field(name, lo, hi)
        elif mw_arr:
            lo = int(mw_arr.group('lo'))
            hi = int(mw_arr.group('hi'))
            stride = int(mw_arr.group('stride'))
            assert name.endswith('(i)')
            struct.add_field(name.removesuffix('(i)'), lo, hi, stride)
        else:
            for f in struct.fields:
                if name.startswith(f.name + '_'):
                    name = name[(len(f.name)+1):]
                    f.add_enum(name, line[2])

    return list(structs.values())

NVCL_RE = re.compile(r'cl(?P<clsver>[0-9a-f]{4}).*')

def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--out-rs', required=True, help='Output Rust file.')
    parser.add_argument('--in-h',
                        help='Input class header file.',
                        required=True)
    args = parser.parse_args()

    clheader = os.path.basename(args.in_h)
    nvcl = NVCL_RE.match(clheader).group('clsver')
    nvcl = nvcl.upper()
    nvcl = "NV" + nvcl

    with open(args.in_h, 'r', encoding='utf-8') as f:
        structs = parse_header(nvcl, f)

    util.write_template_rs(args.out_rs, TEMPLATE_RS, dict(structs=structs))


if __name__ == '__main__':
    main()
