#!/usr/bin/env bash
set -euo pipefail
project_root="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
cmake -S "$project_root/engine" -B "$project_root/build" -G Ninja \
  -DCMAKE_BUILD_TYPE=Release -DPS1RECOMP_USE_STUB=ON -DPS1RECOMP_BUILD_TESTS=ON
cmake --build "$project_root/build" --target ps1Analyzer ps1Recomp ps1Analyzer_tests ps1Recomp_tests --parallel "${BUILD_JOBS:-4}"
"$project_root/build/ps1Test/ps1Analyzer_tests"
"$project_root/build/ps1Test/ps1Recomp_tests"
