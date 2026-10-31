"""Code patches chosen at run time (the code half of a PCSX2 .pnach patch).

PCSX2 applies a patch by writing words into the game's memory; the parts that change instructions only take
effect in a recompiled game if the recompiler compiles them in. Each set in the title's CODE_PATCHES is a bit
of the runtime's g_code_patch and a list of pnach writes (address, size in bytes, value). Of those, the words
in the code that the writes change are the patch's sites:

* a site whose new word is a plain instruction (an immediate changed: "short" writes over a lui, an addiu) runs
  that instruction instead of the original;
* a site whose new word is `j cave` with the cave outside the code runs what PCSX2 runs: the instruction
  after the site (the jump's delay slot), the cave's instructions up to its `j` back, and the word after that
  `j` (its delay slot, from the patched memory, often the next cave's first instruction), then goes on at
  the instruction after the site, which it runs again. The `j` back must return there.

At a site the emitted code is `if (g_code_patch & bit) { new } else { original }`, one step for the
emitter's analyses: it reads what either variant reads, may write what either writes, and writes for sure
only what both write (a register only one variant writes keeps its value on the other path, so the
analyses keep it as they would a register written on one branch of an if). With the bit clear the program
does what it did before.

A site that cannot be taken this way (the original or a new instruction a branch, a jump, a call, a
syscall; a cave that does not come straight back) is reported and left out: that part of the patch is not
applied.
"""
from r5900 import HAS_DELAY_SLOT, decode, format_insn

NOT_PLAIN = HAS_DELAY_SLOT | {"syscall", "eret", "break", "unknown", "vcallms", "vcallmsr"}
CAVE_MAX = 32               # instructions in a cave before its jump back


class Site:
    __slots__ = ("pc", "mask", "name", "alt", "cave")

    def __init__(self, pc, mask, name, alt, cave):
        self.pc = pc
        self.mask = mask        # the bit of g_code_patch
        self.name = name        # the set's name
        self.alt = alt          # the instructions run instead of the original, in order (Insn, with their own pcs)
        self.cave = cave        # a jump into a cave (not to be taken in a branch's delay slot)


def _plain(i):
    return i.op not in NOT_PLAIN


def parse_pnach(text):
    """[(address, size, value)] of the patch lines of a .pnach section: "patch=1,EE,address,type,value // ...".
    byte/short/word write 1/2/4 bytes at the address (in any of RAM's views); "extended" codes 0, 1 and 2 write
    1, 2 and 4 bytes at the low 28 bits. Other lines (gametitle, author, comments) are skipped."""
    out = []
    for line in text.splitlines():
        line = line.split("//")[0].strip()
        if not line.startswith("patch="):
            continue
        f = [x.strip() for x in line[len("patch="):].split(",")]
        if len(f) != 5 or f[1] != "EE":
            raise ValueError(f"pnach line not understood: {line}")
        addr, kind, value = int(f[2], 16), f[3].lower(), int(f[4], 16)
        if kind == "extended":
            code = addr >> 28
            if code not in (0, 1, 2):
                raise ValueError(f"extended code {code:X} not supported: {line}")
            size = (1, 2, 4)[code]
        else:
            size = {"byte": 1, "short": 2, "word": 4}[kind]
        out.append((addr & 0x01FFFFFF, size, value & ((1 << (8 * size)) - 1)))
    return out


def build(img, text_ranges, code_patches, log=print):
    """pc -> Site for every site of every set in code_patches = {name: (bit, [(address, size, value), ...])}."""
    def in_text(a):
        return any(lo <= a < hi for lo, hi in text_ranges)

    sites = {}
    for name, (bit, writes) in sorted(code_patches.items(), key=lambda kv: kv[1][0]):
        mask = 1 << bit
        if isinstance(writes, str):
            writes = parse_pnach(writes)
        mem = {}                            # word address -> the word with this set's writes

        def word(a):
            a &= ~3
            if a in mem:
                return mem[a]
            return img.u32(a) if img.contains(a) else 0

        for addr, size, value in writes:
            a = addr & ~3
            if size == 4:
                if addr & 3:
                    raise ValueError(f"{name}: unaligned word write at {addr:08X}")
                mem[a] = value & 0xFFFFFFFF
                continue
            if size not in (1, 2) or (size == 2 and addr & 1):
                raise ValueError(f"{name}: write of {size} bytes at {addr:08X}")
            shift = (addr & 3) * 8          # little endian: the byte at addr is bits shift..shift+7 of the word
            m = ((1 << (8 * size)) - 1) << shift
            mem[a] = (word(a) & ~m) | ((value << shift) & m)
        n_ok = 0
        for a in sorted(mem):
            if not in_text(a) or not img.contains(a) or mem[a] == img.u32(a):
                continue                    # data (a cave, or nothing to do with code), or unchanged
            orig = decode(img.u32(a), a)
            new = decode(mem[a], a)
            why = None
            alt = None
            if not _plain(orig):
                why = f"the original instruction is {format_insn(orig)}"
            elif _plain(new):
                alt = [new]
            elif new.op != "j":
                why = f"the new instruction is {format_insn(new)}"
            elif in_text(new.target):
                why = f"it jumps into the code ({new.target:08X})"
            else:
                ds = decode(word(a + 4), a + 4)
                alt = [ds]
                p = new.target
                while len(alt) <= CAVE_MAX:
                    i = decode(word(p), p)
                    if i.op == "j":
                        if i.target != a + 4:
                            why = f"its cave at {new.target:08X} goes on at {i.target:08X}, not {a + 4:08X}"
                        alt.append(decode(word(p + 4), p + 4))
                        break
                    alt.append(i)
                    p += 4
                else:
                    why = f"its cave at {new.target:08X} has no jump back within {CAVE_MAX} instructions"
                if why is None:
                    bad = [i for i in alt if not _plain(i)]
                    if bad:
                        why = f"the cave path runs {format_insn(bad[0])} at {bad[0].pc:08X}"
            if why is None and a in sites:
                why = f"set {sites[a].name} patches it too"
            if why is not None:
                log(f"code patch {name}: {a:08X} left out: {why}")
                continue
            sites[a] = Site(a, mask, name, alt, len(alt) > 1 or alt[0].pc != a)
            n_ok += 1
        log(f"code patch {name} (g_code_patch bit {bit}): {n_ok} sites")
    return sites


def describe(site):
    """One comment line: what a site runs instead of the original."""
    return "; ".join(f"{i.pc:08X} {format_insn(i)}" for i in site.alt)
