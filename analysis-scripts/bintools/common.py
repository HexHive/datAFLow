import re
import shlex
import subprocess

from elftools.elf.elffile import ELFFile
from elftools.elf.segments import InterpSegment


LDD_REGEX = re.compile(r".*.so.* => (/.*\.so[^ ]*)")
LDD_NOT_FOUND_REGEX = re.compile(r"(.*.so.*) => not found")


def ex(cmd):
    """
    Execute a given command (string), returning stdout if succesful and
    raising an exception otherwise.
    """

    p = subprocess.Popen(shlex.split(cmd), stdout=subprocess.PIPE,
                         stderr=subprocess.PIPE)
    p.wait()
    if p.returncode:
        print("Error while executing command '%s': %d" % (cmd, p.returncode))
        print("stdout: '%s'" % p.stdout.read())
        print("stderr: '%s'" % p.stderr.read())
        raise Exception("Error while executing command")
    return p.stdout.read().decode('utf8')


def get_binary_info(prog):
    """
    Look for the loader requested by the program, and record existing
    mappings required by the program itself.
    """

    interp = None
    binary_mappings = []

    with open(prog, 'rb') as f:
        e = ELFFile(f)
        for seg in e.iter_segments():
            if isinstance(seg, InterpSegment):
                interp = seg.get_interp_name()
            if seg['p_type'] == 'PT_LOAD':
                binary_mappings.append((seg['p_vaddr'], seg['p_memsz']))
    if interp is None:
        raise Exception("Could not find interp in binary")

    return interp, binary_mappings


def get_library_deps(library):
    """
    Look for all dependency libraries for a given ELF library/binary, and
    return a list of full paths. Uses the ldd command to find this information
    at load-time, so may not be complete.
    """

    # TODO: do we have to do this recursively for all deps?

    deps = []

    ldd = ex("ldd \"%s\"" % library)
    for l in ldd.split("\n"):
        m = LDD_NOT_FOUND_REGEX.search(l)
        if m:
            missing_lib = m.group(1).strip()
            raise Exception("Could not find %s - check LD_LIBRARY_PATH" %
                            missing_lib)

        m = LDD_REGEX.search(l)
        if not m:
            continue
        deps.append(m.group(1))
    return deps
