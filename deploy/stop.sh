#!/usr/bin/env bash
set -euo pipefail

PORT="8081"
while [[ $# -gt 0 ]]; do
  case "$1" in
    --port) PORT="$2"; shift 2 ;;
    *) echo "Unknown arg: $1"; exit 2 ;;
  esac
done

BASE="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
LOGDIR="$BASE/deploy/logs"

PIDS=()
if [[ -f "$LOGDIR/server.pid" ]]; then
  p="$(cat "$LOGDIR/server.pid" 2>/dev/null || true)"
  [[ -n "$p" ]] && PIDS+=("$p")
fi
while IFS= read -r p; do
  [[ -n "$p" ]] && PIDS+=("$p")
done < <(ss -tlnp sport = :"$PORT" 2>/dev/null | grep -oE 'pid=[0-9]+' | cut -d= -f2 | sort -u || true)

if [[ "${#PIDS[@]}" -eq 0 ]]; then
  echo "No Strata server listening on port $PORT."
  exit 0
fi

# Deduplicate and stop parent + children cleanly
UNIQ_PIDS=($(printf '%s\n' "${PIDS[@]}" | sort -u))
echo "Stopping PIDs on port $PORT: ${UNIQ_PIDS[*]}"
for p in "${UNIQ_PIDS[@]}"; do
  pkill -TERM -P "$p" 2>/dev/null || true
  kill -TERM "$p" 2>/dev/null || true
done

for ((i=0; i<20; i++)); do
  if [[ -z "$(ss -H -ltn sport = :"$PORT")" ]]; then
    alive=0
    for p in "${UNIQ_PIDS[@]}"; do
      kill -0 "$p" 2>/dev/null && alive=1
    done
    [[ "$alive" -eq 0 ]] && break
  fi
  sleep 1
done

for p in "${UNIQ_PIDS[@]}"; do
  pkill -KILL -P "$p" 2>/dev/null || true
  kill -KILL "$p" 2>/dev/null || true
done
sleep 1
rm -f "$LOGDIR/server.pid" "$LOGDIR/active.model"
echo "Port $PORT stopped."
