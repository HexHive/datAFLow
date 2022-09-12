#!/usr/bin/env python3

"""
Calculate a target's DDG ratio.

Author: Adrian Herrera
"""


from argparse import ArgumentParser
from subprocess import run, PIPE
import os
import re
import sys


DDG_RE = re.compile(b'''DDG - Instrumented (\d+) locations over a total of (\d+)''')


def main(args):
    """The main function."""
    if len(args) <= 1:
        print(f'usage: {args[0]} ...')
        return 1

    env = os.environ.copy()
    env['DDG_INSTR'] = '1'
    env['AFL_LLVM_INSTRUMENT'] = 'classic'

    proc = run(args[1:], stderr=PIPE, check=False, env=env)

    num_insts = []
    num_bbs = []
    for line in proc.stderr.split(b'\n'):
        print(line.decode('utf-8'), file=sys.stderr)
        match = DDG_RE.search(line)
        if match:
            num_insts.append(int(match[1]))
            num_bbs.append(int(match[2]))

    sys.stderr.flush()
    ddg_ratio = sum(num_insts) / sum(num_bbs)
    print(f'\n==== DDG ratio: {ddg_ratio:.2%} ====')

    return proc.returncode


if __name__ == '__main__':
    sys.exit(main(sys.argv))
