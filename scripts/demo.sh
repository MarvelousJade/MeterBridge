#!/bin/sh
set -eu
root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
build=${BUILD_DIR:-"$root/build"}
state=$(mktemp -d -t meterbridge-demo.XXXXXX)
sim=
gateway=
cleanup() {
    for pid in "$gateway" "$sim"; do
        if [ -n "$pid" ]; then kill "$pid" 2>/dev/null || true; fi
    done
    wait || true
}
trap cleanup EXIT
trap 'exit 130' INT
trap 'exit 143' TERM
"$build/meter-sim" >"$state/simulator.log" 2>&1 &
sim=$!
"$build/meterbridge" --db "$state/readings.db" >"$state/gateway.log" 2>&1 &
gateway=$!
attempt=0
until curl -fsS http://127.0.0.1:8080/health >"$state/health.json" 2>/dev/null; do
    attempt=$((attempt + 1))
    if [ "$attempt" -ge 30 ] || ! kill -0 "$sim" 2>/dev/null || ! kill -0 "$gateway" 2>/dev/null; then
        cat "$state/simulator.log" "$state/gateway.log"
        exit 1
    fi
    sleep 0.2
done
cat "$state/health.json"
printf '\n'
curl -fsS 'http://127.0.0.1:8080/readings?limit=3'
printf '\nDemo running. Ctrl+C stops both processes. Logs and data: %s\n' "$state"
wait "$gateway"
