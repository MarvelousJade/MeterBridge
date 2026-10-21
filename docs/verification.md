# Verification and reproduction

## Portable storage checks

These build the actual Reading/Store implementation with real SQLite, without
POSIX transport, HTTP or MQTT. They do **not** run the Linux gateway.

On Linux with libsqlite3-dev, CMake, Ninja and a C++20 compiler:

```sh
sh scripts/check-storage.sh
```

On Windows Git Bash with MinGW GCC, CMake, Ninja, curl and unzip, the exact
local dependency preparation used on 2026-10-21 was (adjust the compiler bin
path to your installation; it must precede Git's bundled MinGW DLLs):

```sh
export PATH=/c/ProgramData/mingw64/mingw64/bin:$PATH
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
SQLite 3.54.1, Ninja 1.13.2. Warnings treated as errors for project code.

| Source state | Command/check | Result |
|---|---|---|
| a1cdda7 | `wsl --exec sh -lc 'uname -a'` | Blocked: WSL has no installed distributions. Docker/Podman also unavailable. Existing binaries are Linux ELF. |
| e82f808 extraction | command above | CTest 1 passed, 0 failed, 0 skipped; one baseline scenario, 0.24 s. |
| e82f808 + commit-failure test | same command | 1 passed, 1 failed; caller ID 2 instead of 777. [Log](evidence/commit-failure-red.txt). |
| 1a53612 repair | same command | 2 passed, 0 failed, 0 skipped; 0.11 s. [Log](evidence/commit-failure-green.txt). |
| 0abaaf8 intentional rollback fault + observer assertion | targeted partial_prune | 0 passed, 1 failed; writer sees 2 rows, observer 3. [Log](evidence/rollback-red.txt). |
| 834c572 isolated repair | all storage tests in isolated build | 3 passed, 0 failed, 0 skipped. [Log](evidence/rollback-green.txt). |
| 3062324 + observer/capacity tests | native command | 3 passed, 1 failed; invalid constructor accepted. [Log](evidence/capacity-red.txt). |
| 4691e65 capacity repair | native command | 4 passed, 0 failed, 0 skipped. [Log](evidence/capacity-green.txt). |
| 1244d72 expanded storage regressions | native command | 7 passed, 0 failed, 0 skipped; 1.10 s. [Log](evidence/storage-suite.txt). |
| 1244d7252ce581e4445b1cb6396bd11edd411d77 | fresh build-native-clean with explicit SQLite paths | 7 passed, 0 failed, 0 skipped; 1.09 s test time, 3.0 s configure. [Environment/build/test log](evidence/final-native.txt). |
| 1244d72 | five direct storage CLI demonstrations from walkthrough | All returned PASS. Real constraint/insert/lock rejection messages and restored history observed. [Output](evidence/storage-walkthrough.txt). |

Do not combine run counts. Logs identify the exact commands; source state in the
table identifies uncommitted test overlays when relevant. The README's September Linux/sanitizer claims are historical
repository evidence, not checks performed in this release.

No mock SQLite was used. Reopen tests create real files; commit rejection uses a
scoped SQLite commit hook; insert rejection uses a disposable SQL trigger; lock
contention uses another real SQLite connection. Tests inspect observable IDs,
measurements, counts and order, not query strings. There is no measured coverage
percentage, gateway UI automation, rendered visual inspection or human interaction
recorded for this release.

A fresh reproduction can replace `BUILD_DIR=build-native` above with
`BUILD_DIR=build-native-clean`. Both use the same verified local dependency.
Targeted demonstration commands are in [the walkthrough](walkthrough.md).

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
