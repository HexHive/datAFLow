#!/usr/bin/env python
#
# Remove duplicate lines from a text file
#

from __future__ import print_function

from argparse import ArgumentParser
import os
import sys


def parse_args():
    parser = ArgumentParser(description='Remove duplicate lines from a text '
                                        'file')
    parser.add_argument('file', help='Path to text file')

    return parser.parse_args()


def main():
    args = parse_args()

    path = args.file
    if not os.path.isfile(path):
        raise Exception('%s is not a valid file path' % path)

    seen_lines = set()

    with open(path, 'r') as f:
        lines = f.readlines()
        for line in lines:
            if line not in seen_lines:
                print(line, end='')
                seen_lines.add(line)


if __name__ == '__main__':
    sys.exit(main())
