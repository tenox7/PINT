#!/usr/bin/env python3
"""dumpinfo.py DUMP [ADDRESS...]: read an NT crash dump of PINT (ARM), C:\\WINNT\\MEMORY.DMP,
which savedump writes at the start after a STOP screen (crash_dump=1 and total_mem= in
config.txt), copied off the card; or C:\\pagefile.sys before that start.

It prints the STOP code and its parameters, the registers at the bug check, the loaded
drivers (from PsLoadedModuleList, through the page tables of the dump), and the driver
and offset of every register and parameter that points into one.

    tools/dumpinfo.py MEMORY.DMP
    tools/dumpinfo.py MEMORY.DMP F5849F12 ...       (also place these addresses)

The dump is a header page (DUMP_HEADER, ntiodump.h: the physical memory runs at ULONG 25,
the CONTEXT of the bug check at ULONG 200; the ARM CONTEXT is ContextFlags, R0-R12, Sp, Lr,
Pc, Cpsr), then the pages of the runs in order.
"""
import struct
import sys

PAGE = 4096


class Dump:
    def __init__(self, path):
        self.f = open(path, 'rb')
        h = self.f.read(PAGE)
        if h[0:8] != b'PAGEDUMP':
            sys.exit('%s: no dump (it starts %r, not PAGEDUMP)' % (path, h[0:8]))
        u = lambda i: struct.unpack_from('<I', h, i * 4)[0]
        self.dtb = u(4)
        self.pfn = u(5)
        self.modules = u(6)
        self.processes = u(7)
        self.machine = u(8)
        self.processors = u(9)
        self.bugcheck = [u(10 + i) for i in range(5)]
        self.version = (u(2), u(3))
        self.user = h[60:92].split(b'\0')[0].decode('latin-1')
        runs, pages = u(25), u(26)
        self.runs = []                      # (first page, pages, offset in the file)
        offset = PAGE
        for i in range(min(runs, 32)):
            base, count = u(27 + 2 * i), u(28 + 2 * i)
            self.runs.append((base, count, offset))
            offset += count * PAGE
        self.pages = pages
        self.context = struct.unpack_from('<18I', h, 200 * 4)

    def physical(self, address, length):
        page = address // PAGE
        for base, count, offset in self.runs:
            if base <= page < base + count:
                self.f.seek(offset + (page - base) * PAGE + address % PAGE)
                return self.f.read(min(length, PAGE - address % PAGE))
        return None

    def u32p(self, address):
        b = self.physical(address, 4)
        return None if b is None or len(b) < 4 else struct.unpack('<I', b)[0]

    def translate(self, va):
        """PINT's ARM memory manager keeps the MIPS (R4000) design: KSEG0 (0x80000000-0xBFFFFFFF)
        is the first gigabyte of physical memory; the rest goes through NT's own two-level
        page tables (the ARM MMU's are a cache of them), R4000 PTEs (Valid bit 1, the page in
        bits 6-29): the directory, a page whose PTE is the DirectoryTableBase, a PDE for each
        4 MB (va >> 22), a page of 1024 PTEs under it."""
        if 0x80000000 <= va < 0xC0000000:
            return va - 0x80000000
        if not self.dtb & 2:
            return None
        pde = self.u32p(((self.dtb >> 6) & 0xFFFFFF) * PAGE + (va >> 22) * 4)
        if pde is None or not pde & 2:
            return None
        pte = self.u32p(((pde >> 6) & 0xFFFFFF) * PAGE + ((va >> 12) & 0x3FF) * 4)
        if pte is None or not pte & 2:
            return None
        return ((pte >> 6) & 0xFFFFFF) * PAGE + (va & 0xFFF)

    def read(self, va, length):
        out = b''
        while length > 0:
            pa = self.translate(va)
            if pa is None:
                return None
            chunk = self.physical(pa, min(length, PAGE - va % PAGE))
            if not chunk:
                return None
            out += chunk
            va += len(chunk)
            length -= len(chunk)
        return out

    def u32(self, va):
        b = self.read(va, 4)
        return None if b is None else struct.unpack('<I', b)[0]

    def unicode(self, va):
        b = self.read(va, 8)
        if b is None:
            return '?'
        length, _, buf = struct.unpack('<HHI', b)
        s = self.read(buf, min(length, 512))
        return '?' if s is None else s.decode('utf-16-le', 'replace')

    def loaded_modules(self):
        mods, entry, seen = [], self.u32(self.modules), set()
        while entry and entry != self.modules and entry not in seen and len(mods) < 256:
            seen.add(entry)
            b = self.read(entry, 52)
            if b is None:
                break
            base, ep, size = struct.unpack_from('<III', b, 24)
            mods.append((base, size, self.unicode(entry + 44)))
            entry = struct.unpack_from('<I', b, 0)[0]
        return mods


def place(mods, value):
    for base, size, name in mods:
        if base <= value < base + size:
            return '%s+0x%X' % (name, value - base)
    return ''


def main():
    if len(sys.argv) < 2:
        sys.exit('usage: dumpinfo.py DUMP [ADDRESS...]')
    d = Dump(sys.argv[1])
    code, *params = d.bugcheck
    print('STOP 0x%08X (0x%08X, 0x%08X, 0x%08X, 0x%08X)' % (code, *params))
    build = {0xF: 'free', 0xC: 'checked'}.get(d.version[0] & 0xFFFF, '0x%X' % d.version[0])
    print('NT build %d (%s), %d processor(s), %d MB in %d run(s)' %
          (d.version[1], build, d.processors, d.pages * PAGE >> 20, len(d.runs)))
    mods = d.loaded_modules()
    names = ['R%d' % i for i in range(13)] + ['SP', 'LR', 'PC', 'CPSR']
    print('\nRegisters at the bug check:')
    for name, value in zip(names, d.context[1:]):
        where = place(mods, value)
        print('  %-4s %08X  %s' % (name, value, where))
    print('\nParameters:')
    for i, value in enumerate(params):
        print('  P%d   %08X  %s' % (i + 1, value, place(mods, value)))
    for arg in sys.argv[2:]:
        value = int(arg, 16)
        print('  %08X  %s' % (value, place(mods, value) or '(in no loaded driver)'))
    print('\nLoaded drivers (%d):' % len(mods))
    for base, size, name in sorted(mods):
        print('  %08X-%08X  %s' % (base, base + size, name))


if __name__ == '__main__':
    main()
