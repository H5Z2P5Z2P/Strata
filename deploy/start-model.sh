#!/usr/bin/env bash
set -euo pipefail

MODEL="${1:-gyro-m-dflash2}"
shift || true
PORT="8081"
HOST="0.0.0.0"
CHECK_ONLY=0
RESTART=0
while [[ $# -gt 0 ]]; do
  case "$1" in
    --port) PORT="$2"; shift 2 ;;
    --host) HOST="$2"; shift 2 ;;
    --check) CHECK_ONLY=1; shift ;;
    --restart) RESTART=1; shift ;;
    *) echo "Unknown arg: $1"; exit 2 ;;
  esac
done

BASE="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
CONFIG="$BASE/deploy/strata-${MODEL}-0166.json"
LOGDIR="$BASE/deploy/logs"
LOG="$LOGDIR/${MODEL}.server.log"
PY="/home/huaihuai/local-run-ai/Strata-rdna3/.venv/bin/python"

mkdir -p "$LOGDIR"
[[ -x "$PY" && -f "$CONFIG" ]] || { echo "Missing python or config: $CONFIG"; exit 1; }
"$PY" -m json.tool "$CONFIG" >/dev/null

if [[ "$CHECK_ONLY" -eq 1 ]]; then
  echo "Configuration OK: $CONFIG"
  exit 0
fi

exec 9>"$LOGDIR/start.lock"
flock -n 9 || { echo "Another model startup is already in progress."; exit 1; }

if [[ "$RESTART" -eq 1 ]]; then
  bash "$BASE/deploy/stop.sh" --port "$PORT"
fi

if [[ -n "$(ss -H -ltn sport = :"$PORT")" ]]; then
  echo "Port $PORT is occupied. Run '$BASE/deploy/stop.sh --port $PORT' or pass '--restart'."
  exit 1
fi

# Raise current shell's RLIMIT_MEMLOCK to 16 GiB while keeping UID 1000 (huaihuai)
sudo -n /usr/bin/prlimit --pid "$$" --memlock=17179869184:17179869184 2>/dev/null || true

PYTHONPATH="$BASE/terminal${PYTHONPATH:+:$PYTHONPATH}" \
  nohup "$PY" "$BASE/terminal/serve/server.py" --engine strata \
  --config "$CONFIG" --host "$HOST" --port "$PORT" \
  >>"$LOG" 2>&1 </dev/null 9>&- &
pid=$!
echo "$pid" >"$LOGDIR/${MODEL}.pid"
echo "$pid" >"$LOGDIR/server.pid"
echo "$MODEL" >"$LOGDIR/active.model"
echo "Starting $MODEL (PID $pid) on $HOST:$PORT. Log: $LOG"

for ((i=0; i<240; i++)); do
  if ! kill -0 "$pid" 2>/dev/null; then
    echo "Server exited; tail of $LOG:"
    tail -n 40 "$LOG"
    exit 1
  fi
  if "$PY" -c "import json,urllib.request; d=json.load(urllib.request.urlopen('http://127.0.0.1:${PORT}/health',timeout=1)); raise SystemExit(0 if d.get('loaded') and d.get('status')=='ok' else 1)" 2>/dev/null; then
    echo "READY: $MODEL on http://127.0.0.1:${PORT}/v1"
    exit 0
  fi
  sleep 1
done

echo "Startup timed out after 240s; inspect $LOG"
exit 1
