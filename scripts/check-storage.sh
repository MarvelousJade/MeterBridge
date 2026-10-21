#!/bin/sh
# Portable storage tests only; does not validate the Linux gateway.
set -eu
root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
build=${BUILD_DIR:-"$root/build-storage"}
cmake -S "$root" -B "$build" -G Ninja -DCMAKE_BUILD_TYPE=Debug \
  -DMETERBRIDGE_STORAGE_ONLY=ON -DBUILD_TESTING=ON "$@"
cmake --build "$build" --parallel 2
ctest --test-dir "$build" --output-on-failure
