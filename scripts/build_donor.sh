#!/usr/bin/env bash
set -euo pipefail
project_root="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$project_root"
donor_dir="$project_root/.local/swanstation"
revision=7f69c199ed88d5723f71dd3a6e9c1b7a45b535a6
if [[ ! -d "$donor_dir/.git" ]]; then
  mkdir -p .local
  git clone https://github.com/libretro/swanstation.git "$donor_dir"
  git -C "$donor_dir" checkout --detach "$revision"
  git -C "$donor_dir" apply "$project_root/donor/swanstation-trace.patch"
fi
[[ "$(git -C "$donor_dir" rev-parse HEAD)" == "$revision" ]] || { echo 'Unexpected SwanStation revision' >&2; exit 1; }
git -C "$donor_dir" apply --reverse --check "$project_root/donor/swanstation-trace.patch"
cp donor/donor_pc_memory_snapshot.h "$donor_dir/src/core/donor_pc_memory_snapshot.h"
make -C "$donor_dir" -f Makefile.libretro -j"${BUILD_JOBS:-4}"
"${CXX:-c++}" -std=c++17 -O2 -Wall -Wextra \
  -I"$donor_dir/dep/libretro-common/include" \
  engine/tools/donor_trace/libretro_headless.cpp -ldl -o .local/libretro_headless
sha256sum "$donor_dir/swanstation_libretro.so" .local/libretro_headless > .local/donor-build.sha256
