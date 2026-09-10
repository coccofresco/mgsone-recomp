#!/usr/bin/env bash
set -euo pipefail
project_root="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$project_root"
exec build/ps1Runtime/ps1Runtime --config data/mgs.usa.native.toml "$@"
