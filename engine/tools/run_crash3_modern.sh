#!/usr/bin/env bash
set -euo pipefail

repo_root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)

# Production presentation: the expensive exhaustive spatial-match verifier is
# reserved for the checkpoint matrix.  Exact direct/indexed admission remains
# active here.  The guest/software oracle stays native 1x; only Vulkan output
# and the Vulkan-generated temporal frames use the 4x surface.
exec env \
  PS1_CRASH3_VULKAN_RESIDENT_MODE=debug \
  PS1_CRASH3_VULKAN_RESIDENT_SHADOW_REPLACE=1 \
  PS1_CRASH3_VULKAN_RESIDENT_SEMI_REPLACE=1 \
  PS1_CRASH3_VULKAN_OBJECT_MODE=debug \
  PS1_CRASH3_VULKAN_OBJECT_PRESENT=1 \
  PS1_CRASH3_VULKAN_SCENE_SIDECAR=1 \
  PS1_CRASH3_VULKAN_SIDECAR_DEPTH=intrinsic \
  PS1_CRASH3_VULKAN_OBJECT_REPLACE=1 \
  PS1_CRASH3_VULKAN_OBJECT_SEMI_REPLACE=1 \
  PS1_CRASH3_VULKAN_RESIDENT_GPU_OPAQUE_BATCH=1 \
  PS1_CRASH3_VULKAN_GPU_RAW_SCENE=1 \
  PS1_CRASH3_VULKAN_GPU_DIRECT_MATERIAL=1 \
  PS1_CRASH3_VULKAN_GPU_SEMI_BATCH=1 \
  PS1_CRASH3_VULKAN_GPU_RAW_SEMI=1 \
  PS1_CRASH3_VULKAN_CULL_STRATEGY=guard \
  PS1_CRASH3_OBJECT_MATCH_VERIFY=0 \
  PS1_CRASH3_VULKAN_VERTEX_INTERPOLATION=2 \
  PS1_INTERNAL_RES_SCALE=4 \
  PS1_PRESENT_VSYNC=1 \
  "$repo_root/build/ps1Runtime/ps1Runtime" \
  --config "$repo_root/crash3_config.toml" "$@"
