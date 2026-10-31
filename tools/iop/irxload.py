"""Load a PS2 IOP module (.IRX) relocated to a fixed IOP address.

IRX files are MIPS I ELF images (e_type 0xFF80) linked at 0 with REL
relocations whose offsets are module addresses. Relocating to `base` gives a
flat image the recompiler can treat like an executable. Import stubs (the
0x41E00000 tables) are reported so calls to other IOP libraries can be routed
to native implementations.

The IRX format and import-stub layout follow ps2sdk's headers; no ps2sdk source
is included. See CREDITS.md.
"""
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from irxdis import EXPORTS  # noqa: E402

R_MIPS_32, R_MIPS_26, R_MIPS_HI16, R_MIPS_LO16 = 2, 4, 5, 6


class IrxImage:
    """Same interface as elf.Image: base, data, memsz, end, entry, u32(), contains()."""

    def __init__(self, path, base):
        d = open(path, "rb").read()
        if d[:4] != b"\x7fELF" or struct.unpack_from("<H", d, 16)[0] != 0xFF80:
            raise SystemExit(f"{path}: not an IRX module")
        self.path = path
        self.base = base
        entry, phoff = struct.unpack_from("<II", d, 24)
        phnum, = struct.unpack_from("<H", d, 44)
        img = None
        for k in range(phnum):
            p_type, off, vaddr, _pa, filesz, memsz, _fl, _al = struct.unpack_from("<8I", d, phoff + 32 * k)
            if p_type == 1:
                img = bytearray(d[off:off + filesz])
                self.memsz = memsz
                if vaddr:
                    raise SystemExit("IRX load segment not at 0")
        shoff, = struct.unpack_from("<I", d, 32)
        shnum, shstr = struct.unpack_from("<HH", d, 48)
        secs = [struct.unpack_from("<10I", d, shoff + 40 * k) for k in range(shnum)]
        names = secs[shstr][4]
        self.sections = {}
        nrel = 0
        for s in secs:
            name = d[names + s[0]:d.index(b"\0", names + s[0])].decode()
            self.sections[name] = (s[3], s[5])
            if s[1] == 9:                               # SHT_REL
                nrel += self._relocate(img, d[s[4]:s[4] + s[5]])
        self.nrel = nrel
        self.data = bytes(img)
        self.entry = base + entry
        self.end = base + len(self.data)
        self.text = self.sections[".text"]
        self.imports = self._find_imports()             # stub address -> (lib, ordinal)
        self.exports = self._find_exports()             # library name -> [function address]

    def _relocate(self, img, rel):
        base = self.base
        pending_hi = []
        n = 0
        for k in range(0, len(rel), 8):
            off, info = struct.unpack_from("<II", rel, k)
            typ = info & 0xFF
            if off + 4 > len(img):
                continue                                # relocation inside .bss: nothing to patch
            w, = struct.unpack_from("<I", img, off)
            if typ == R_MIPS_32:
                w = (w + base) & 0xFFFFFFFF
            elif typ == R_MIPS_26:
                w = (w & 0xFC000000) | (((w & 0x03FFFFFF) + (base >> 2)) & 0x03FFFFFF)
            elif typ == R_MIPS_HI16:
                pending_hi.append(off)
                continue
            elif typ == R_MIPS_LO16:
                lo = w & 0xFFFF
                lo_s = lo - 0x10000 if lo & 0x8000 else lo
                for h in pending_hi:
                    hw, = struct.unpack_from("<I", img, h)
                    ahl = ((hw & 0xFFFF) << 16) + lo_s + base
                    hw = (hw & 0xFFFF0000) | (((ahl + 0x8000) >> 16) & 0xFFFF)
                    struct.pack_into("<I", img, h, hw)
                    n += 1
                pending_hi = []
                w = (w & 0xFFFF0000) | ((lo + base) & 0xFFFF)
            elif typ == 0:
                continue
            else:
                raise SystemExit(f"unsupported relocation type {typ} at {off:#x}")
            struct.pack_into("<I", img, off, w)
            n += 1
        if pending_hi:
            raise SystemExit("HI16 relocation without LO16")
        return n

    def u32(self, addr):
        return struct.unpack_from("<I", self.data, addr - self.base)[0]

    def contains(self, addr):
        return self.base <= addr < self.end

    def _find_imports(self):
        t0, tsize = self.text
        out = {}
        self.import_tables = []                         # [(start, end)] IOP addresses
        a = t0
        while a + 20 <= t0 + tsize:
            if self.u32(self.base + a) == 0x41E00000:
                lib = self.data[a + 12:a + 20].split(b"\0")[0].decode("latin-1")
                p = a + 20
                while p + 8 <= t0 + tsize and self.u32(self.base + p) == 0x03E00008:
                    out[self.base + p] = (lib, self.u32(self.base + p + 4) & 0xFFFF)
                    p += 8
                if p + 8 <= t0 + tsize and self.u32(self.base + p) == 0 and self.u32(self.base + p + 4) == 0:
                    p += 8                              # terminator
                self.import_tables.append((self.base + a, self.base + p))
                a = p
            else:
                a += 4
        return out

    def _find_exports(self):
        """Export tables {0x41C00000, next, version, mode, name[8], functions..., 0}."""
        t0, tsize = self.text
        out = {}
        self.export_tables = []                         # [(start, end)] IOP addresses
        for a in range(0, len(self.data) - 24, 4):
            if self.u32(self.base + a) != 0x41C00000:
                continue
            name = self.data[a + 12:a + 20].split(bytes(1))[0]
            if not name or not all(32 < ch < 127 for ch in name):
                continue
            funcs, p = [], a + 20
            while p + 4 <= len(self.data) and self.u32(self.base + p):
                funcs.append(self.u32(self.base + p))
                p += 4
            if all(self.base + t0 <= f < self.base + t0 + tsize for f in funcs):
                out[name.decode("latin-1")] = funcs
                self.export_tables.append((self.base + a, self.base + p + 4))
        return out

    def code_ranges(self):
        """.text without the import and export tables."""
        t0, tsize = self.text
        lo, hi = self.base + t0, self.base + t0 + tsize
        out = []
        for a, b in sorted(t for t in self.import_tables + self.export_tables if t[0] < hi and t[1] > lo):
            if a > lo:
                out.append((lo, a))
            lo = b
        if lo < hi:
            out.append((lo, hi))
        return out

    def import_name(self, lib, ordinal):
        return EXPORTS.get(lib, {}).get(ordinal, str(ordinal))
