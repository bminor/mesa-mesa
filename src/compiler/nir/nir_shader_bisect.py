#!/usr/bin/env python3

import argparse
import subprocess
import os
import re


def run(args, lo, hi):
    print(f'NIR_SHADER_BISECT_LO: {lo}')
    print(f'NIR_SHADER_BISECT_HI: {hi}')

    env = os.environ.copy()
    env['MESA_SHADER_CACHE_DISABLE'] = '1'
    env['NIR_SHADER_BISECT_LO'] = lo
    env['NIR_SHADER_BISECT_HI'] = hi
    env.pop('MESA_LOG_FILE', None)

    cmd = [args.cmd] + args.cmd_args
    if args.debug:
        print(f"running: {cmd}")
    result = subprocess.run(cmd, env=env,
                            text=True, capture_output=True)
    if args.debug:
        print(f"Result: {result.returncode}")
        print("stdout:")
        print(result.stdout)
        print("stderr:")
        print(result.stderr)

    shaders = set(re.findall(
        "NIR bisect selected source_blake3: (.*) \((.*)\)", result.stderr))
    num = len(shaders)
    print(f'Shaders matched: {num}')
    if num <= 5:
        for blake3, id in sorted(shaders):
            print(f'  {blake3}')
    return shaders


def was_good():
    while True:
        response = input('Was the previous run [g]ood or [b]ad? ')

        if response in ('g', 'b'):
            return response == 'g'


def bisect(args):
    lo = 0
    hi = (1 << (8 * 32)) - 1
    lo = f'{lo:064x}'
    hi = f'{hi:064x}'

    # Do an initial run to sanity check that the user has actually made
    # nir_shader_bisect_select() select some broken behavior.
    bad = run(args, lo, hi)
    if was_good():
        print("Entire hash range produced a good result -- did you make nir_shader_bisect_select() select the behavior you want?")
        exit(1)

    if len(bad) == 0:
        print("No bad shaders detected -- did you rebuild after adding nir_shader_bisect_select()?")
        exit(1)

    while True:
        if len(bad) == 1:
            for shader in bad:
                print(f"Bisected to source_blake3 {shader}")
                print(
                    f"You can now replace nir_shader_bisect_select() with _mesa_printed_blake3_equal(s->info.source_blake3, (uint32_t[]){shader})")
            exit(0)
        else:
            num = len(bad)
            print(f"Shaders remaining to bisect: {num}")
            if num <= 5:
                for shader in sorted(bad):
                    print(f'  {shader}')

        # Find the middle shader remaining in the set of shaders from the last
        # bad run, and check if the bottom half of set of possibly-bad shaders
        # (up to and including it) is bad.
        ids = sorted([id for blake3, id in bad])
        lo = ids[0]
        split = ids[(len(ids) - 1) // 2]
        cur = run(args, lo, split)

        if was_good():
            bad = bad.difference(cur)
        else:
            bad = cur


if __name__ == '__main__':
    parser = argparse.ArgumentParser()

    parser.add_argument('-d', '--debug', action='store_true')
    parser.add_argument('cmd')
    parser.add_argument('cmd_args', nargs=argparse.REMAINDER)
    args = parser.parse_args()
    bisect(args)
