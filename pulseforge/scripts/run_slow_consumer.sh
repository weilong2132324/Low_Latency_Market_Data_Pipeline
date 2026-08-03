#!/usr/bin/env bash
# run_slow_consumer.sh - demonstrate backpressure and queue-full events.
#
# A deliberately slow worker (--slow-consumer-us 200) consumes far slower
# than the publisher produces, so the bounded mutex queue fills up and
# the receiver must apply backpressure. The monitor shows queue_full and
# queue_depth climbing, then the queue draining after publishing stops.
#
# Usage: bash scripts/run_slow_consumer.sh [PORT]
set -u

cd "$(dirname "$0")/.."
PORT="${1:-9000}"
SHM="/pulseforge_stats"

make >/dev/null

# Clean up any leftovers from a previous interrupted run.
pkill -f 'bin/receiver' 2>/dev/null
pkill -f 'bin/monitor' 2>/dev/null
pkill -f 'bin/publisher' 2>/dev/null
sleep 1

echo "Starting receiver (mutex queue, 1 worker, 200 us/msg slow consumer)..."
./bin/receiver --port "$PORT" --queue mutex --workers 1 \
    --slow-consumer-us 200 --shared-memory-name "$SHM" \
    >/tmp/pf_receiver.log 2>&1 &
RECV_PID=$!

sleep 0.6
echo "Starting monitor..."
./bin/monitor --shared-memory-name "$SHM" >/tmp/pf_monitor.log 2>&1 &
MON_PID=$!

echo "Sending 20,000 ticks at 20,000/s (1 worker can only do ~5,000/s)..."
./bin/publisher --port "$PORT" --rate 20000 --count 20000 --drop-every 5

# Give the worker time to drain the backlog after the publisher stops.
sleep 5
echo "Stopping pipeline..."
kill -INT "$RECV_PID" "$MON_PID" 2>/dev/null || true
wait "$RECV_PID" || true
wait "$MON_PID" || true

echo
echo "=== receiver log ==="
cat /tmp/pf_receiver.log
echo
echo "=== monitor log (last frame, ANSI stripped) ==="
tail -c 4000 /tmp/pf_monitor.log | sed 's/\x1b\[[0-9;]*[A-Za-z]//g' | tail -n 14
