#!/usr/bin/env bash
set -euo pipefail

preset="${1:-debug}"

cmake --preset "${preset}"
cmake --build --preset "${preset}" --parallel

if [[ "${preset}" != "release" ]]; then
    ctest --preset "${preset}"
fi

