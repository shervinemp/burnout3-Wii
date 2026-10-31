"""Disassembler for PS2 IOP modules (.IRX) with import names and string hints.

    python -I tools/iop/irxdis.py MODULE.IRX [--from HEX] [--count N] [--imports]

IRX files are MIPS I ELF images (e_type 0xFF80) linked at address 0. Calls to
other modules go through import stubs: a table that starts with the magic
0x41E00000, names the library, and is followed by `jr ra; addiu zero,zero,N`
pairs, where N is the export ordinal. Calls to stubs are shown by name.

The export names and IRX layout follow ps2sdk's headers; no ps2sdk source is
included. See CREDITS.md.
"""
import argparse
import os
import struct
import sys

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "recomp"))
from r5900 import decode, format_insn, GPR_NAMES  # noqa: E402

EXPORTS = {
    "sysmem": {4: "AllocSysMemory", 5: "FreeSysMemory", 6: "QueryMemSize", 7: "QueryMaxFreeMemSize",
               8: "QueryTotalFreeMemSize", 9: "QueryBlockTopAddress", 10: "QueryBlockSize", 14: "Kprintf"},
    "loadcore": {4: "FlushIcache", 5: "FlushDcache", 6: "RegisterLibraryEntries", 7: "ReleaseLibraryEntries",
                 12: "QueryBootMode"},
    "intrman": {4: "RegisterIntrHandler", 5: "ReleaseIntrHandler", 6: "EnableIntr", 7: "DisableIntr",
                8: "CpuDisableIntr", 9: "CpuEnableIntr", 17: "CpuSuspendIntr", 18: "CpuResumeIntr",
                23: "QueryIntrContext", 24: "QueryIntrStack", 25: "iCatchMultiIntr"},
    "sifcmd": {4: "sceSifInitCmd", 5: "sceSifExitCmd", 6: "sceSifGetSreg", 7: "sceSifSetSreg",
               8: "sceSifSetCmdBuffer", 9: "sceSifSetSysCmdBuffer", 10: "sceSifAddCmdHandler",
               11: "sceSifRemoveCmdHandler", 12: "sceSifSendCmd", 13: "isceSifSendCmd",
               14: "sceSifInitRpc", 15: "sceSifBindRpc", 16: "sceSifCallRpc", 17: "sceSifRegisterRpc",
               18: "sceSifCheckStatRpc", 19: "sceSifSetRpcQueue", 20: "sceSifGetNextRequest",
               21: "sceSifExecRequest", 22: "sceSifRpcLoop", 23: "sceSifGetOtherData",
               24: "sceSifRemoveRpc", 25: "sceSifRemoveRpcQueue", 28: "sceSifSendCmdIntr",
               29: "isceSifSendCmdIntr"},
    "sifman": {4: "sceSifDma?4", 5: "sceSifInit", 6: "sceSifSetDChain", 7: "sceSifSetDma",
               8: "sceSifDmaStat", 9: "sceSifSetOneDma", 12: "sceSifCheckInit", 13: "sceSifSetDmaIntr"},
    "thbase": {4: "CreateThread", 5: "DeleteThread", 6: "StartThread", 7: "StartThreadArgs",
               8: "ExitThread", 9: "ExitDeleteThread", 10: "TerminateThread", 11: "iTerminateThread",
               12: "DisableDispatchThread", 13: "EnableDispatchThread", 14: "ChangeThreadPriority",
               15: "iChangeThreadPriority", 16: "RotateThreadReadyQueue", 17: "iRotateThreadReadyQueue",
               18: "ReleaseWaitThread", 19: "iReleaseWaitThread", 20: "GetThreadId", 21: "CheckThreadStack",
               22: "ReferThreadStatus", 23: "iReferThreadStatus", 24: "SleepThread", 25: "WakeupThread",
               26: "iWakeupThread", 27: "CancelWakeupThread", 28: "iCancelWakeupThread",
               29: "SuspendThread", 30: "iSuspendThread", 31: "ResumeThread", 32: "iResumeThread",
               33: "DelayThread", 34: "GetSystemTime", 35: "SetAlarm", 36: "iSetAlarm",
               37: "CancelAlarm", 38: "iCancelAlarm", 39: "USec2SysClock", 40: "SysClock2USec",
               41: "GetSystemStatusFlag", 42: "GetThreadCurrentPriority", 43: "GetSystemTimeLow"},
    "thsemap": {4: "CreateSema", 5: "DeleteSema", 6: "SignalSema", 7: "iSignalSema", 8: "WaitSema",
                9: "PollSema", 10: "ReferSemaStatus", 11: "iReferSemaStatus"},
    "thevent": {4: "CreateEventFlag", 5: "DeleteEventFlag", 6: "SetEventFlag", 7: "iSetEventFlag",
                8: "ClearEventFlag", 9: "iClearEventFlag", 10: "WaitEventFlag", 11: "PollEventFlag",
                12: "ReferEventFlagStatus", 13: "iReferEventFlagStatus"},
    "thmsgbx": {4: "CreateMbx", 5: "DeleteMbx", 6: "SendMbx", 7: "iSendMbx", 8: "ReceiveMbx",
                9: "PollMbx"},
    "stdio": {4: "printf", 5: "getchar", 6: "putchar", 7: "puts"},
    "sysclib": {4: "setjmp", 5: "longjmp", 6: "toupper", 7: "tolower", 8: "look_ctype_table",
                9: "get_ctype_table", 10: "memchr", 11: "memcmp", 12: "memcpy", 13: "memmove",
                14: "memset", 15: "bcmp", 16: "bcopy", 17: "bzero", 18: "prnt", 19: "sprintf",
                20: "strcat", 21: "strchr", 22: "strcmp", 23: "strcpy", 24: "strcspn", 25: "index",
                26: "rindex", 27: "strlen", 28: "strncat", 29: "strncmp", 30: "strncpy", 31: "strpbrk",
                32: "strrchr", 33: "strspn", 34: "strstr", 35: "strtok", 36: "strtol", 37: "atob",
                38: "strtoul", 40: "wmemcopy", 41: "wmemset", 42: "vsprintf"},
    "ioman": {4: "open", 5: "close", 6: "read", 7: "write", 8: "lseek", 9: "ioctl", 10: "remove",
              11: "mkdir", 12: "rmdir", 13: "dopen", 14: "dclose", 15: "dread", 16: "getstat",
              17: "chstat", 18: "format", 20: "AddDrv", 21: "DelDrv"},
    "libsd": {2: "sceSdQuit?", 4: "sceSdInit", 5: "sceSdSetParam", 6: "sceSdGetParam",
              7: "sceSdSetSwitch", 8: "sceSdGetSwitch", 9: "sceSdSetAddr", 10: "sceSdGetAddr",
              11: "sceSdSetCoreAttr", 12: "sceSdGetCoreAttr", 13: "sceSdNote2Pitch",
              14: "sceSdPitch2Note", 15: "sceSdProcBatch", 16: "sceSdProcBatchEx",
              17: "sceSdVoiceTrans", 18: "sceSdBlockTrans", 19: "sceSdVoiceTransStatus",
              20: "sceSdBlockTransStatus", 21: "sceSdSetTransCallback", 22: "sceSdSetIRQCallback",
              23: "sceSdSetEffectAttr", 24: "sceSdGetEffectAttr", 25: "sceSdClearEffectWorkArea",
              26: "sceSdSetTransIntrHandler", 27: "sceSdSetSpu2IntrHandler"},
    "cdvdman": {4: "sceCdInit", 5: "sceCdStandby", 6: "sceCdRead", 7: "sceCdSeek",
                8: "sceCdGetError", 9: "sceCdGetToc", 10: "sceCdSearchFile", 11: "sceCdSync",
                12: "sceCdGetDiskType", 13: "sceCdDiskReady", 14: "sceCdTrayReq", 15: "sceCdStop",
                16: "sceCdPosToInt", 17: "sceCdIntToPos", 21: "sceCdCheckCmd", 22: "sceCdReadILinkID",
                24: "sceCdReadClock", 28: "sceCdStatus", 29: "sceCdApplySCmd", 37: "sceCdCallback",
                38: "sceCdPause", 39: "sceCdBreak", 40: "sceCdReadCDDA", 44: "sceCdGetReadPos",
                45: "sceCdCtrlADout", 46: "sceCdNop", 47: "sceCdStInit", 48: "sceCdStRead",
                49: "sceCdStSeek", 50: "sceCdStStart", 51: "sceCdStStat", 52: "sceCdStStop",
                54: "sceCdRead0", 56: "sceCdRM", 57: "sceCdWM", 58: "sceCdReadChain",
                59: "sceCdStPause", 60: "sceCdStResume", 74: "sceCdPowerOff", 75: "sceCdMmode"},
}


