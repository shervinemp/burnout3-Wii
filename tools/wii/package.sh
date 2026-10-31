#!/bin/sh
# package.sh TARGET [ISO]: the Homebrew Channel app in TARGET/apps/burnout3: boot.dol (the build in build-wii),
# meta.xml, README.txt, and the licence/credit files (LICENSE, NOTICE, THIRD_PARTY_NOTICES.md, CREDITS.md,
# LICENSES/), and with ISO given the game image as burnout3.iso. TARGET: an SD card's root (e.g. /e) or a folder;
# the ELF of the build is kept next to it as burnout3.elf (the profile is read with it: python -I
# work/wprof.py PROF -e ELF). TEST=1 also copies auto.txt (the benchmark and profile switches).
#
# package.sh --split SD_ROOT USB_ROOT [ISO]: the app on the SD card, the data on a USB drive (FAT32 or NTFS).
# boot.dol, meta.xml and README.txt go to SD_ROOT/apps/burnout3; burnout3.iso and burnout3.elf go to
# USB_ROOT/burnout3 (where run.log, saves/ and prof.bin come back; on NTFS the app mounts the drive as ntfs0, ...).
# TEST=1 also puts auto.txt there.
# Do not leave a burnout3.iso in SD_ROOT/burnout3: the game looks there before USB_ROOT/burnout3
# (platform/wii/plat_wii.c, k_roots). The folder itself may stay: the memory card is kept in its saves/.
set -e
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
if [ "$1" = "--split" ]; then
    SD=${2:?usage: package.sh --split SD_ROOT USB_ROOT [ISO]}
    USB=${3:?usage: package.sh --split SD_ROOT USB_ROOT [ISO]}
    ISO=$4
    APP="$SD/apps/burnout3"
    DATA="$USB/burnout3"
    ELF_DIR="$DATA"
else
    TARGET=${1:?usage: package.sh TARGET [ISO], or package.sh --split SD_ROOT USB_ROOT [ISO]}
    ISO=$2
    APP="$TARGET/apps/burnout3"
    DATA="$APP"
    ELF_DIR="$TARGET"
fi
DOL="$ROOT/build-wii/burnout3.dol"
[ -f "$DOL" ] || { echo "no $DOL: build first (make -f Makefile.wii)"; exit 1; }
mkdir -p "$APP" "$DATA"
# Only a folder with the image shadows the USB data (the game skips the others), and sd:/burnout3/saves
# is where the game keeps its memory card: do not tell anyone to remove that.
if [ "$APP" != "$DATA" ] && [ -f "$SD/burnout3/burnout3.iso" ]; then
    echo "warning: $SD/burnout3/burnout3.iso exists; the game reads that before $DATA (remove or rename it)"
fi
cp "$DOL" "$APP/boot.dol"
cp "$ROOT/platform/wii/hbc/meta.xml" "$ROOT/platform/wii/hbc/README.txt" "$APP/"
# The GPL and third-party notices travel with the app.
cp "$ROOT/LICENSE" "$ROOT/NOTICE" "$ROOT/THIRD_PARTY_NOTICES.md" "$ROOT/CREDITS.md" "$APP/"
mkdir -p "$APP/LICENSES"
cp "$ROOT/LICENSES/"* "$APP/LICENSES/"
# auto.txt is the console benchmark's script: only with TEST=1. Normal play
# must not have it (it pauses the game at the race start and runs a profile).
if [ "${TEST:-0}" = 1 ]; then
    cp "$ROOT/platform/wii/hbc/auto.txt" "$DATA/"
elif [ -f "$DATA/auto.txt" ]; then
    echo "warning: $DATA/auto.txt is left from an earlier TEST=1 package (it pauses the game at the race start); delete it to play normally"
fi
cp "$ROOT/build-wii/burnout3.elf" "$ELF_DIR/burnout3.elf"
# A raw 0xFF file is not a formatted PS2 card. Ship the known-good card from
# work/saves when there is one (the game can overwrite its profile).
CARD="$ROOT/work/saves/Mcd001.ps2"
if [ -f "$CARD" ]; then
    mkdir -p "$DATA/saves"
    if [ ! -f "$DATA/saves/Mcd001.ps2" ] || \
       ! head -c 20 "$DATA/saves/Mcd001.ps2" 2>/dev/null | grep -q "Sony PS2 Memory Card"; then
        cp "$CARD" "$DATA/saves/Mcd001.ps2"
    fi
fi
if [ -n "$ISO" ]; then
    # Size alone is not enough: an interrupted copy on NTFS can leave the full
    # size allocated with only part of it valid (a short Vdl), and reads past
    # that point return zeros. Compare the bytes before trusting the file.
    if [ -f "$DATA/burnout3.iso" ] && cmp -s "$ISO" "$DATA/burnout3.iso"; then
        echo "burnout3.iso is identical: not copied again"
    else
        echo "copying the game image (2.9 GB)..."
        rm -f "$DATA/burnout3.iso"
        cp "$ISO" "$DATA/burnout3.iso"
    fi
fi
if [ "$APP" = "$DATA" ]; then
    echo "packaged in $APP:"
    ls -la "$APP"
else
    echo "packaged the app in $APP:"
    ls -la "$APP"
    echo "and the data in $DATA:"
    ls -la "$DATA"
fi
