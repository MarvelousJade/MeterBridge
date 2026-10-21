# Verification and reproduction

## Portable storage checks

These build the actual Reading/Store implementation with real SQLite, without
POSIX transport, HTTP or MQTT. They do **not** run the Linux gateway.

On Linux with libsqlite3-dev, CMake, Ninja and a C++20 compiler:

```sh
sh scripts/check-storage.sh
```

On Windows Git Bash with MinGW GCC, CMake, Ninja, curl and unzip, the exact
local dependency preparation used on 2026-10-21 was:

```sh
mkdir -p build-native/deps
curl -fL https://www.sqlite.org/2026/sqlite-amalgamation-3540100.zip -o build-native/deps/sqlite.zip
echo '0a9c8e34352dad46006aa82836d5383bff9dc6bc9ce5a60553c05aa30e6ad1f9 *build-native/deps/sqlite.zip' | sha256sum -c -
unzip -q build-native/deps/sqlite.zip -d build-native/deps
gcc -O2 -c build-native/deps/sqlite-amalgamation-3540100/sqlite3.c -o build-native/deps/sqlite3.o
ar rcs build-native/deps/libsqlite3.a build-native/deps/sqlite3.o
BUILD_DIR=build-native sh scripts/check-storage.sh \
  -DSQLite3_INCLUDE_DIR="$PWD/build-native/deps/sqlite-amalgamation-3540100" \
  -DSQLite3_LIBRARY="$PWD/build-native/deps/libsqlite3.a"
```

Downloaded public-domain SQLite and generated artifacts stay in ignored build
folders. A package-manager SQLite can instead be supplied using the same CMake
include/library variables. No SQLite update is forced on deployed Linux systems.

## Run ledger

Environment: Windows x86-64, Git Bash, MinGW GCC 16.1.0 (posix-seh), CMake 4.4.4,
SQLite 3.54.1. Warnings treated as errors for project code.

| Source state | Command/check | Result |
|---|---|---|
| a1cdda7 | `wsl --exec sh -lc 'uname -a'` | Blocked: WSL has no installed distributions. Docker/Podman also unavailable. Existing binaries are Linux ELF. |
| portable storage extraction (next commit after f4306f7) | command above | CTest 1 passed, 0 failed, 0 skipped; one baseline scenario, 0.24 s. |

Do not combine run counts. Subsequent regressions/results are recorded below as
work proceeds. The README's September Linux/sanitizer claims are historical
repository evidence, not checks performed in this release.

## Linux integration gate

```sh
sh scripts/check.sh
BUILD_DIR=build-asan ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 \
  UBSAN_OPTIONS=halt_on_error=1 sh scripts/check.sh -DMETERBRIDGE_SANITIZE=ON
sh scripts/demo.sh
```

These remain required on a Linux host before treating this release as fully
runtime-verified. Native storage success is not TCP/RTU/MQTT or UI verification.
Physical RS485, ARM, power interruption, hosted CI and systemd deployment remain
unverified. No performance benchmark has been run; test duration is not throughput.
