"""Print instruction usage over the .text range: python -I tools/recomp/stats.py ELF"""
import collections
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from elf import Image          # noqa: E402
from r5900 import decode       # noqa: E402

TEXT_START, TEXT_END = 0x100000, 0x469E00

img = Image(sys.argv[1])
count = collections.Counter()
unknown = []
for pc in range(TEXT_START, TEXT_END, 4):
    i = decode(img.u32(pc), pc)
    count[i.op] += 1
    if i.op == "unknown":
        unknown.append(i)
for op, n in sorted(count.items(), key=lambda kv: -kv[1]):
    print(f"{n:8d} {op}")
print("unknown words:", len(unknown))
for i in unknown[:40]:
    print(f"  {i.pc:08X} {i.word:08X}")
