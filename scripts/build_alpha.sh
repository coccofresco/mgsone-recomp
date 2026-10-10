#!/usr/bin/env bash
# Private source build. Retail inputs and generated translations stay ignored.
set -euo pipefail
project_root="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$project_root"
if [[ $# -ne 2 ]]; then
  echo "Usage: $0 /path/to/USA-v1.0-disc1.cue /path/to/USA-v1.0-disc2.cue" >&2
  exit 2
fi
mkdir -p .local data
alpha_build_profile="$project_root/data/alpha-build"
if [[ -f "$alpha_build_profile/installation.json" ]]; then
  echo "Reusing verified USA v1.0 build import."
  python3 scripts/mgs_alpha.py --user-dir "$alpha_build_profile" verify
else
  python3 scripts/mgs_alpha.py --user-dir "$alpha_build_profile" import "$1" "$2"
fi
alpha_media_root="$(python3 - <<'PY'
import sys
sys.path.insert(0, 'scripts')
from mgs_alpha import ROOT, installed, user_paths
print(installed(user_paths(override=ROOT/'data/alpha-build'))[0])
PY
)"
cmake -S engine -B build -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DPS1RECOMP_USE_STUB=ON -DPS1RECOMP_DIAGNOSTIC_FAST_COMPILE=ON
cmake --build build --target ps1Analyzer ps1Recomp --parallel "${BUILD_JOBS:-4}"
python3 scripts/prepare_native.py --campaign --exe "$alpha_media_root/SLUS_005.94" \
  --cue "$alpha_media_root/disc1.cue" --disc2-cue "$alpha_media_root/disc2.cue"
build/ps1Recomp/ps1Recomp data/mgs.usa.native.toml engine/ps1Runtime/src/recompiled_out.cpp
python3 scripts/install_decomp_bridges.py --executable "$alpha_media_root/SLUS_005.94"
cmake -S engine -B build -G Ninja -DPS1RECOMP_USE_STUB=OFF
cmake --build build --target ps1Runtime mgsFrontend mgsController_tests ps1Runtime_tests --parallel "${BUILD_JOBS:-4}"
build/ps1Test/mgsController_tests --gtest_brief=1
build/ps1Test/ps1Runtime_tests --gtest_brief=1
python3 -m unittest discover -s scripts/tests
echo "Native build and unit tests complete. Package with scripts/package_alpha.py OUTPUT_DIRECTORY."
