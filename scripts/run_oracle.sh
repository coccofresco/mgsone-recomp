#!/usr/bin/env bash
set -euo pipefail
project_root="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
if [[ $# -lt 1 ]]; then
  echo "Usage: $0 /absolute/path/disc.cue [frames=5000] [core option=value ...]" >&2
  exit 2
fi
disc_path="$(realpath -- "$1")"
frames="${2:-5000}"
shift
if [[ $# -gt 0 ]]; then shift; fi
cd "$project_root"
export PS1_DONOR_WORK_DIRECTORY="${PS1_DONOR_WORK_DIRECTORY:-$project_root/data/oracle/work}"
exec .local/libretro_headless .local/swanstation/swanstation_libretro.so \
  "$disc_path" "$frames" \
  swanstation_GPU_Renderer=Software \
  swanstation_CPU_ExecutionMode=Interpreter \
  swanstation_BIOS_PatchFastBoot=true \
  "swanstation_CPU_Overclock=${MGS_ORACLE_CPU_PERCENT:-165}" "$@"
