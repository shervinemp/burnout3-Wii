"""Pad every section of a DOL to 32 bytes (address and size).

elf2dol copies ELF segments as they are; Dolphin and some loaders reject
sections whose size is not a multiple of 32. The padding is zeros, which at
the end of the data segment fall on .sbss/.bss that crt0 clears anyway.

    python -I tools/wii/dolfix.py build-wii/burnout3.dol
"""
import struct
import sys


def main(path):
    d = open(path, "rb").read()
    hdr = list(struct.unpack_from(">64I", d, 0))
    offs, addrs, sizes = hdr[0:18], hdr[18:36], hdr[36:54]
    out = bytearray(0x100)
    pos = 0x100
    for k in range(18):
        if not sizes[k]:
            offs[k] = 0
            continue
        if addrs[k] & 31:
            sys.exit(f"section {k} at {addrs[k]:08X} is not 32-byte aligned")
        body = d[offs[k]:offs[k] + sizes[k]]
        size = (sizes[k] + 31) & ~31
        body += bytes(size - len(body))
        offs[k], sizes[k] = pos, size
        out += body
        pos += size
    hdr[0:18], hdr[36:54] = offs, sizes
    struct.pack_into(">64I", out, 0, *hdr)
    open(path, "wb").write(out)


if __name__ == "__main__":
    main(sys.argv[1])
