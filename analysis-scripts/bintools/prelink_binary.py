#!/usr/bin/env python3

"""
Adapted from prelink_binary.py from
https://github.com/vusec/midfat/blob/master/shrinkaddrspace/prelink_binary.py
"""

import argparse
import os
import shlex
import shutil
import subprocess
import re

from elftools.elf.elffile import ELFFile

from common import ex, get_binary_info, get_library_deps


LDD_REGEX = re.compile(r".*.so.* => (/.*\.so[^ ]*)")
LDD_NOT_FOUND_REGEX = re.compile(r"(.*.so.*) => not found")


def parse_args():
    parser = argparse.ArgumentParser(description="Shrink address space of the "
                                     "given binary by prelinking all "
                                     "dependency libraries.")
    parser.add_argument("binary", help="The ELF binary")
    parser.add_argument("--in-place", help="Modify binary in-place",
                        action="store_true", default=False)
    parser.add_argument("--set-rpath", action="store_true", default=False,
                        help="Set RPATH of (new) binary and preload lib to "
                             "out-dir")
    parser.add_argument("--out-dir", default=None,
                        help="Output directory for prelinked libs")
    parser.add_argument("--base-addr", default="0xffffffff",
                        help="New base address for libs")

    return parser.parse_args()


def get_overlap(mapping, other_mappings):
    """
    Returns the *last* area out of `other_mappings` that overlaps with
    `mapping`, or None. Assumes `other_mappings` is ordered.
    """

    start, size = mapping
    end = start + size

    for m in other_mappings[::-1]:
        m_start, m_size = m
        m_end = m_start + m_size
        if (start >= m_start and start < m_end) or \
           (end > m_start and end <= m_end) or \
           (start <= m_start and end >= m_end):
            return m

    return None


def prelink_libs(libs, outdir, existing_mappings, baseaddr):
    """
    For every library we calculate its size and alignment, find a space in
    our new compact addr space and create a copy of the library that is
    prelinked to the addr. Start mapping these from the *end* of the addr space
    down, but leaving a bit of space at the top for stuff like the stack.
    """

    for lib in libs:
        with open(lib, 'rb') as f:
            # Determine the alignment and size required for all LOAD segments
            # combined
            align, size = 0, 0
            e = ELFFile(f)
            for seg in e.iter_segments():
                if seg['p_type'] != 'PT_LOAD':
                    continue
                if seg['p_align'] > align:
                    align = seg['p_align']
                size = seg['p_vaddr'] + seg['p_memsz']

            baseaddr -= size

            # Search for a slot that is not overlapping with anything else and
            # aligned properly
            found = False
            while not found:
                if baseaddr % align:
                    baseaddr -= baseaddr % align
                overlap = get_overlap((baseaddr, size), existing_mappings)
                if overlap:
                    baseaddr = overlap[0] - size
                else:
                    found = True

            if baseaddr < 0:
                raise Exception("Invalid base address %#x for %s" % (baseaddr,
                                lib))

            print("Found %#08x - %#08x for %s" % (baseaddr, baseaddr + size,
                  lib))

            newlib = os.path.join(outdir, os.path.basename(lib))
            shutil.copy(lib, newlib)
            ex("prelink -r %#x \"%s\"" % (baseaddr, newlib))


def main():
    args = parse_args()

    outdir = args.out_dir
    if not args.out_dir:
        outdir = os.path.abspath("prelink-%s" % args.binary.replace("/", "_"))

    print("Using output dir %s for %s" % (outdir, args.binary))
    if os.path.isdir(outdir):
        shutil.rmtree(outdir)
    os.mkdir(outdir)

    # Get loader and existing mappings for binary
    interp, binary_mappings = get_binary_info(args.binary)

    # Determine all dependency libraries
    libs = set()
    libs.update(get_library_deps(args.binary))
    libs.add(interp)

    # The magic, construct new addr space by prelinking all dependency libs
    prelink_libs(libs, outdir, binary_mappings, int(args.base_addr, 16))

    # Update the loader to use our prelinked version
    if args.in_place:
        # Create a backup of the original binary
        shutil.copy(args.binary, '%s.bak' % args.binary)
        newprog = args.binary
    else:
        newprog = os.path.join(outdir, os.path.basename(args.binary))
        shutil.copy(args.binary, newprog)
    newinterp = os.path.realpath(os.path.join(outdir, os.path.basename(interp)))
    ex("patchelf --set-interpreter \"%s\" \"%s\"" % (newinterp, newprog))

    # By setting the rpath, we can avoid having to specify LD_LIBRARY_PATH
    if args.set_rpath:
        absoutdir = os.path.realpath(outdir)
        ex("patchelf --set-rpath \"%s\" \"%s\"" % (absoutdir, newprog))


if __name__ == '__main__':
    main()
