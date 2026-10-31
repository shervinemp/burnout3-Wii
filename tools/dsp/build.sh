#!/bin/sh
# build.sh [UCODE [MAIN]]: assemble tools/dsp/UCODE.s (default toy) with gcdsptool into build-dsptest/UCODE.h and build
# tools/dsp/MAIN.c (default dsptest; the mixer's test is "dspmix dspmix_test", it also compiles platform/wii/dsp_mix.c)
# into build-dsptest/MAIN.dol. Run from the devkitPro MSYS2 shell (DEVKITPRO=/opt/devkitpro), e.g.
#   C:/devkitPro/msys2/usr/bin/bash.exe -lc "cd /c/Users/sherv/Desktop/Projects/burnout3-Wii && PYTHON=<python.exe> sh tools/dsp/build.sh dspmix dspmix_test"
set -e
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
export DEVKITPRO=${DEVKITPRO:-/opt/devkitpro}
DEVKITPPC=$DEVKITPRO/devkitPPC
LIBOGC=$DEVKITPRO/libogc
NAME=${1:-toy}
MAIN=${2:-dsptest}
OUT=$ROOT/build-dsptest
mkdir -p "$OUT"
$DEVKITPRO/tools/bin/gcdsptool -c "$ROOT/tools/dsp/$NAME.s" -o "$OUT/$NAME.h"
EXTRA=""
case "$MAIN" in dspmix*) EXTRA="$ROOT/platform/wii/dsp_mix.c" ;; esac
$DEVKITPPC/bin/powerpc-eabi-gcc -O2 -DGEKKO -DHW_RVL -mrvl -mcpu=750 -meabi -mhard-float -Wall \
    -I"$OUT" -I"$ROOT/platform/wii" -I"$LIBOGC/include" -o "$OUT/$MAIN.elf" "$ROOT/tools/dsp/$MAIN.c" $EXTRA -L"$LIBOGC/lib/wii" -logc -lm
$DEVKITPRO/tools/bin/elf2dol "$OUT/$MAIN.elf" "$OUT/$MAIN.dol"
${PYTHON:-python3} -I "$ROOT/tools/wii/dolfix.py" "$OUT/$MAIN.dol"
ls -la "$OUT/$MAIN.dol"
