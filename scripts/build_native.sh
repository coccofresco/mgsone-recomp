#!/usr/bin/env bash
set -euo pipefail
project_root="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$project_root"
if [[ $# -ne 1 ]]; then echo "Usage: $0 /path/to/USA-v1.0-disc1.cue" >&2; exit 2; fi
mkdir -p .local
cmake -S engine -B build -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DPS1RECOMP_USE_STUB=ON -DPS1RECOMP_DIAGNOSTIC_FAST_COMPILE=ON
cmake --build build --target ps1Analyzer ps1Recomp --parallel "${BUILD_JOBS:-4}"
python3 scripts/prepare_native.py --campaign --cue "$1"
build/ps1Recomp/ps1Recomp data/mgs.usa.native.toml engine/ps1Runtime/src/recompiled_out.cpp
python3 scripts/install_decomp_bridges.py
cmake -S engine -B build -G Ninja -DPS1RECOMP_USE_STUB=OFF
cmake --build build --target ps1Runtime --parallel "${BUILD_JOBS:-4}"
