#!/usr/bin/env bash
set -euo pipefail
project_root="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$project_root"
# The user-selected UHD cache presentation is the default. Use
# PS1_MGS_MESH_REPLAY=0 for the reference presentation during diagnostics.
export PS1_MGS_MESH_REPLAY="${PS1_MGS_MESH_REPLAY:-1}"
if [[ -z "${PS1_MGS_ASSET_CACHE+x}" ]]; then
    if [[ -f data/presentation/usa-source/assets.mgsc ]]; then
        export PS1_MGS_ASSET_CACHE=data/presentation/usa-source/assets.mgsc
    elif [[ -f data/presentation/usa/assets.mgsc ]]; then
        export PS1_MGS_ASSET_CACHE=data/presentation/usa/assets.mgsc
    fi
fi
exec "${PS1_NATIVE_RUNTIME:-build/ps1Runtime/ps1Runtime}" --config "${PS1_NATIVE_CONFIG:-data/mgs.usa.native.toml}" "$@"
