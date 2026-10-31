Burnout 3 (recompiled) - console test
=====================================

Unofficial, non-commercial fan project. Burnout 3: Takedown is a trademark of
Electronic Arts;
this project is not affiliated with or endorsed by EA or Criterion. No game
assets are included: you must supply your own legally owned copy. Do not
redistribute the ISO or a built DOL.

This app is free software under the GNU GPL v3 or later; see the project's
LICENSE, THIRD_PARTY_NOTICES.md and CREDITS.md. It comes with absolutely no
warranty.

What this is for: the first measurements on a real Wii. Everything measured so
far is Dolphin's, which does not model the caches; this run says where the
time really goes.

The data folder (FAT32) can be next to the app or on a USB drive.

Together on the SD card:

    apps/burnout3/boot.dol
    apps/burnout3/meta.xml
    apps/burnout3/README.txt
    apps/burnout3/LICENSE
    apps/burnout3/NOTICE
    apps/burnout3/THIRD_PARTY_NOTICES.md
    apps/burnout3/CREDITS.md
    apps/burnout3/LICENSES/       GPL-2.0, GPL-3.0 and BSD-3-Clause texts
    apps/burnout3/auto.txt        the test's switches (only with TEST=1; delete it to play normally)
    apps/burnout3/burnout3.iso    your copy of the game (PS2, NTSC-U), 2.9 GB

Or the app on the SD card and the data on USB:

    sd:/apps/burnout3/boot.dol
    sd:/apps/burnout3/meta.xml
    usb:/burnout3/auto.txt        (only with TEST=1)
    usb:/burnout3/burnout3.iso

