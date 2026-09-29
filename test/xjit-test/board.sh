#!/bin/bash
# Run the built xjit-test on the board without touching the emulators: it goes into the
# opentyrian-go partition through the launcher's SD update, runs once (XJIT lines on the USB
# console) and reboots into the launcher; then the original opentyrian-go image goes back.
#   ./board.sh               (from retro-go/xjit-test, after ./docker.sh "idf.py build")
set -e
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
CTL="python3 $ROOT/scripts/board_ctl.py"
ORIG=$ROOT/retro-go/opentyrian-go/build/opentyrian-go.bin
[ -f "$ORIG" ] || { echo "no $ORIG to restore afterwards: build opentyrian-go first"; exit 1; }
cd "$ROOT"
update() {   # update <image>: into the opentyrian-go partition
  timeout 30 $CTL launcher >/dev/null 2>&1 || true; sleep 10
  timeout 120 $CTL put "$1" /sd/retro-go/update/opentyrian-go.bin | grep "put done"
  timeout 30 $CTL launcher >/dev/null 2>&1 || true; sleep 25
  timeout 20 $CTL ls /sd/retro-go/update | grep opentyrian
}
update retro-go/xjit-test/build/xjit-test.bin
timeout 200 python3 - <<'PY' | grep -a -E "XJIT|Guru|PC  |Backtrace"
import sys, time
sys.path.insert(0, 'scripts')
from board_ctl import Board
b = Board('/dev/ttyACM0')
b.send("launch opentyrian-go opentyrian-go /sd/xjit", timeout=3, echo=False)
t0 = time.time()
while time.time() - t0 < 180:
    l = b.readline()
    if l:
        print(l.strip(), flush=True)
        if "XJIT BENCH done" in l or "Rebooting" in l: break
PY
sleep 8
update "$ORIG"
echo "opentyrian-go restored"
