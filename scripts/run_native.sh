#!/usr/bin/env bash
set -euo pipefail
project_root="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$project_root"
# The user-selected UHD cache presentation is the default. Use
# PS1_MGS_MESH_REPLAY=0 for the reference presentation during diagnostics.
export PS1_MGS_MESH_REPLAY="${PS1_MGS_MESH_REPLAY:-1}"
if [[ -z "${PS1_MGS_ASSET_CACHE+x}" && -f data/presentation/usa/assets.mgsc ]]; then
    export PS1_MGS_ASSET_CACHE=data/presentation/usa/assets.mgsc
fi
exec build/ps1Runtime/ps1Runtime --config data/mgs.usa.native.toml "$@"
