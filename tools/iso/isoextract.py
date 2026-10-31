"""Extract files from an ISO9660 image (PS2 DVD layout).

Usage:
    python -I tools/iso/isoextract.py <image.iso> <out_dir> [--list] [path ...]

With no paths, extracts everything. Paths are ISO paths such as SLUS_210.50 or
DATA/FOO.BIN (case-insensitive, ";1" version suffix optional).
"""
import os
import struct
import sys

SECTOR = 2048


def read_sectors(f, lba, count):
    f.seek(lba * SECTOR)
    return f.read(count * SECTOR)


def parse_dir(f, lba, size, prefix, out):
    data = read_sectors(f, lba, (size + SECTOR - 1) // SECTOR)
    pos = 0
    while pos < len(data):
        rec_len = data[pos]
        if rec_len == 0:
            # records never span sectors; skip to next sector
            pos = (pos // SECTOR + 1) * SECTOR
            continue
        rec = data[pos:pos + rec_len]
        ext_lba = struct.unpack_from("<I", rec, 2)[0]
        ext_size = struct.unpack_from("<I", rec, 10)[0]
        flags = rec[25]
        name_len = rec[32]
        name = rec[33:33 + name_len]
        pos += rec_len
        if name in (b"\x00", b"\x01"):
            continue
        name = name.decode("ascii", "replace").split(";")[0]
        path = prefix + name
        if flags & 2:
            parse_dir(f, ext_lba, ext_size, path + "/", out)
        else:
            out.append((path, ext_lba, ext_size))


def walk(f):
    pvd = read_sectors(f, 16, 1)
    assert pvd[1:6] == b"CD001", "not an ISO9660 image"
    root = pvd[156:156 + 34]
    lba = struct.unpack_from("<I", root, 2)[0]
    size = struct.unpack_from("<I", root, 10)[0]
    files = []
    parse_dir(f, lba, size, "", files)
    return files


def main():
    args = [a for a in sys.argv[1:] if not a.startswith("--")]
    listing = "--list" in sys.argv
    image, out_dir, wanted = args[0], args[1], [w.upper().split(";")[0] for w in args[2:]]
    with open(image, "rb") as f:
        files = walk(f)
        total = 0
        for path, lba, size in files:
            if wanted and path.upper() not in wanted:
                continue
            total += size
            if listing:
                print(f"{lba:8d} {size:11d} {path}")
                continue
            dst = os.path.join(out_dir, *path.split("/"))
            os.makedirs(os.path.dirname(dst), exist_ok=True)
            f.seek(lba * SECTOR)
            remain = size
            with open(dst, "wb") as o:
                while remain:
                    chunk = f.read(min(remain, 1 << 20))
                    o.write(chunk)
                    remain -= len(chunk)
        print(f"{'listed' if listing else 'extracted'} {total} bytes", file=sys.stderr)


if __name__ == "__main__":
    main()
