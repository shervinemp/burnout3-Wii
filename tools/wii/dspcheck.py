"""What the linker put at 0x81000000.

libogc's start-up (__dsp_bootstrap) writes the DSP's 128 bytes of boot code to
0x81000000, starts the DSP and puts the old bytes back. With more than 16 MB
of code a function lives there: one that start-up itself calls (memcpy, the
cache flush) then runs as DSP code, and an emulator keeps the garbage it
translated (a build once took minutes to boot that way). Only code that does
not run before main may be there: recompiled game code (f_...) or the padding
of platform/wii/dsppad.S, which Makefile.wii puts between the program's own
code and the libraries.

    python -I tools/wii/dspcheck.py NM ELF          check; fails when something else is there
    python -I tools/wii/dspcheck.py --pad NM ELF    print the padding (bytes) the link needs
"""
import subprocess
import sys

LO, HI = 0x81000000, 0x81000080


def symbols(nm, elf):
    syms = []
    for line in subprocess.run([nm, "-n", elf], capture_output=True, text=True, check=True).stdout.splitlines():
        p = line.split()
        if len(p) == 3 and p[1] in "tTwW":
            syms.append((int(p[0], 16), p[2]))
    return syms


def main(args):
    pad = args[0] == "--pad"
    syms = symbols(*args[-2:])
    by_name = dict((n, a) for a, n in syms)
    if pad:
        start, end = by_name["__dsp_pad"], by_name["__dsp_pad_end"]
        # the libraries follow the padding: they must start past the DSP's bytes,
        # unless all of the code ends below them
        need = 0 if start >= HI or syms[-1][0] < LO else HI - start
        print((need + 31) & ~31 if need > end - start else end - start)
        return
    there = [n for k, (a, n) in enumerate(syms)
             if a < HI and (syms[k + 1][0] if k + 1 < len(syms) else a + 4) > LO and n != "__dsp_pad_end"]
    bad = [n for n in there if not n.startswith("f_") and n != "__dsp_pad"]
    if bad:
        sys.exit(f"dspcheck: {', '.join(bad)} at 0x81000000, where libogc's start-up puts the DSP boot code "
                 f"(see tools/wii/dspcheck.py)")
    print(f"dspcheck: {', '.join(there) or 'nothing'} at 0x81000000")


if __name__ == "__main__":
    main(sys.argv[1:])
