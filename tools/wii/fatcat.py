"""Print or copy a file out of a FAT32 SD card image (e.g. Dolphin's WiiSD.raw),
or put a file (8.3 name) into it.

    python -I tools/wii/fatcat.py IMAGE burnout3/run.log [OUTFILE]
    python -I tools/wii/fatcat.py IMAGE --put LOCALFILE burnout3/args.txt
    python -I tools/wii/fatcat.py IMAGE --ls burnout3

Path components match 8.3 short names or long names, case-insensitively
(--put writes 8.3 names only)."""
import struct
import sys


class Fat32:
    def __init__(self, path, write=False):
        self.f = open(path, "r+b" if write else "rb")
        self.base = 0
        b = self.read(0, 512)
        if b[82:87] != b"FAT32":
            # partitioned: first MBR entry
            self.base = struct.unpack_from("<I", b, 0x1BE + 8)[0] * 512
            b = self.read(0, 512)
            if b[82:87] != b"FAT32":
                sys.exit("not a FAT32 image")
        self.bps, self.spc = struct.unpack_from("<HB", b, 11)
        reserved, nfats = struct.unpack_from("<HB", b, 14)
        fatsz, self.root = struct.unpack_from("<I4xI", b, 36)
        self.fat = reserved * self.bps
        self.fatsz, self.nfats = fatsz * self.bps, nfats
        self.data = (reserved + nfats * fatsz) * self.bps
        self.csize = self.bps * self.spc

    def read(self, off, n):
        self.f.seek(self.base + off)
        return self.f.read(n)

    def chain(self, c):
        while 2 <= c < 0x0FFFFFF8:
            yield c
            c = struct.unpack("<I", self.read(self.fat + 4 * c, 4))[0] & 0x0FFFFFFF

    def cluster(self, c):
        return self.read(self.data + (c - 2) * self.csize, self.csize)

    def write(self, off, data):
        self.f.seek(self.base + off)
        self.f.write(data)

    def set_fat(self, c, v):
        for k in range(self.nfats):
            self.write(self.fat + k * self.fatsz + 4 * c, struct.pack("<I", v))

    def free_clusters(self, count):
        """the first `count` free clusters"""
        n = self.fatsz // 4
        out = []
        for start in range(0, n, 16384):
            b = self.read(self.fat + 4 * start, 4 * min(16384, n - start))
            for k in range(0, len(b), 4):
                if start + k // 4 >= 2 and struct.unpack_from("<I", b, k)[0] & 0x0FFFFFFF == 0:
                    out.append(start + k // 4)
                    if len(out) == count:
                        return out
        sys.exit("image is full")

    def put(self, path, data):
        parts = path.strip("/").split("/")
        dirc = self.lookup("/".join(parts[:-1]))[0] if len(parts) > 1 else self.root
        base, _, ext = parts[-1].upper().partition(".")
        if len(base) > 8 or len(ext) > 3:
            sys.exit("put needs an 8.3 name")
        name83 = base.ljust(8).encode() + ext.ljust(3).encode()
        slot = None
        for cl in self.chain(dirc):
            d = self.cluster(cl)
            for k in range(0, len(d), 32):
                e = d[k:k + 32]
                off = self.data + (cl - 2) * self.csize + k
                if e[0] in (0, 0xE5):
                    slot = slot or off
                    if e[0] == 0:
                        break
                elif e[11] != 0x0F and e[0:11] == name83:
                    slot = off
                    break
            else:
                continue
            break
        if slot is None:
            sys.exit("directory is full")
        e = bytearray(self.read(slot, 32))
        if e[0:11] == name83:
            # drop the old cluster chain
            old = struct.unpack_from("<H", e, 20)[0] << 16 | struct.unpack_from("<H", e, 26)[0]
            for cl in list(self.chain(old)):
                self.set_fat(cl, 0)
        else:
            e = bytearray(32)
            e[0:11] = name83
            e[11] = 0x20
        nclus = max(1, (len(data) + self.csize - 1) // self.csize)
        clusters = self.free_clusters(nclus)
        for k, cl in enumerate(clusters):
            self.set_fat(cl, clusters[k + 1] if k + 1 < nclus else 0x0FFFFFFF)
            chunk = data[k * self.csize:(k + 1) * self.csize]
            self.write(self.data + (cl - 2) * self.csize, chunk.ljust(self.csize, b"\0"))
        first = clusters[0]
        struct.pack_into("<H", e, 20, first >> 16)
        struct.pack_into("<H", e, 26, first & 0xFFFF)
        struct.pack_into("<I", e, 28, len(data))
        self.write(slot, bytes(e))

    def entries(self, c):
        """(short name, attr, first cluster, size, long name or None)"""
        lfn = {}
        for cl in self.chain(c):
            d = self.cluster(cl)
            for k in range(0, len(d), 32):
                e = d[k:k + 32]
                if e[0] == 0:
                    return
                if e[0] == 0xE5:
                    lfn = {}
                    continue
                if e[11] == 0x0F:
                    part = (e[1:11] + e[14:26] + e[28:32]).decode("utf-16-le", "replace")
                    lfn[e[0] & 0x1F] = part.split(chr(0))[0].replace(chr(0xFFFF), "")
                    continue
                name = e[0:8].decode("latin-1").rstrip()
                ext = e[8:11].decode("latin-1").rstrip()
                full = f"{name}.{ext}" if ext else name
                first = struct.unpack_from("<H", e, 20)[0] << 16 | struct.unpack_from("<H", e, 26)[0]
                size = struct.unpack_from("<I", e, 28)[0]
                long = "".join(lfn[i] for i in sorted(lfn)) if lfn else None
                lfn = {}
                yield full.lower(), e[11], first, size, long

    def lookup(self, path):
        c, size, attr = self.root, 0, 0x10
        for part in path.strip("/").split("/"):
            for name, attr, first, sz, long in self.entries(c):
                if name == part.lower() or (long and long.lower() == part.lower()):
                    c, size = first, sz
                    break
            else:
                sys.exit(f"{path}: {part} not found")
        return c, size, attr

    def readfile(self, path):
        c, size, _ = self.lookup(path)
        out = bytearray()
        for cl in self.chain(c):
            out += self.cluster(cl)
            if len(out) >= size:
                break
        return bytes(out[:size])


def main():
    if len(sys.argv) > 4 and sys.argv[2] == "--put":
        Fat32(sys.argv[1], write=True).put(sys.argv[4], open(sys.argv[3], "rb").read())
        return
    fs = Fat32(sys.argv[1])
    if len(sys.argv) > 3 and sys.argv[2] == "--ls":
        c, _, _ = fs.lookup(sys.argv[3])
        for name, attr, first, sz, long in fs.entries(c):
            print(long or name, sz)
        return
    data = fs.readfile(sys.argv[2])
    if len(sys.argv) > 3:
        open(sys.argv[3], "wb").write(data)
    else:
        sys.stdout.write(data.decode("utf-8", "replace"))


if __name__ == "__main__":
    main()