The app looks for the data in sd:/burnout3, usb:/burnout3, sd:/apps/burnout3,
usb:/apps/burnout3, then the NTFS mounts ntfs0:/burnout3, ntfs1:/burnout3, ...
(the log's "storage: NTFS ntfs0 (...)" line says the name), in that order. A
folder is used only if it contains burnout3.iso: the app's own apps/burnout3
folder without the image is skipped, so a split layout is not shadowed by it.
Do not leave a burnout3.iso in sd:/burnout3 if the data is on USB: it would be
chosen first. The folder itself may stay (the memory card lives in its saves/).

The USB drive may be FAT32 or NTFS. From FAT32 USB the image is read through
libfat; from NTFS it goes through libntfs. Both are slower than the SD card's
direct-sector path, so loads may be slower. auto.txt, args.txt, saves/ and the
logs all belong in the data folder; on NTFS they are written there too. Shut
Windows down fully (not hibernate or Fast Startup) before using an NTFS drive:
libntfs will not mount a hibernated volume.

Saves prefer the SD card: the game keeps its memory card at
sd:/burnout3/saves/Mcd001.ps2. On the first start an existing card in the data
folder is copied there; without a writable SD card it stays in the data
folder. The card must already be a formatted PS2 memory card (Mcd001.ps2 is
8,650,752 bytes); a file of 0xFF bytes is not a formatted card and the game
will not offer to create a profile. If package.sh finds a formatted card in
work/saves/Mcd001.ps2 it copies it; otherwise create one in Dolphin's Memory
Card Manager (8 MB, raw) or use a card formatted on a PS2. The BURNOUT profile
on a supplied card can be overwritten or deleted in the game.

If the game hangs at the end of loading, check that the ISO copy is complete.
An interrupted copy can leave burnout3.iso at the full size while only part of
it is valid; reads past that point return zeros. On Windows, this shows as a
short "Vdl" (valid data length):

    fsutil file layout E:\burnout3\burnout3.iso

Vdl must equal Size. If it is smaller, delete the file and copy the ISO again;
do not trust the size shown by Explorer. The package script now compares the
bytes, so an interrupted image is copied again.

Then:

1. Start "Burnout 3 (recompiled)" from the Homebrew Channel.
   Controller: GameCube pad or Classic Controller (A = cross/select/accelerate,
   B = circle/back/boost, X = square/brake, Y = triangle; on the Classic
   Controller Y = square and X = triangle keep their PS2 positions), or Wii
   Remote + Nunchuk (A = cross, B = circle, 1 = square, 2 = triangle).
   To use the old PS2 face-button positions instead, put "--pad 0" in args.txt
   in the data folder; the default is "--pad 1".
   Frame rate stays adaptive (the game's own 60 fps race mode, with extra
   update passes so game time stays right). If a console cannot hold it,
   "--fps 30" in args.txt uses the game's 30 fps mode (one update pass a
   frame, normal game speed); "--fps 60" forces 60.
   The console draws one car-reflection view in three and holds the rest.
   "--reflect 4" or "--reflect 6" updates them less often; the draw-count
   saving is small, so three stays the default.
   The world's big triangles and the ones across the near plane are cut to
   the screen before the GPU draws them (the PS2's GS did, the Wii's GPU
   does not, and its texture warps over a triangle that big): "--clip 0"
   turns it off, "--clipband N" is how far outside the screen a triangle
   may reach before it is cut, in percent of the screen's size (200; 0 cuts
   everything that leaves it), "--clipevery N" how often a batch with
   nothing to cut is looked at again (every 4th frame; 1 every frame).
   The shadow under a car fades away where the road is in shade (the game's
   own lighting of it: the trees, the bridges, the way into the tunnel).
   The console keeps a faint one there, 35 percent of its full strength:
   "--shadowfloor N" sets that percent (0 is the game's own, none in shade).
   The game's vibration (crashes, takedowns, bumps) goes to the GameCube pad
   and to the Wii Remote (also with a Classic Controller or a Nunchuk on it:
   the motor is the Remote's): "--rumble 0" turns it off. Each has one motor
   that is on or off, so a weaker vibration is the motor on for that share
   of the frames.
   The sound's voices heard dry only are stretched and summed on the Wii's
   DSP by default (the chip is otherwise idle), which takes that work off
   the CPU: "--dsp 0" mixes them on the CPU instead, "--dsp 2" makes and
   stretches their jobs here (the CPU's sound to the bit), "--dsp 3" runs
   each voice both ways and the log says whether the chip's sums differed.
   The picture is 640x448 in a 480-line frame, as on the PS2, so the HUD at
   the bottom is not in the television's overscan. "--tvfit 0" stretches it
   to the frame's height instead (the old way: the HUD's last row then in
   the frame's last line, where a television's overscan cuts it).
   HOME, the console's power button and RESET now flush the save and log and
   leave the game.
2. Go into any race (Single Event > Race is the one Dolphin's test uses) and
   race for at least a minute. During the test the game runs a frame per
   update, so it may look like slow motion: that is the benchmark, not a bug.
3. Switch the Wii off (or go back with the power button) and bring the card
   (and the USB drive, if used) back.

What comes back, in the data folder (apps/burnout3, or usb:/burnout3):

    run.log         the log: a status block every 10 s, a "bench:" line with
                    the race's frame times, "profile:" when the profile is done
    run.log "pmc" lines   the processor's own counters: at the start a self-test ("pmc selftest": each line should say ok),
                    then in every 10 s status block the cache misses and stall cycles of a frame, three kinds in turn
                    (L1 misses, L2 misses, stall cycles); a minute of racing gives each twice.

    prof.bin        where the processor's time went, 5 s right after the
    profstack.bin   benchmark (read with work/wprof.py and the build's ELF)

For a profile of the race alone (not of the ISO load and the menus), put
these two lines in auto.txt in the data folder - the game still plays with
the controller as usual, and the profile covers the 20 s from the game tick
300:

    0 ptstat 1
    300 prof 20

"ptstat 1" also prints "pmc by part" lines in the status blocks (the
processor's IPC and cache misses in each part of the frame: EE, VIF, GIF, GX,
NAT ...), and it fills in the "ms:" field of the slow-frame lines with the
frame's time split (EE, VU, VIF, GIF, sync - the wait for the GPU - and the
native work): it is what says whether a slow frame was the processor's work
or the GPU's wait.

Widescreen (when you play, not in this test):

    The game follows the Wii's own setting (Wii Settings > Screen > Widescreen
    16:9 or 4:3): on 16:9 it draws a wide picture (wider view, HUD at the
    corners, menus kept in the middle) and the TV stretches it; on 4:3 it is
    the normal picture. To force it, put one line in args.txt in the data
    folder (apps/burnout3/args.txt, or usb:/burnout3/args.txt):

        --widescreen 0        4:3 always
        --widescreen 1        16:9 always
        --widescreen 2        as the Wii is set (what it does without the line)

    The test's auto.txt has "0 widescreen 0", so its numbers are the 4:3 game's.

If it hangs or shows an error, run.log still has what happened up to then
(it is written out every 10 s); a photo of the screen helps too.

Credits
-------

Burnout 3: Takedown was developed by Criterion Games and published by
Electronic Arts. This port is built on work from:

  PCSX2 (GS swizzle tables and hardware references; Aero_'s widescreen patch)
  Dolphin (GX/DSP reference)
  PS2SDK / ps2dev (libsd, libkernel, SIO2/SIF protocol and reverb references)
  extrems / libntfs and NTFS-3G (NTFS support)
  libchdr / MAME (optional CHD conversion)
  Burnout3 decompilation by siddharthakumar-98 (layout reference)
  devkitPro / libogc / AESND (Wii toolchain, runtime and DSP boot protocol)

The full list with links is in CREDITS.md. Thank you to everyone who tested,
measured, and documented the port.
