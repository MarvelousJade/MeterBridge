# MeterBridge

C++20 energy-meter gateway for Linux. Poll Modbus TCP/RTU, keep bounded SQLite
history, serve REST, and publish MQTT with a durable QoS 1 outbox. Includes a meter simulator.

```sh
cmake -S . -B build -G Ninja
cmake --build build
ctest --test-dir build --output-on-failure
./build/meter-sim
# Another terminal:
./build/meterbridge
# Read the latest samples:
curl 'http://127.0.0.1:8080/readings?limit=5'
curl http://127.0.0.1:8080/health
```

Dependencies: CMake, a C++20 compiler, pkg-config, libmodbus, SQLite and libmosquitto
development files. CMake fetches a checksum-pinned cpp-httplib archive.
Tests also require Python 3 and the Mosquitto broker.

Run a local broker with `mosquitto -p 1883`, then add `--mqtt-host 127.0.0.1` to
the gateway. Readings use topic `meterbridge/readings`. SQLite marks a reading
sent only after PUBACK. Delivery is at least once: consumers must deduplicate
by `id` for a given database. An ACK means broker receipt, not subscriber processing.

REST listens on loopback. `/health` returns 503 for stale samples, meter/storage
failure, or an enabled broker connection being offline. `/readings?limit=1..100`
returns newest first. `--max-rows` defaults to 10000; only sent history is evicted.
A full pending outbox rejects new samples and reports an error. MQTT is opt-in.
Use `--help` for all options. Hosts are numeric IPv4; serial uses 8N1.

The fixture uses holding registers 0-5: voltage in 0.1 V, current in 0.01 A,
signed 32-bit power in W, and unsigned 32-bit energy in Wh. Multi-register values
store the high word first. This is a project register map, not an Accuenergy map.

Use `--serial /dev/ttyUSB0 --baud 9600 --unit 1` for RTU (8N1).
The simulator serves one client and synthetic readings. Hardware is not yet verified.
