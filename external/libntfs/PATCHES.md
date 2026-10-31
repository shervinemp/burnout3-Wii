# libntfs for this port

Source: https://github.com/extremscorner/libntfs, commit
`f73a4d1ff9e2d6bd8fdc7c8caa3a74df74803103` (2026-09-29, NTFS-3G 2026.9.28).
Licence: GNU GPL v2 (see `LICENSE`). The library is vendored so the Wii build
is reproducible without a devkitPro package for it.

The upstream source is written for libogc2/devkitRice. This copy builds with
the standard devkitPPC/libogc 3.1 used here. The changes are only the
interface between the two libogc variants:

- `source/types.h`: include `<gcbool.h>` (standard libogc's `BOOL`, `TRUE`
  and `FALSE`); define `SYS_IsDMAAddress(p, a)` to 0 (libogc2's DMA-range
  test; the cache path is used instead).
- `source/cache2.c`: `DISC_INTERFACE` callbacks without the libogc2
  `interface` argument; `SYS_IsDMAAddress` as above; `DISC_INTERFACE::flush`
  (not in standard libogc) is a no-op.
- `source/gekko_io.c`: the same callback signatures and no `flush`.
- `source/ntfs.c`: the same callback signatures.
- `source/ea.c`: `makedev` replaced by standard newlib's `mkdev`.
- `source/ntfs.c`: `ntfsFindGPT` added. Upstream only scanned MBR and
  extended partitions; a GPT disk's protective MBR (type 0xEE) is now read as
  a GPT partition table, and Microsoft basic-data partitions with an NTFS boot
  sector are returned like any other. This covers large USB drives formatted
  by current Windows.
- `source/ntfsinternal.c`: the volume mutex is recursive. Several paths take
  it and then call a helper that takes it again, e.g. `ntfs_stat_r` ->
  `ntfsCloseEntry`; with a non-recursive mutex the first `stat` on the mount
  never returns. This was the boot hang at `storage: looking in
  ntfs0:/burnout3`.

No NTFS logic was changed otherwise. `ntfsMountAll` names the volumes
`ntfs0`, `ntfs1`, …, and each is a devoptab, so the existing
`fopen`/`mkdir`/`stat` code works on it once `platform/wii/plat_wii.c` adds
the mount to its data roots.
