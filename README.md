# MeterBridge

C++20 energy-meter gateway for Linux. Poll a Modbus meter, decode its registers,
and retain readings in SQLite. Includes a deterministic meter simulator.

```sh
cmake -S . -B build -G Ninja
cmake --build build
ctest --test-dir build --output-on-failure
./build/meter-sim
# Another terminal:
./build/meterbridge
```

Dependencies: CMake, a C++20 compiler, pkg-config, libmodbus and SQLite development files.

The fixture uses holding registers 0-5: voltage in 0.1 V, current in 0.01 A,
signed 32-bit power in W, and unsigned 32-bit energy in Wh. Multi-register values
store the high word first. This is a project register map, not an Accuenergy map.

Use `--serial /dev/ttyUSB0 --baud 9600 --unit 1` for RTU (8N1).
The simulator serves one client and synthetic readings. Hardware is not yet verified.
