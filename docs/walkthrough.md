# Feature walkthrough

## Gateway workflow (Linux)

This is a reproducible operator walkthrough, **not a claim that this session ran
it**. The current Windows host has no Linux runtime. The earlier README records
historical Linux validation; rerun these steps for the current source.

1. Follow README dependency installation, then `sh scripts/check.sh`.
   Expect the core, seven storage scenarios and Python integration suite to pass.
2. Run `sh scripts/demo.sh`. It starts the included synthetic meter and gateway,
   waits for healthy storage, prints health/readings, and reports its disposable
   database/log directory. The first values should be 230 V, 5 A and 1150 W.
3. In another terminal, use:
   ```sh
   curl -i http://127.0.0.1:8080/health
   curl 'http://127.0.0.1:8080/readings?limit=3'
   curl -i 'http://127.0.0.1:8080/readings?limit=0'
   ```
   Expect 200 with fresh samples, newest-first JSON history, and 400 JSON for the
   invalid limit. Repeat the valid query; it must not change data or ordering.
4. Ctrl+C stops the demo processes and leaves the printed logs/database intact.
   Do not delete that directory if you want to inspect the saved output.

For explicit failure/restart demonstration, use separate terminals and a new
state directory (not an existing production database):

```sh
state=$(mktemp -d)
./build/meter-sim
# Another terminal; substitute the printed/selected state path:
./build/meterbridge --db /your/disposable/path/readings.db --max-rows 10
```

Stop the simulator with Ctrl+C. `/health` should change to 503, poll_errors should
increase, and saved `/readings` should remain accessible. Restart `meter-sim`;
health should recover and IDs increase. Restart the gateway with the same DB;
old samples remain until retention evicts them. Before any meter sample arrives,
`/readings` is `[]` and health is degraded rather than inventing zero measurements.

For MQTT, run local `mosquitto -p 1883`, subscribe with
`mosquitto_sub -t meterbridge/readings -q 1`, then enable
`--mqtt-host 127.0.0.1` on a fresh gateway. Stop the broker: pending grows and health
is degraded. Restart it: pending drains after PUBACK. Duplicate deliveries are
allowed; consumers deduplicate IDs **within that database**, not across devices.
At a full pending outbox, polling rejects new samples instead of deleting unsent
ones. This is intentional backpressure, not a lossless infinite buffer.

There is no graphical navigation or rendered UI to inspect. Interaction is CLI,
HTTP JSON and MQTT; no human or visual inspection is claimed for this release.

## Portable failure demonstrations (performed programmatically)

Follow [verification](verification.md) to build storage-only on Windows or Linux.
Then run (Windows binaries accept the same arguments):

```sh
./build-native/storage-tests commit_failure
./build-native/storage-tests partial_prune
./build-native/storage-tests insert_failure
./build-native/storage-tests reopen
./build-native/storage-tests lock_recovery
```

Expected: each ends in `PASS`. Commit rejection prints a SQLite constraint error
but caller ID remains **777**. Partial-prune rejection prints **3** rows from both
connections. Insert rejection recovers after its disposable trigger is removed.
Reopen proves pending replay and boundary-value persistence; lock recovery prints
a database-locked error then succeeds after releasing the competing writer.

These execute production storage code and real SQLite, not a mocked gateway.
They delete their temporary databases automatically and do not export user data.
Historical red logs remain in `docs/evidence/`; do not check out the intentional
fault on main to demonstrate it.
