#!/usr/bin/env bash
set -euo pipefail
project_root="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$project_root"
# Immutable asset storage is loaded once; scene replacement remains opt-in
# until its pose/material/packet provenance is certified.
if [[ -z "${PS1_MGS_ASSET_CACHE+x}" && -f data/presentation/usa/assets.mgsc ]]; then
    export PS1_MGS_ASSET_CACHE=data/presentation/usa/assets.mgsc
fi
exec build/ps1Runtime/ps1Runtime --config data/mgs.usa.native.toml "$@"
