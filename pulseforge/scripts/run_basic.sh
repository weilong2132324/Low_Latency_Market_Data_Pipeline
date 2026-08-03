#!/usr/bin/env bash
# run_basic.sh - start the full Phase 2 pipeline:
#   receiver (mutex queue) + monitor + publisher, then tear it down.
#
# Usage: bash scripts/run_basic.sh [PORT]
set -euo pipefail

cd "$(dirname "$0")/.."
PORT="${1:-9000}"
SHM="/pulseforge_stats"

make >/dev/null

echo "Starting receiver (queue=mutex, workers=1)..."
./bin/receiver --port "$PORT" --queue mutex --workers 1 \
    --shared-memory-name "$SHM" >/tmp/pf_receiver.log 2>&1 &
RECV_PID=$!

sleep 0.5
echo "Starting monitor..."
./bin/monitor --shared-memory-name "$SHM" >/tmp/pf_monitor.log 2>&1 &
MON_PID=$!

echo "Sending 10,000 ticks at 10,000/s (dropping every 7th)..."
./bin/publisher --port "$PORT" --rate 10000 --count 10000 --drop-every 7

sleep 1
echo "Stopping pipeline..."
kill -INT "$RECV_PID" "$MON_PID" 2>/dev/null || true
wait "$RECV_PID" || true
wait "$MON_PID" || true

echo
echo "=== receiver log ==="
cat /tmp/pf_receiver.log
echo
echo "=== monitor log (last frame, ANSI stripped) ==="
tail -c 2000 /tmp/pf_monitor.log | sed 's/\x1b\[[0-9;]*[A-Za-z]//g' | tail -n 12
