#!/usr/bin/env python3

"""
Extract a binary's library dependencies and copy them to a given location.
"""

from argparse import ArgumentParser
import os
import shutil

from common import get_binary_info, get_library_deps


def parse_args():
    """Parse command-line arguments."""
    parser = ArgumentParser(description='Extract a binary\'s library '
                                        'dependencies')
    parser.add_argument('--out-dir', required=True,
                        help='Output directory for libraries')
    parser.add_argument('binary', help='The ELF binary')

    return parser.parse_args()


def main():
    """The main function."""
    args = parse_args()

    outdir = args.out_dir
    binary = args.binary

    print('Using output dir %s for %s' % (outdir, binary))
    if os.path.isdir(outdir):
        shutil.rmtree(outdir)
    os.mkdir(outdir)

    # Get loader
    interp, _ = get_binary_info(binary)

    # Determine all library dependencies
    libs = set()
    libs.update(get_library_deps(binary))
    libs.add(interp)

    for lib in libs:
        newlib = os.path.join(outdir, os.path.basename(lib))
        shutil.copy(lib, newlib)


if __name__ == '__main__':
    main()
