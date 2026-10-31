"""Minimal ELF32 little-endian loader for PS2 executables."""
import struct


class Image:
    """The loaded memory image of an ELF: one contiguous byte array from base."""

    def __init__(self, path):
        data = open(path, "rb").read()
        assert data[:4] == b"\x7fELF" and data[4] == 1 and data[5] == 1, "not ELF32 LE"
        self.entry, phoff = struct.unpack_from("<II", data, 24)
        phnum = struct.unpack_from("<H", data, 44)[0]
        segs = []
        for i in range(phnum):
            p_type, p_off, p_vaddr, _, p_filesz, p_memsz, _, _ = struct.unpack_from("<8I", data, phoff + i * 32)
            if p_type == 1 and p_memsz:
                segs.append((p_vaddr, data[p_off:p_off + p_filesz], p_memsz))
        assert len(segs) == 1, "expected a single PT_LOAD"
        self.base, self.data, self.memsz = segs[0]
        self.end = self.base + len(self.data)

    def contains(self, addr):
        return self.base <= addr < self.end

    def u32(self, addr):
        return struct.unpack_from("<I", self.data, addr - self.base)[0]
