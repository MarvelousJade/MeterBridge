#!/bin/sh
set -eu
root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
build=${BUILD_DIR:-"$root/build"}
cmake -S "$root" -B "$build" -G Ninja -DCMAKE_BUILD_TYPE=Debug "$@"
cmake --build "$build" --parallel 2
ctest --test-dir "$build" --output-on-failure
