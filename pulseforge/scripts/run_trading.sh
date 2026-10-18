#!/usr/bin/env bash
# run_trading.sh - run the full trading loop: bin/exchange + bin/trader.
#
# Usage: bash scripts/run_trading.sh [MODE] [SECONDS]
#   MODE    blocking  (default) both sides sleep in epoll_wait
#           spin      both sides busy-poll, pinned to CPUs 1 (exchange) / 2 (trader)
#           gaps      blocking + the exchange drops every 2000th market data
#                     packet, so you can watch gap detection and re-snapshots
#   SECONDS how long the trader trades (default 15)
#
# Prints the trader's tick-to-trade latency report, then the exchange summary.
set -euo pipefail

cd "$(dirname "$0")/.."
MODE="${1:-blocking}"
SECS="${2:-15}"

EX_ARGS=(--quiet)
TR_ARGS=(--duration "$SECS")
case "$MODE" in
    blocking) ;;
    spin) EX_ARGS+=(--spin --cpu 1); TR_ARGS+=(--spin --cpu 2) ;;
    gaps) EX_ARGS+=(--drop-every 2000) ;;
    *) echo "unknown mode: $MODE (blocking | spin | gaps)"; exit 2 ;;
esac

make bin/exchange bin/trader >/dev/null

echo "Starting exchange (${EX_ARGS[*]})..."
./bin/exchange "${EX_ARGS[@]}" >/tmp/pf_exchange.log 2>&1 &
EX_PID=$!
trap 'kill -INT "$EX_PID" 2>/dev/null || true' EXIT
sleep 1

echo "Starting trader (${TR_ARGS[*]})..."
./bin/trader "${TR_ARGS[@]}"

kill -INT "$EX_PID" 2>/dev/null || true
wait "$EX_PID" || true
trap - EXIT
echo
echo "=== exchange log ==="
cat /tmp/pf_exchange.log
