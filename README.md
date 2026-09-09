# MeterBridge

A compact C++20 energy-meter gateway for Linux: Modbus TCP/RTU, SQLite history,
REST, and MQTT. Two executables: `meterbridge` and a synthetic `meter-sim`.

## Run

On Ubuntu/Debian or Ubuntu WSL, install the build and test dependencies:

```sh
sudo apt-get install cmake ninja-build g++ pkg-config libmodbus-dev libsqlite3-dev libmosquitto-dev mosquitto python3 curl
sh scripts/check.sh
sh scripts/demo.sh
```

The demo starts both processes, prints readings, and stops on Ctrl+C. It retains
its temporary database and logs for inspection. Windows builds run through WSL.
CMake 3.24+ fetches a checksum-pinned cpp-httplib archive on the first build.

For separate terminals:

```sh
./build/meter-sim
./build/meterbridge --db readings.db
curl 'http://127.0.0.1:8080/readings?limit=5'
curl http://127.0.0.1:8080/health
```

To enable MQTT, run `mosquitto -p 1883` and add `--mqtt-host 127.0.0.1` to the
gateway. Subscribe with `mosquitto_sub -t meterbridge/readings -q 1`.
Both executables accept `--help`. RTU uses `--serial /dev/ttyUSB0 --baud 9600 --unit 1` (8N1).

## Behavior

```text
meter -> poller -> SQLite -> REST
                    |
                    +-> MQTT outbox -> PUBACK -> mark sent
```

- One polling worker owns the Modbus connection. Failures retry with 250 ms to
  5 s backoff. TCP connect and response timeouts are 500 ms.
- SQLite writes use transactions, WAL, and FULL synchronous mode. A mutex
  serializes the poller, REST requests, and outbox worker.
- MQTT sends one QoS 1 message at a time. Pending records survive broker outages
  and process restarts. Consumers deduplicate by `id` within a given database.
  PUBACK proves broker receipt, not subscriber processing.
- `--max-rows` defaults to 10000. Only acknowledged/unscheduled history is evicted.
  If pending records fill the limit, new samples are rejected and health degrades.
- REST binds to loopback with two workers and eight queued requests. `/readings`
  returns newest first; `limit` accepts 1-100. `/health` returns 503 for stale data,
  meter/storage failure, or an enabled MQTT connection being offline. It exposes
  sample age, poll duration, accepted samples, errors, and pending records.

The simulator serves one client with 230 V, 5 A, 1150 W, and an elapsed-time energy
counter. Read holding registers with function 03; writes return an exception.

| Zero-based register | Encoding | Unit |
|---|---|---|
| 0 | unsigned 16-bit, divide by 10 | V |
| 1 | unsigned 16-bit, divide by 100 | A |
| 2-3 | signed 32-bit, high word first | W |
| 4-5 | unsigned 32-bit, high word first | Wh |

libmodbus handles wire byte order. This is a project register map; a real meter
needs its manufacturer's map and scaling. The gateway reads measurements rather
than calculating them from electrical waveforms. Use one gateway per database.
MQTT is plaintext and intended for a trusted local lab; no cloud service is required.

## Verification

On September 9, 2026, the core checks and **12 integration scenarios** passed on
x86-64 Linux under WSL, including AddressSanitizer and UndefinedBehaviorSanitizer
with leak detection. Environment: GCC 14.2.1, libmodbus 3.1.12, SQLite 3.49.1,
Mosquitto 2.1.2, and cpp-httplib 0.18.3.
The demo's startup/cleanup, a staged install, and systemd unit syntax also passed.

Scenarios cover wire encoding/exceptions, RTU over pseudo-terminals, meter timeout
and reconnect, wrong transaction IDs, REST validation, retention after restart,
database write failures, full-outbox backpressure, unacknowledged messages, broker
recovery, and durable replay after SIGKILL. Subprocess shutdown failures fail the suite.

```sh
BUILD_DIR=build-asan ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 \
  UBSAN_OPTIONS=halt_on_error=1 sh scripts/check.sh -DMETERBRIDGE_SANITIZE=ON
```

CI defines native x86-64 and ARM64 Linux jobs. Those hosted jobs have not been run.
Physical ARM/RS485 testing and the cross-build/service deployment below remain
unverified. Pseudo-terminals validate RTU framing, not electrical bus behavior.

## Deploy

For ARM64, use a GCC cross-compiler and a complete target sysroot containing the
C/C++ runtime, libmodbus, SQLite, and libmosquitto development files:

```sh
METERBRIDGE_SYSROOT=/path/to/target-sysroot cmake -S . -B build-arm \
  -DCMAKE_TOOLCHAIN_FILE=cmake/aarch64-linux-gnu.cmake -DBUILD_TESTING=OFF
cmake --build build-arm
```

The sysroot must match the target OS. Alternatively, build natively on an ARM
Linux board with the same commands as above.

`sudo cmake --install build` installs both binaries under `/usr/local/bin` by default.
`deploy/meterbridge.service` is an optional systemd unit using a dedicated state
directory. Set its `METER_HOST` environment value for the meter. For RTU, override
`ExecStart` and grant the service the device's serial-group access.

References: [libmodbus](https://libmodbus.org/reference/),
[Mosquitto](https://mosquitto.org/api/files/mosquitto-h.html),
[cpp-httplib](https://github.com/yhirose/cpp-httplib/tree/v0.18.3).
