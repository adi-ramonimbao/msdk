#!/bin/bash
# Build this test against an MSDK, flash the board and log the console.
#
# usage: tools/run_release.sh <MAXIM_PATH> <label> [console port]
#   The MSDK's own Examples/<TARGET>/Hello_World/Makefile is used, so the build
#   system matches its Libraries/. The log goes to results/logs/<board name>-<label>.txt.
#   Console port default: /dev/ttyUSB0 (MAX32690EVKIT USB-UART), /dev/ttyACM0 (MAX32655FTHR DAPLink).
#
# Environment: TARGET / BOARD (default: from this folder's location; MAX32655 -> FTHR_Apps_P1,
#   MAX32690 -> EvKit_V1), PYTHON (default python3, with pyserial), OPENOCD (default
#   $MAXIM_PATH/Tools/OpenOCD/bin/openocd). arm-none-eabi-gcc must be on PATH.
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
mp=$(cd "$1" && pwd); label=$2
case $target-$board in
    MAX32655-FTHR_Apps_P1) name=MAX32655FTHR; port=${3:-/dev/ttyACM0} ;;
    MAX32655-*) name=MAX32655EVKIT; port=${3:-/dev/ttyACM0} ;;
    *) name=MAX32690EVKIT; port=${3:-/dev/ttyUSB0} ;;
esac
py=${PYTHON:-python3}
openocd=${OPENOCD:-$mp/Tools/OpenOCD/bin/openocd}
scripts=$(dirname "$openocd")/../scripts
log=$here/results/logs/$name-$label.txt
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
mkdir -p "$(dirname "$log")"
cp "$here/main.c" "$here/project.mk" "$work/"
cp "$mp/Examples/$target/Hello_World/Makefile" "$work/"
make -C "$work" -j"$(nproc)" MAXIM_PATH="$mp" BOARD="$board" BUILD_DIR="$work/build" \
    MSDK_LABEL="$label" > "$work/build.log" 2>&1 || { echo "build failed"; tail "$work/build.log"; exit 1; }
"$py" "$here/tools/console_cap.py" "$port" 150 "$work/console.txt" > /dev/null & cap=$!
sleep 0.3
"$openocd" -s "$scripts" -f interface/cmsis-dap.cfg -f "target/$ocd_target" \
    -c "program $(ls "$work/build"/*.elf | head -1) verify reset exit" > "$work/flash.log" 2>&1
wait $cap
tr -d '\r' < "$work/console.txt" > "$log"
grep -aE "Summary|Example" "$log"
