#!/usr/bin/env python3

"""
Timestamp an Angora queue.

Author: Adrian Herrera
"""


from argparse import ArgumentParser
from pathlib import Path
import csv


def parse_args():
    """Parse command-line arguments."""
    parser = ArgumentParser(description='Retrieve Angora queue timestamps')
    parser.add_argument('-o', '--out', type=Path, metavar='CSV', required=True,
                        help='Output CSV')
    parser.add_argument('queue', type=Path, metavar='DIR',
                        help='Angora queue directory')
    return parser.parse_args()


def main():
    """The main function."""
    args = parse_args()

    timestamps = []
    for testcase in args.queue.glob('id:*'):
        timestamps.append((testcase.name, testcase.stat().st_ctime))

    with args.out.open('w') as outf:
        writer = csv.writer(outf)
        for testcase, timestamp in timestamps:
            writer.writerow((testcase, timestamp))


if __name__ == '__main__':
    main()
