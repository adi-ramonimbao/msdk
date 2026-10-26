#!/bin/bash
# Capture one iteration of each test case with an ADALM2000 and plot it.
#
# usage: tools/capture_cases.sh <MAXIM_PATH> <label> [case ...]
#   MAXIM_PATH  MSDK to build against (needs Libraries/ and Examples/<TARGET>/Hello_World/Makefile)
#   label       output folder name under results/captures/
#   case ...    case numbers (default: 1..14)
#
# Environment: TARGET / BOARD (default: from this folder's location; MAX32655 -> FTHR_Apps_P1,
#   MAX32690 -> EvKit_V1), PYTHON (default python3, with tools/requirements.txt installed),
#   OPENOCD (default $MAXIM_PATH/Tools/OpenOCD/bin/openocd), M2K_URI (default ip:192.168.2.1),
#   M2K_SDA / M2K_SCL (logic analyzer DIO numbers, default 1 / 0).
# arm-none-eabi-gcc must be on PATH. Close Scopy (or disconnect it from the M2K) first.
set -e
here=$(cd "$(dirname "$0")/.." && pwd)
# Target from the example's location (Examples/<TARGET>/I2C_Transaction_Test)
target=${TARGET:-$(basename "$(dirname "$here")")}
case $target in
    MAX32655) board=${BOARD:-FTHR_Apps_P1} ;;
    MAX32690) board=${BOARD:-EvKit_V1} ;;
    *) echo "unsupported target $target"; exit 1 ;;
esac
ocd_target=$(echo "$target" | tr 'A-Z' 'a-z').cfg
mp=$(cd "$1" && pwd); label=$2; shift 2
cases=${*:-$(seq 1 14)}
py=${PYTHON:-python3}
openocd=${OPENOCD:-$mp/Tools/OpenOCD/bin/openocd}
scripts=$(dirname "$openocd")/../scripts
out=$here/results/captures/$label
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
mkdir -p "$out"
cp "$here/main.c" "$here/project.mk" "$work/"
cp "$mp/Examples/$target/Hello_World/Makefile" "$work/"
for n in $cases; do
    f=$(printf "case%02d" "$n")
    make -C "$work" -j"$(nproc)" MAXIM_PATH="$mp" BOARD="$board" BUILD_DIR="$work/build-$n" \
        SCOPE_CASE="$n" > "$work/build-$n.log" 2>&1 || { echo "case $n: build failed"; tail "$work/build-$n.log"; exit 1; }
    "$openocd" -s "$scripts" -f interface/cmsis-dap.cfg -f "target/$ocd_target" \
        -c "program $(ls "$work/build-$n"/*.elf | head -1) verify reset exit" > "$work/flash-$n.log" 2>&1
    # The capture build waits 1.5 s before its first iteration, so arming after flashing is in time.
    "$py" "$here/tools/m2k_cap.py" "$out/$f.npz" --uri "${M2K_URI:-ip:192.168.2.1}" --rate 4e6 \
        --samples 16384 --pretrig 400 --sda "${M2K_SDA:-1}" --scl "${M2K_SCL:-0}" --timeout-ms 8000 > /dev/null
    name=$(grep -o "{ \"[^\"]*\", case_" "$here/main.c" | sed -n "${n}p" | sed 's/{ "\(.*\)", case_/\1/')
    echo "case $n: $("$py" "$here/tools/i2c_plot.py" "$out/$f.npz" "$out/$f.png" \
        --title "Case $n: $name - $label ($target $board)" | grep -v '^plot:\|^(note')"
done