class Irx:
    def __init__(self, path):
        d = self.data = open(path, "rb").read()
        if d[:4] != b"\x7fELF":
            raise SystemExit("not an ELF file")
        phoff, = struct.unpack_from("<I", d, 28)
        phnum, = struct.unpack_from("<H", d, 44)
        self.entry, = struct.unpack_from("<I", d, 24)
        self.image = b""
        for k in range(phnum):
            p_type, off, vaddr, _pa, filesz, memsz, _fl, _al = struct.unpack_from("<8I", d, phoff + 32 * k)
            if p_type == 1:
                self.image = bytearray(memsz)
                self.image[vaddr:vaddr + filesz] = d[off:off + filesz]
        shoff, = struct.unpack_from("<I", d, 32)
        shnum, shstr = struct.unpack_from("<HH", d, 48)
        secs = [struct.unpack_from("<10I", d, shoff + 40 * k) for k in range(shnum)]
        names = secs[shstr][4]
        self.sections = {}
        for s in secs:
            name = d[names + s[0]:d.index(b"\0", names + s[0])].decode()
            self.sections[name] = (s[3], s[5])
        self.text = self.sections.get(".text", (0, len(self.image)))
        self.imports = {}       # stub address -> "lib:Name"
        self._find_imports()

    def u32(self, a):
        return struct.unpack_from("<I", self.image, a)[0]

    def _find_imports(self):
        base, size = self.text
        a = base
        while a + 20 <= base + size:
            if self.u32(a) == 0x41E00000:
                lib = bytes(self.image[a + 12:a + 20]).split(b"\0")[0].decode("latin-1")
                p = a + 20
                while p + 8 <= base + size and self.u32(p) == 0x03E00008:
                    n = self.u32(p + 4) & 0xFFFF
                    name = EXPORTS.get(lib, {}).get(n, f"#{n}")
                    self.imports[p] = f"{lib}:{name}"
                    p += 8
                a = p
            else:
                a += 4

    def string_at(self, a):
        if not (0 <= a < len(self.image)):
            return None
        end = a
        while end < len(self.image) and self.image[end] and end - a < 120:
            c = self.image[end]
            if c < 9 or (13 < c < 32) or c > 126:
                return None
            end += 1
        if end - a < 3 or end >= len(self.image) or self.image[end]:
            return None
        return bytes(self.image[a:end]).decode("latin-1")

    def disasm(self, start, count):
        hi = {}
        out = []
        for k in range(count):
            pc = start + 4 * k
            if pc + 4 > len(self.image):
                break
            w = self.u32(pc)
            i = decode(w, pc)
            text = format_insn(i)
            note = ""
            if i.op == "jal" and i.target in self.imports:
                note = self.imports[i.target]
            elif pc in self.imports:
                note = "stub " + self.imports[pc]
            if i.op == "lui":
                hi[i.rt] = i.imm << 16
            elif i.op in ("addiu", "ori") and i.rs in hi:
                v = (hi[i.rs] + (i.simm if i.op == "addiu" else i.imm)) & 0xFFFFFFFF
                s = self.string_at(v)
                note = f"= 0x{v:X}" + (f' "{s}"' if s else "")
            elif i.word >> 26 >= 0x20 and i.rs in hi:
                v = (hi[i.rs] + i.simm) & 0xFFFFFFFF
                note = f"[0x{v:X}]"
            out.append(f"{pc:06X}: {w:08X}  {text:<34} {('; ' + note) if note else ''}".rstrip())
        return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("irx")
    ap.add_argument("--from", dest="start", default=None)
    ap.add_argument("--count", type=int, default=None)
    ap.add_argument("--imports", action="store_true")
    a = ap.parse_args()
    m = Irx(a.irx)
    if a.imports:
        print(f"entry 0x{m.entry:X}; sections {m.sections}")
        for addr, name in sorted(m.imports.items()):
            print(f"{addr:06X} {name}")
        return
    base, size = m.text
    start = int(a.start, 16) if a.start else base
    count = a.count if a.count else (base + size - start) // 4
    print("\n".join(m.disasm(start, count)))


if __name__ == "__main__":
    main()
