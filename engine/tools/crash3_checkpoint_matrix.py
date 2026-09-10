#!/usr/bin/env python3
"""Run reproducible Crash 3 warp, streaming and attract-demo checkpoints.

The committed manifest contains only hashes, guest VBlank metadata and paths
to local save states; game data remains outside Git.  Each run gets a private
diagnostic directory whose slot 0 is copied from the immutable reference state,
so an interactive F5 save can never overwrite the corpus.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import math
import os
import shutil
import subprocess
import sys
from dataclasses import dataclass
from datetime import datetime, timezone
from pathlib import Path
from typing import Any


ROOT = Path(__file__).resolve().parent.parent
DEFAULT_MANIFEST = (
    ROOT / "evidence/renderer/crash3-checkpoint-corpus-20260902.json"
)
SCHEMA = "crash3-checkpoint-corpus/v1"
RESULT_SCHEMA = "crash3-checkpoint-matrix-result/v1"


@dataclass(frozen=True)
class Scenario:
    scenario_id: str
    checkpoint: Path
    checkpoint_sha256: str
    restored_vblank: int
    input_script: Path | None
    input_script_sha256: str | None
    frames: int
    tags: tuple[str, ...]


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def shader_implementation(runtime: Path, environment: dict[str, str]) -> dict[str, Any]:
    """Fingerprint external shader modules: the executable alone is not a build ID."""
    override = environment.get("PS1RECOMP_VULKAN_SHADER_DIR")
    directory = Path(override) if override is not None else runtime.resolve().parent / "shaders"
    if not directory.is_absolute():
        directory = ROOT / directory
    files = {
        path.name: sha256_file(path)
        for path in sorted(directory.glob("*.spv")) if path.is_file()
    }
    return {
        "directory": str(directory.resolve()),
        "resolution": "external_modules" if files else "unknown_compiled_fallback",
        "files": files,
        "sha256": hashlib.sha256(
            json.dumps(files, sort_keys=True, separators=(",", ":")).encode("ascii")
        ).hexdigest() if files else None,
    }


def input_implementation(environment: dict[str, str]) -> dict[str, Any]:
    """Fingerprint the executed script, including diagnostic environment overrides."""
    value = environment.get("PS1_INPUT_SCRIPT")
    if not value:
        return {"path": None, "sha256": None}
    path = Path(value)
    if not path.is_absolute():
        path = ROOT / path
    return {"path": str(path.resolve()), "sha256": sha256_file(path)}


def diagnostic_window_title(scenario: str, environment: dict[str, str]) -> str:
    """Keep automatic controls visibly distinct from the interactive game."""
    scale = environment.get("PS1_INTERNAL_RES_SCALE", "configured")
    interpolation = environment.get("PS1_CRASH3_VULKAN_VERTEX_INTERPOLATION", "0")
    parts = ["AUTOMATED TEST", scenario, f"Vulkan {scale}x",
             f"interpolation {interpolation}x"]
    if environment.get("PS1RECOMP_VULKAN_SHADER_DIR"):
        parts.append("ALTERNATIVE SHADERS")
    if environment.get("PS1_CRASH3_VULKAN_SCENE_SIDECAR") != "1":
        parts.append("ORIGINAL-PATH CONTROL")
    for domain in ("OBJECT", "RESIDENT", "REPLAY", "BACKGROUND"):
        if environment.get(f"PS1_CRASH3_VULKAN_TEMPORAL_DISABLE_{domain}", "0") not in ("", "0"):
            parts.append(f"{domain} INTERPOLATION OFF")
    return " | ".join(parts)


def _relative_to_manifest(manifest: Path, value: str) -> Path:
    candidate = Path(value)
    if candidate.is_absolute():
        return candidate
    # Corpus paths are repository-relative so the manifest remains movable
    # inside evidence/renderer without acquiring ../../ prefixes.
    return ROOT / candidate


def load_manifest(path: Path = DEFAULT_MANIFEST) -> list[Scenario]:
    with path.open(encoding="utf-8") as stream:
        document: dict[str, Any] = json.load(stream)
    if document.get("schema") != SCHEMA:
        raise ValueError(f"{path}: expected schema {SCHEMA!r}")
    raw_scenarios = document.get("scenarios")
    if not isinstance(raw_scenarios, list) or not raw_scenarios:
        raise ValueError(f"{path}: scenarios must be a non-empty list")

    result: list[Scenario] = []
    seen: set[str] = set()
    for index, raw in enumerate(raw_scenarios):
        if not isinstance(raw, dict):
            raise ValueError(f"{path}: scenario {index} is not an object")
        scenario_id = str(raw.get("id", ""))
        if not scenario_id or scenario_id in seen:
            raise ValueError(f"{path}: missing or duplicate scenario id {scenario_id!r}")
        seen.add(scenario_id)
        checksum = str(raw.get("checkpoint_sha256", "")).lower()
        if len(checksum) != 64 or any(c not in "0123456789abcdef" for c in checksum):
            raise ValueError(f"{path}: {scenario_id} has an invalid SHA-256")
        restored_vblank = int(raw.get("restored_vblank", -1))
        if restored_vblank < 0:
            raise ValueError(f"{path}: {scenario_id} has no restored VBlank")
        frames = int(raw.get("frames", 180))
        if frames <= 0:
            raise ValueError(f"{path}: {scenario_id} has an invalid frame bound")
        input_value = raw.get("input_script")
        input_checksum_value = raw.get("input_script_sha256")
        input_checksum = (
            str(input_checksum_value).lower()
            if input_checksum_value is not None
            else None
        )
        if input_value:
            if input_checksum is None or len(input_checksum) != 64 or any(
                c not in "0123456789abcdef" for c in input_checksum
            ):
                raise ValueError(
                    f"{path}: {scenario_id} has an invalid input SHA-256"
                )
        elif input_checksum is not None:
            raise ValueError(
                f"{path}: {scenario_id} has an input hash without a script"
            )
        result.append(
            Scenario(
                scenario_id=scenario_id,
                checkpoint=_relative_to_manifest(path, str(raw["checkpoint"])),
                checkpoint_sha256=checksum,
                restored_vblank=restored_vblank,
                input_script=(
                    _relative_to_manifest(path, str(input_value))
                    if input_value
                    else None
                ),
                input_script_sha256=input_checksum,
                frames=frames,
                tags=tuple(str(tag) for tag in raw.get("tags", [])),
            )
        )
    return result


def verify_scenario(scenario: Scenario) -> None:
    if not scenario.checkpoint.is_file():
        raise FileNotFoundError(
            f"{scenario.scenario_id}: missing local checkpoint {scenario.checkpoint}"
        )
    observed = sha256_file(scenario.checkpoint)
    if observed != scenario.checkpoint_sha256:
        raise ValueError(
            f"{scenario.scenario_id}: checkpoint SHA-256 {observed} != "
            f"manifest {scenario.checkpoint_sha256}"
        )
    if scenario.input_script is not None and not scenario.input_script.is_file():
        raise FileNotFoundError(
            f"{scenario.scenario_id}: missing input script {scenario.input_script}"
        )
    if scenario.input_script is not None:
        observed_input = sha256_file(scenario.input_script)
        if observed_input != scenario.input_script_sha256:
            raise ValueError(
                f"{scenario.scenario_id}: input SHA-256 {observed_input} != "
                f"manifest {scenario.input_script_sha256}"
            )


def materialize_slot_zero(scenario: Scenario, run_directory: Path) -> Path:
    verify_scenario(scenario)
    run_directory.mkdir(parents=True, exist_ok=False)
    destination = run_directory / "quick_slot_0.ps1s"
    # Diagnostics can now autosave as well as load. Do not share an inode with
    # the user's reference, even if today's writer uses atomic replacement.
    shutil.copy2(scenario.checkpoint, destination)
    return destination


def parse_environment_overrides(values: list[str]) -> dict[str, str]:
    result: dict[str, str] = {}
    for value in values:
        name, separator, setting = value.partition("=")
        if not separator or not name or not name.replace("_", "A").isalnum():
            raise ValueError(f"invalid environment override {value!r}; use NAME=VALUE")
        result[name] = setting
    return result


def scenario_environment(
    scenario: Scenario,
    run_directory: Path,
    frames: int,
    scale: int,
    paced: bool,
    modern: bool,
    headless: bool = False,
    benchmark: bool = False,
) -> dict[str, str]:
    if frames <= 0:
        raise ValueError("frames must be positive")
    if not 1 <= scale <= 8:
        raise ValueError("internal scale must be in 1..8")
    if benchmark and (paced or headless or not modern):
        raise ValueError("--benchmark requires modern Vulkan without --paced/--headless/--original")
    environment = dict(os.environ)
    if benchmark:
        # A capacity sample must not inherit a previous diagnostic's culling,
        # native-raster omission, readback, trace or layer-disable switches.
        # Deliberate experimental changes still use --set-env after this preset.
        environment = {
            key: value for key, value in environment.items()
            if not key.startswith("PS1_") and key != "PS1RECOMP_VULKAN_SHADER_DIR"
        }
    # SDL otherwise prefers X11 whenever DISPLAY is also exported by a
    # Wayland desktop. Repeated 4x diagnostic swapchains then burden Xwayland
    # and, on NVIDIA, can exhaust its EGL-image pool. Bind the matrix to the
    # native session backend; interactive launchers keep their ordinary audio
    # and user-selected video policy.
    if "SDL_VIDEODRIVER" not in environment:
        wayland_display = environment.get("WAYLAND_DISPLAY")
        runtime_directory = environment.get("XDG_RUNTIME_DIR")
        if wayland_display and runtime_directory and (
            Path(runtime_directory) / wayland_display
        ).exists():
            environment["SDL_VIDEODRIVER"] = "wayland"
        elif environment.get("DISPLAY"):
            environment["SDL_VIDEODRIVER"] = "x11"
    environment.setdefault("SDL_AUDIODRIVER", "dummy")
    environment.update(
        {
            "PS1_DIAGNOSTIC_SAVESTATE_DIR": str(run_directory),
            "PS1_AUTOLOAD_VBLANK": "20",
            # A relative bound survives checkpoint replacement and avoids
            # coupling execution to manually copied timeline metadata.
            "PS1_EXIT_AFTER_LOAD_VBLANKS": str(frames),
            "PS1_INTERNAL_RES_SCALE": str(scale),
            "PS1_PRESENT_VSYNC": "1" if paced else "0",
            "PS1_DIAGNOSTIC_IGNORE_INPUT": "1",
            "PS1_METRICS": str(run_directory / "metrics.json"),
        }
    )
    if headless:
        # CPU/GTE/stream profiling still executes the real recompiled game and
        # native 1x oracle, but does not require a working desktop swapchain.
        # This is an analysis mode, never a software presentation fallback.
        environment["PS1_DIAGNOSTIC_HEADLESS"] = "1"
    if scenario.input_script is not None:
        environment["PS1_INPUT_SCRIPT"] = str(scenario.input_script)
    if modern:
        environment.update(
            {
                "PS1_CRASH3_VULKAN_RESIDENT_MODE": "debug",
                "PS1_CRASH3_VULKAN_RESIDENT_SHADOW_REPLACE": "1",
                "PS1_CRASH3_VULKAN_RESIDENT_SEMI_REPLACE": "1",
                "PS1_CRASH3_VULKAN_OBJECT_MODE": "debug",
                "PS1_CRASH3_VULKAN_OBJECT_PRESENT": "1",
                # One persistent host-owned scene and scanout span the entire
                # 16:9 canvas. Guest packets remain exact temporal/material
                # witnesses, but cannot select a second visible 4:3 surface.
                "PS1_CRASH3_VULKAN_SCENE_SIDECAR": "1",
                # Pin the current apparent/OT-derived common depth domain;
                # never inherit the older physical-depth experiment from the
                # invoking shell.
                "PS1_CRASH3_VULKAN_SIDECAR_DEPTH": "intrinsic",
                "PS1_CRASH3_VULKAN_OBJECT_REPLACE": "1",
                "PS1_CRASH3_VULKAN_OBJECT_SEMI_REPLACE": "1",
                # Certified production GPU paths: immutable resident topology,
                # per-frame scene state and the ordered semitransparent stream
                # stay in Vulkan. Each capability gate retains an atomic exact
                # projected fallback on unsupported hardware or transitions.
                "PS1_CRASH3_VULKAN_RESIDENT_GPU_OPAQUE_BATCH": "1",
                "PS1_CRASH3_VULKAN_GPU_RAW_SCENE": "1",
                "PS1_CRASH3_VULKAN_GPU_DIRECT_MATERIAL": "1",
                "PS1_CRASH3_VULKAN_GPU_SEMI_BATCH": "1",
                "PS1_CRASH3_VULKAN_GPU_RAW_SEMI": "1",
                "PS1_CRASH3_OBJECT_MATCH_VERIFY": "1",
                "PS1_CRASH3_VULKAN_CULL_STRATEGY": "guard",
            }
        )
    if benchmark:
        environment.update(
            {
                "PS1_VBLANK_PERIOD_US": "1000",
                "PS1_CRASH3_OBJECT_MATCH_VERIFY": "0",
                "PS1_CRASH3_VULKAN_VERTEX_INTERPOLATION": "2",
                "PS1_VULKAN_OMIT_PROJECTED_NATIVE_RASTER_DIAGNOSTIC": "0",
                "PS1_VULKAN_GPU_TIMINGS": "1",
            }
        )
    return environment


def measurement_contract(environment: dict[str, str]) -> dict[str, Any]:
    def enabled(name: str) -> bool:
        return environment.get(name, "") not in ("", "0", "off")

    period = int(environment.get("PS1_VBLANK_PERIOD_US", "0"), 0)
    diagnostic_work = sorted(
        name for name in environment
        if enabled(name) and (
            name.startswith("PS1_TRACE")
            or name.startswith("PS1_VULKAN_REPLAY_READBACK")
            or name == "PS1_CRASH3_OBJECT_MATCH_VERIFY"
        )
    )
    omitted_native = enabled("PS1_VULKAN_OMIT_PROJECTED_NATIVE_RASTER_DIAGNOSTIC")
    capacity = (
        0 < period <= 1000 and not enabled("PS1_PRESENT_VSYNC")
        and not diagnostic_work and not omitted_native
    )
    return {
        "kind": "capacity" if capacity else "paced_or_diagnostic",
        "host_vblank_period_us": period or None,
        "native_refresh_pacing": period == 0,
        "present_vsync": enabled("PS1_PRESENT_VSYNC"),
        "diagnostic_work": diagnostic_work,
        "native_projected_raster_omitted": omitted_native,
        "interpolation_requested": enabled("PS1_CRASH3_VULKAN_VERTEX_INTERPOLATION"),
        "rate_unit": "guest VBlanks/s, not distinct presented frames/s",
        "structural_gate_scope": (
            "Known capture/transport failures over the recorded run; does not "
            "certify image coverage, surface depth ordering, motion or audio."
        ),
    }


def rate_failures(summary: dict[str, int | float], contract: dict[str, Any],
                  minimum_rate: float | None) -> list[str]:
    if minimum_rate is None:
        return []
    if contract["kind"] != "capacity":
        return ["capacity rate gate requires unpaced, native-complete execution without heavy diagnostics"]
    observed = float(summary["effective_vblanks_per_second"])
    if not math.isfinite(observed) or observed < minimum_rate:
        return [f"capacity {observed:.2f} VBlank/s is below {minimum_rate:.2f}"]
    return []


def summarize_metrics(document: dict[str, Any]) -> dict[str, int | float]:
    counters = document.get("counters", {})
    state = document.get("state", {})
    if not isinstance(counters, dict) or not isinstance(state, dict):
        raise ValueError("metrics counters/state must be objects")

    def counter(name: str) -> int:
        return int(counters.get(name, 0))

    def state_value(name: str) -> int:
        return int(state.get(name, 0))

    def observed_failures(name: str) -> int:
        # New runtimes retain intermittent failures in counters as well as
        # the last-frame gauge. Old recordings only have the gauge. Never
        # add the two: the final observation is already part of the total.
        return max(counter(name), state_value(name))

    projected_vertices = counter(
        "host.renderer.replay_coverage.m1_submitted.projected_vertices"
    )
    elapsed_us = state_value("host.elapsed_us")
    vblanks = counter("timing.vblank")
    elapsed_after_load_us = state_value("host.elapsed_after_load_us")
    vblanks_after_load = state_value("timing.vblanks_after_load")
    interpolation_eligible = counter(
        "host.renderer.vertex_interpolation.eligible_triangle"
    )
    interpolation_interpolated = counter(
        "host.renderer.vertex_interpolation.interpolated_triangle"
    )
    replay_interpolation_eligible = counter(
        "host.renderer.vertex_interpolation.replay_eligible_triangle"
    )
    replay_interpolation_interpolated = counter(
        "host.renderer.vertex_interpolation.replay_interpolated_triangle"
    )
    measured_elapsed_us = (
        elapsed_after_load_us if elapsed_after_load_us > 0 else elapsed_us
    )
    measured_vblanks = vblanks_after_load if elapsed_after_load_us > 0 else vblanks
    return {
        "elapsed_us": elapsed_us,
        "vblanks": vblanks,
        "elapsed_after_load_us": elapsed_after_load_us,
        "vblanks_after_load": vblanks_after_load,
        "effective_vblanks_per_second": (
            float(measured_vblanks) * 1_000_000.0 / float(measured_elapsed_us)
            if measured_elapsed_us > 0
            else 0.0
        ),
        "peak_vblank_debt": state_value("timing.vblank_debt_peak"),
        "m1_geometry": counter("host.renderer.vulkan_replay_route.m1_geometry"),
        "m1_latch": counter("host.renderer.vulkan_replay_route.m1_latch"),
        "m1_prefixes": counter(
            "host.renderer.replay_coverage.m1_submitted.prefixes"
        ),
        "m1_complete_prefixes": counter(
            "host.renderer.replay_coverage.m1_submitted."
            "high_precision_complete_prefixes"
        ),
        "m1_incomplete_prefixes": counter(
            "host.renderer.replay_coverage.m1_submitted."
            "high_precision_incomplete_prefixes"
        ),
        "projected_packets": counter(
            "host.renderer.replay_coverage.m1_submitted.projected_packets"
        ),
        "projected_vertices": projected_vertices,
        "projected_xy_precise_vertices": counter(
            "host.renderer.replay_coverage.m1_submitted."
            "projected_xy_precise_vertices"
        ),
        "projected_w_valid_vertices": counter(
            "host.renderer.replay_coverage.m1_submitted."
            "projected_w_valid_vertices"
        ),
        "projected_w_invented_vertices": counter(
            "host.renderer.replay_coverage.m1_submitted."
            "projected_w_invented_vertices"
        ),
        "fallback_packets": counter(
            "host.renderer.replay_coverage.m1_submitted.fallback_packets"
        ),
        "unsupported_mutations": counter(
            "host.renderer.replay_coverage.m1_submitted.unsupported_mutations"
        ),
        "bootstrap_missing_precision_reanchors": counter(
            "host.renderer.vulkan_replay_missing_capability."
            "fallback_missing_precision"
        ),
        "billboard_centres_captured": counter(
            "host.crash3.pgxp.billboard_centre_captured"
        ),
        "billboard_vertices_derived": counter(
            "host.crash3.pgxp.billboard_vertex_derived"
        ),
        "object_distance_cue_conflicts": counter(
            "host.crash3.object_sidecar.distance_cue_conflict"
        ),
        "object_match_spatial_candidates": state_value(
            "host.renderer.crash3_profile.object_match_spatial_candidates"
        ),
        "object_match_fallback_faces": state_value(
            "host.renderer.crash3_profile.object_match_fallback_faces"
        ),
        "object_match_spatial_miss_owners": state_value(
            "host.renderer.crash3_profile.object_match_spatial_miss_owners"
        ),
        "object_match_fallback_owners": state_value(
            "host.renderer.crash3_profile.object_match_fallback_owners"
        ),
        "object_match_spatial_false_misses": observed_failures(
            "host.renderer.crash3_profile.object_match_spatial_false_misses"
        ),
        "object_match_verification_active": state_value(
            "host.renderer.crash3_profile.object_match_verification_active"
        ),
        "object_intrinsic_depth_proofs": counter(
            "host.renderer.crash3_profile.object_intrinsic_depth_proof"
        ),
        "object_intrinsic_depth_mismatches": counter(
            "host.renderer.crash3_profile.object_intrinsic_depth_mismatch"
        ),
        "object_intrinsic_depth_certified_nodes": counter(
            "host.renderer.crash3_profile.object_intrinsic_depth_certified_node"
        ),
        "object_intrinsic_depth_uncertified_source_absent_faces": counter(
            "host.renderer.crash3_profile."
            "object_intrinsic_depth_uncertified_source_absent_face"
        ),
        "object_exact_face_depth_fallbacks": counter(
            "host.renderer.crash3_profile.object_exact_face_depth_fallback"
        ),
        "object_exact_face_depth_applied": counter(
            "host.renderer.crash3_profile.object_exact_face_depth_applied"
        ),
        "object_clamped_node_deferred_faces": counter(
            "host.renderer.crash3_profile."
            "object_replacement_clamped_node_deferred_face"
        ),
        "object_replacement_requested": state_value(
            "host.renderer.crash3_profile.object_replacement_requested"
        ),
        "object_replacement_record_active": state_value(
            "host.renderer.crash3_profile.object_replacement_record_active"
        ),
        "object_original_color_shadowed": counter(
            "host.renderer.crash3_profile."
            "object_replacement_original_color_shadowed"
        ),
        "object_supplement_draw_groups": counter(
            "host.renderer.crash3_profile.object_supplement_draw_group"
        ),
        "object_supplement_page_rejects": counter(
            "host.renderer.crash3_profile.object_supplement_page_reject"
        ),
        "object_supplement_empty": counter(
            "host.renderer.crash3_profile.object_supplement_empty"
        ),
        "object_depth_guide_active": state_value(
            "host.renderer.crash3_profile.object_depth_guide_active"
        ),
        "object_depth_guide_draws": counter(
            "host.renderer.crash3_profile.object_depth_guide_draw"
        ),
        "presentation_canvas_discontinuities": counter(
            "host.renderer.crash3_profile.presentation_canvas_discontinuity"
        ),
        "presentation_native_fallback_visible": counter(
            "host.renderer.crash3_profile.presentation_native_fallback_visible"
        ),
        "presentation_canonical_holds": counter(
            "host.renderer.crash3_profile.presentation_canonical_hold"
        ),
        "presentation_latch_submits_elided": counter(
            "host.renderer.crash3_profile.presentation_latch_submit_elided"
        ),
        "presentation_latch_pages_hidden": counter(
            "host.renderer.crash3_profile.presentation_latch_page_hidden"
        ),
        "presentation_native_reanchors_hidden": counter(
            "host.renderer.crash3_profile.presentation_native_reanchor_hidden"
        ),
        "presentation_projected_epochs": counter(
            "host.renderer.crash3_profile.presentation_epoch_projected"
        ),
        "presentation_epoch_stable": state_value(
            "host.renderer.crash3_profile.presentation_epoch_stable"
        ),
        "vertex_interpolation_operational": state_value(
            "host.renderer.vertex_interpolation.operational"
        ),
        "vertex_interpolation_endpoint_pending": state_value(
            "host.renderer.vertex_interpolation.endpoint_pending"
        ),
        "vertex_interpolation_endpoints_with_geometry": counter(
            "host.renderer.vertex_interpolation.endpoint_presented_with_geometry"
        ),
        "vertex_interpolation_consecutive_endpoints_deferred": counter(
            "host.renderer.vertex_interpolation.consecutive_endpoint_deferred"
        ),
        "vertex_interpolation_replay_survivors_enabled": state_value(
            "host.renderer.vertex_interpolation.replay_survivors_enabled"
        ),
        "vertex_interpolation_current_ambiguous_vertices": state_value(
            "host.renderer.vertex_interpolation.current_ambiguous_vertices"
        ),
        "vertex_interpolation_current_ambiguous_replay_vertices": state_value(
            "host.renderer.vertex_interpolation."
            "current_ambiguous_replay_vertices"
        ),
        "vertex_interpolation_current_eligible_replay_source_vertices": state_value(
            "host.renderer.vertex_interpolation."
            "current_eligible_replay_source_vertices"
        ),
        "vertex_interpolation_current_ambiguous_replay_source_vertices": state_value(
            "host.renderer.vertex_interpolation."
            "current_ambiguous_replay_source_vertices"
        ),
        "vertex_interpolation_current_ambiguous_non_replay_vertices": state_value(
            "host.renderer.vertex_interpolation."
            "current_ambiguous_non_replay_vertices"
        ),
        "vertex_interpolation_eligible_triangles": interpolation_eligible,
        "vertex_interpolation_matched_triangles": counter(
            "host.renderer.vertex_interpolation.matched_triangle"
        ),
        "vertex_interpolation_interpolated_triangles": interpolation_interpolated,
        "vertex_interpolation_procedural_partial_triangles": counter(
            "host.renderer.vertex_interpolation.procedural_partial_triangle"
        ),
        "vertex_interpolation_procedural_partial_vertices": counter(
            "host.renderer.vertex_interpolation.procedural_partial_vertex"
        ),
        "vertex_interpolation_coverage": (
            interpolation_interpolated / interpolation_eligible
            if interpolation_eligible > 0
            else 0.0
        ),
        "vertex_interpolation_unmatched_triangles": counter(
            "host.renderer.vertex_interpolation.unmatched_triangle"
        ),
        "vertex_interpolation_rejected_triangles": counter(
            "host.renderer.vertex_interpolation.rejected_triangle"
        ),
        "vertex_interpolation_replay_atomic_snapped_triangles": counter(
            "host.renderer.vertex_interpolation.replay_primitive_atomic_snap_triangle"
        ),
        "vertex_interpolation_replay_batch_shape_mismatches": counter(
            "host.renderer.vertex_interpolation.replay_batch_shape_mismatch"
        ),
        "vertex_interpolation_resident_raw_opaque_triangles": counter(
            "host.renderer.vertex_interpolation."
            "resident_raw_opaque_interpolated_triangle"
        ),
        "vertex_interpolation_resident_raw_semi_triangles": counter(
            "host.renderer.vertex_interpolation."
            "resident_raw_semi_interpolated_triangle"
        ),
        "vertex_interpolation_resident_projected_triangles": counter(
            "host.renderer.vertex_interpolation."
            "resident_projected_interpolated_triangle"
        ),
        "vertex_interpolation_resident_semi_projected_triangles": counter(
            "host.renderer.vertex_interpolation."
            "resident_semi_projected_interpolated_triangle"
        ),
        "vertex_interpolation_replay_triangles": counter(
            "host.renderer.vertex_interpolation.replay_triangle"
        ),
        "vertex_interpolation_replay_eligible_triangles": (
            replay_interpolation_eligible
        ),
        "vertex_interpolation_replay_interpolated_triangles": (
            replay_interpolation_interpolated
        ),
        "vertex_interpolation_replay_coverage": (
            replay_interpolation_interpolated / replay_interpolation_eligible
            if replay_interpolation_eligible > 0
            else 0.0
        ),
        "vertex_interpolation_object_triangles": counter(
            "host.renderer.vertex_interpolation.object_triangle"
        ),
        "vertex_interpolation_object_interpolated_triangles": counter(
            "host.renderer.vertex_interpolation.object_interpolated_triangle"
        ),
        "vertex_interpolation_object_model_variant_barriers": counter(
            "host.renderer.vertex_interpolation.object_model_variant_barrier"
        ),
        "vertex_interpolation_object_model_variant_snapped_triangles": counter(
            "host.renderer.vertex_interpolation."
            "object_model_variant_snapped_triangle"
        ),
        "vertex_interpolation_object_model_program_transitions": counter(
            "host.renderer.vertex_interpolation.object_model_program_transition"
        ),
        "vertex_interpolation_object_model_multipart": state_value(
            "host.renderer.vertex_interpolation.object_model_multipart_count"
        ),
        "procedural_identity_water_regular": counter(
            "host.crash3.procedural_identity.water_regular"
        ),
        "procedural_identity_terrain_grid": counter(
            "host.crash3.procedural_identity.terrain_grid"
        ),
        "billboard_wide_extents_corrected": counter(
            "host.crash3.pgxp.billboard_wide_extent_corrected"
        ),
        "procedural_identity_terrain_invalid": sum(
            counter(name)
            for name in (
                "host.crash3.procedural_identity.terrain_invalid_walker",
                "host.crash3.procedural_identity.terrain_invalid_grid",
            )
        ),
        "procedural_identity_water_transposed": counter(
            "host.crash3.procedural_identity.water_transposed"
        ),
        "procedural_identity_water_invalid": sum(
            counter(name)
            for name in (
                "host.crash3.procedural_identity.water_invalid_slot",
                "host.crash3.procedural_identity.water_ambiguous_walker",
                "host.crash3.procedural_identity.water_slot_axis_mismatch",
                "host.crash3.procedural_identity."
                "water_unaligned_grid_coordinate",
            )
        ),
        "vertex_interpolation_midpoints_presented": counter(
            "host.renderer.vertex_interpolation.midpoint_presented"
        ),
        "vertex_interpolation_midpoints_resolved": counter(
            "host.renderer.vertex_interpolation.midpoint_resolved"
        ),
        "vertex_interpolation_endpoints_presented": counter(
            "host.renderer.vertex_interpolation.endpoint_presented"
        ),
        "vertex_interpolation_endpoints_resolved": counter(
            "host.renderer.vertex_interpolation.endpoint_resolved"
        ),
        "vertex_interpolation_backups_recorded": counter(
            "host.renderer.vertex_interpolation.backup_recorded"
        ),
        "vertex_interpolation_restores_recorded": counter(
            "host.renderer.vertex_interpolation.restore_recorded"
        ),
        "object_margin_only_draw_groups": counter(
            "host.renderer.crash3_profile.object_margin_only_draw_group"
        ),
        "object_semi_draws": counter(
            "host.renderer.crash3_profile.object_semi_draw"
        ),
        "object_margin_only_semi_draws": counter(
            "host.renderer.crash3_profile.object_margin_only_semi_draw"
        ),
        "procedural_surface_cull_prepared": counter(
            "host.crash3.presentation_cull.procedural_surface_prepared"
        ),
        "procedural_surface_cull_recovered": counter(
            "host.crash3.presentation_cull.procedural_surface_edge_recovered"
        ),
        "procedural_surface_cull_walkers": {
            walker: counter("host.crash3.presentation_cull." + walker)
            for walker in (
                "terrain_forward", "terrain_reverse", "water_forward", "water_reverse"
            )
        },
        "procedural_surface_cull_bound_mismatches": counter(
            "game.crash3.procedural_surface_cull_guard.bound_mismatch"
        ),
        "object_background_painter_passes": counter(
            "host.renderer.crash3_profile.object_background_painter_pass"
        ),
        "object_background_depth_test_passes": counter(
            "host.renderer.crash3_profile.object_background_depth_test_pass"
        ),
        "object_background_crossing_rejected_faces": counter(
            "host.renderer.crash3_profile.object_camera_background_crossing_rejected_face"
        ),
        "background_underlay_triangles": {
            kind: counter("host.renderer.crash3_profile." + kind + "_background_underlay_triangles")
            for kind in ("resident", "object")
        },
        "full_page_screen_overlays": counter(
            "host.crash3.presentation.full_page_screen_overlay"
        ),
        "object_replacement_active": state_value(
            "host.renderer.crash3_profile.object_replacement_active"
        ),
        # Presentation-completeness gates are deliberately separate from M1
        # transport. These detect known construction failures; a pass is not
        # proof of complete visible 16:9 coverage or correct surface ownership.
        "object_unattached_host_intersecting_faces": observed_failures(
            "host.renderer.crash3_profile."
            "object_supplement_unattached_host_intersecting_faces"
        ),
        "object_unresolved_material_faces": observed_failures(
            "host.renderer.crash3_profile.object_supplement_unresolved_material"
        ),
        "object_color_mismatches": observed_failures(
            "host.renderer.crash3_profile.object_supplement_color_mismatches"
        ),
        "object_material_mismatches": observed_failures(
            "host.renderer.crash3_profile.object_supplement_material_mismatches"
        ),
        "object_semi_faces": state_value(
            "host.renderer.crash3_profile.object_supplement_semi_transparent"
        ),
        "object_ordered_semi_faces": state_value(
            "host.renderer.crash3_profile.object_supplement_ordered_semi_faces"
        ),
        "object_unresolved_semi_faces": observed_failures(
            "host.renderer.crash3_profile.object_supplement_unresolved_semi_faces"
        ),
        "object_invalid_depth_faces": observed_failures(
            "host.renderer.crash3_profile.object_supplement_invalid_depth"
        ),
        "object_semi_replacement_active": state_value(
            "host.renderer.crash3_profile.object_semi_replacement_active"
        ),
        "object_semi_draw_calls": counter(
            "host.renderer.crash3_profile.object_semi_draw"
        ),
        "object_semi_draw_triangles": counter(
            "host.renderer.crash3_profile.object_semi_draw_triangles"
        ),
        "resident_unresolved_faces": observed_failures(
            "host.renderer.crash3_profile.resident_supplement_unresolved_faces"
        ),
        "resident_color_mismatches": counter(
            "host.renderer.crash3_profile.resident_color_mismatch"
        ),
        "resident_material_mismatches": sum(
            counter("host.renderer.crash3_profile.resident_material_" + kind + "_mismatch")
            for kind in ("type", "tpage", "clut", "uv", "domain")
        ),
        "resident_constructor_lit_faces": counter(
            "host.crash3.resident_sidecar.constructor_lit_faces"
        ),
        "resident_constructor_flat_black_faces": counter(
            "host.crash3.resident_sidecar.constructor_flat_black_faces"
        ),
        "scenery_highest_material_unavailable": counter(
            "host.crash3.resident_sidecar.highest_material_unavailable"
        ),
        "scenery_detail_transport_invalid": counter(
            "host.renderer.crash3_profile.scenery_texture_detail_invalid"
        ),
        "scenery_detail_material_fallback": counter(
            "host.renderer.crash3_profile.scenery_texture_detail_fallback"
        ),
        "scenery_variant_suppressed_faces": state_value(
            "host.renderer.crash3_profile.scenery_variant_suppressed_faces"
        ),
        "object_normal_constructor_rejected": counter(
            "host.crash3.object_normal_constructor_rejected"
        ),
        "object_source_preflight_rejected": counter(
            "host.crash3.object_source_preflight_rejected"
        ),
        "object_source_constructor_nodes": counter(
            "host.renderer.crash3_profile.object_source_constructor_nodes"
        ),
        "object_source_constructor_faces": counter(
            "host.renderer.crash3_profile.object_source_constructor_faces"
        ),
        "scenery_variant_path_invalid": counter("host.crash3.scenery_variants.path_invalid"),
        "scenery_variant_source_selection_invalid": counter("host.crash3.scenery_variants.source_selection_invalid"),
        "scenery_variant_path_contradiction": counter("host.crash3.scenery_variants.path_contradiction"),
        "scenery_variant_missing_replacement": counter(
            "host.renderer.crash3_profile.scenery_variant_missing_replacement"
        ),
        "scenery_variant_plan_not_committed": counter(
            "host.renderer.crash3_profile.scenery_variant_plan_not_committed"
        ),
        "resident_raw_endpoint_unavailable": observed_failures(
            "host.renderer.crash3_profile.resident_raw_endpoint_unavailable"
        ),
    }


def diagnostic_capture_failures(log: str, environment: dict[str, str],
                                metrics: dict | None = None) -> list[str]:
    """Structural success cannot stand in for a requested visual capture."""
    if '[RendererVK] replay readback rejected:' in log:
        return ['Vulkan readback configuration was rejected; no visual acceptance']
    requested = environment.get('PS1_VULKAN_REPLAY_READBACK') in ('all', 'capture', '1', 'reanchor')
    dumps = [counter for key, counter in (
        ('PS1_VULKAN_REPLAY_READBACK_DUMP_DIRECTORY', 'dump'),
        ('PS1_VULKAN_REPLAY_READBACK_DEPTH_DUMP_DIRECTORY', 'depth_dump'))
        if environment.get(key)]
    if requested and dumps and metrics is not None:
        # The production/actual-only lane may not flush a diagnostic stdout
        # message before the bounded runtime exits. Use this process's actual
        # completed/write counters, not log verbosity or pre-existing files.
        counters = metrics.get('counters', {})
        prefix = 'host.renderer.vulkan_replay_readback.'
        if any(counters.get(prefix + kind + '_failed', 0) for kind in dumps):
            return ['requested Vulkan dump failed to write an image']
        if (counters.get(prefix + 'completed', 0) <= 0 or
                any(counters.get(prefix + kind + '_written', 0) <= 0 for kind in dumps)):
            return ['requested Vulkan dump captured no frame; check bounds and alternating-page sampling']
    elif requested and dumps and '[RendererVK] replay readback frame ' not in log:
        return ['requested Vulkan dump captured no frame; check bounds and alternating-page sampling']
    return []


def modern_metric_failures(
    summary: dict[str, int | float],
    tags: tuple[str, ...] = (),
    *,
    full_scene_sidecar: bool = False,
    depth_guide_requested: bool = False,
    vertex_interpolation_requested: bool = False,
) -> list[str]:
    failures: list[str] = []
    prefixes = int(summary["m1_prefixes"])
    if prefixes <= 0:
        failures.append("no M1 prefix was submitted")
    if int(summary["m1_complete_prefixes"]) != prefixes:
        failures.append("not every M1 prefix is high-precision complete")
    if int(summary["m1_incomplete_prefixes"]) != 0:
        failures.append("incomplete M1 prefixes were submitted")
    vertices = int(summary["projected_vertices"])
    if int(summary["projected_xy_precise_vertices"]) != vertices:
        failures.append("not every Projected3D vertex has precise XY")
    if int(summary["projected_w_valid_vertices"]) != vertices:
        failures.append("not every Projected3D vertex has authentic W")
    if int(summary["projected_w_invented_vertices"]) != 0:
        failures.append("invented Projected3D W reached M1")
    if int(summary["fallback_packets"]) != 0:
        failures.append("fallback packets reached M1")
    if int(summary["unsupported_mutations"]) != 0:
        failures.append("unsupported mutations reached M1")
    if int(summary["object_distance_cue_conflicts"]) != 0:
        failures.append("an object used multiple DPCT tuples in one node")
    if int(summary["object_match_spatial_false_misses"]) != 0:
        failures.append("the object spatial index missed an exact face match")
    if int(summary["object_intrinsic_depth_mismatches"]) != 0:
        failures.append("intrinsic object OT depth contradicted an emitted packet")
    if (
        int(summary["object_replacement_active"]) != 0
        and int(summary["object_intrinsic_depth_proofs"]) <= 0
    ):
        failures.append("object replacement has no intrinsic OT depth proof")
    if int(summary["object_intrinsic_depth_uncertified_source_absent_faces"]) != 0:
        failures.append("source-absent object faces lack intrinsic OT depth")
    if int(summary["object_exact_face_depth_fallbacks"]) != 0:
        failures.append("object faces fell back to per-packet depth")
    if full_scene_sidecar:
        if int(summary["resident_raw_endpoint_unavailable"]) != 0:
            failures.append("resident scene lost both raw and projected modern endpoints")
        # Full-scene presentation deliberately replaces exact original colour
        # packets and draws over the complete 16:9 page. Its hard temporal
        # failure is losing an exact capture/page, which used to make whole
        # object families alternate with the original 4:3 presentation.
        if int(summary["object_supplement_page_rejects"]) != 0:
            failures.append("a full-scene object capture was rejected")
        if int(summary["presentation_canvas_discontinuities"]) != 0:
            failures.append("the full-scene canvas changed inside one 3D epoch")
        if int(summary["presentation_native_fallback_visible"]) != 0:
            failures.append("a native page became visible inside the modern epoch")
        latch_pages = int(summary["presentation_latch_pages_hidden"])
        latch_elisions = int(summary["presentation_latch_submits_elided"])
        if vertex_interpolation_requested:
            midpoint_presented = int(
                summary["vertex_interpolation_midpoints_presented"]
            )
            midpoint_resolved = int(
                summary["vertex_interpolation_midpoints_resolved"]
            )
            endpoint_presented = int(
                summary["vertex_interpolation_endpoints_presented"]
            )
            endpoint_resolved = int(
                summary["vertex_interpolation_endpoints_resolved"]
            )
            endpoint_pending = int(
                summary["vertex_interpolation_endpoint_pending"]
            )
            backups = int(summary["vertex_interpolation_backups_recorded"])
            restores = int(summary["vertex_interpolation_restores_recorded"])
            if int(summary["vertex_interpolation_operational"]) != 1:
                failures.append("vertex interpolation was requested but not operational")
            if midpoint_presented <= 0:
                failures.append("vertex interpolation presented no midpoint")
            if midpoint_presented != midpoint_resolved:
                failures.append("a midpoint was presented without an exact resolve")
            if endpoint_presented != endpoint_resolved:
                failures.append("an endpoint was presented without an exact resolve")
            deferred = int(summary["vertex_interpolation_consecutive_endpoints_deferred"])
            endpoints_with_geometry = int(summary["vertex_interpolation_endpoints_with_geometry"])
            if midpoint_presented + deferred != endpoint_presented + endpoint_pending:
                failures.append("midpoint and endpoint presentation counts diverged")
            if backups != midpoint_resolved or restores != midpoint_resolved:
                failures.append("midpoint backup/restore accounting diverged")
            if latch_elisions + endpoint_presented - endpoints_with_geometry != latch_pages:
                failures.append("a canonical latch page has no proven presentation route")
            if (
                int(
                    summary[
                        "vertex_interpolation_current_ambiguous_non_replay_vertices"
                    ]
                )
                != 0
            ):
                failures.append(
                    "a resident/object temporal identity became ambiguous"
                )
            if int(summary["vertex_interpolation_interpolated_triangles"]) <= 0:
                failures.append("vertex interpolation moved no complete triangle")
        elif latch_elisions != latch_pages:
            failures.append("a canonical latch page still performed a Vulkan submit")
        if (
            int(summary["projected_vertices"]) > 0
            and int(summary["presentation_projected_epochs"]) <= 0
        ):
            failures.append("no canonical projected presentation epoch was established")
        if not depth_guide_requested and (
            int(summary["object_depth_guide_active"]) != 0
            or int(summary["object_depth_guide_draws"]) != 0
        ):
            failures.append("the diagnostic object depth guide ran operationally")
    else:
        if int(summary["object_replacement_requested"]) != 0:
            failures.append("the operational matrix requested object replacement")
        if int(summary["object_replacement_record_active"]) != 0:
            failures.append("object replacement reached Vulkan recording")
        if int(summary["object_original_color_shadowed"]) != 0:
            failures.append("an original object color face was shadowed")
        if int(summary["object_supplement_draw_groups"]) != int(
            summary["object_margin_only_draw_groups"]
        ):
            failures.append("an opaque host object draw escaped the 16:9 margins")
        if int(summary["object_semi_draws"]) != int(
            summary["object_margin_only_semi_draws"]
        ):
            failures.append(
                "a semitransparent host object draw escaped the 16:9 margins"
            )
    if int(summary["procedural_surface_cull_bound_mismatches"]) != 0:
        failures.append("a procedural surface used an unexpected cull bound")
    if int(summary["vertex_interpolation_replay_batch_shape_mismatches"]) != 0:
        failures.append("temporal replay metadata disagreed with the current vertex batch")
    if int(summary["procedural_identity_terrain_invalid"]) != 0:
        failures.append("a procedural terrain vertex lacked intrinsic grid identity")
    if "procedural-surface" in tags:
        if int(summary["procedural_surface_cull_prepared"]) <= 0:
            failures.append("the procedural surface cull path was not exercised")
        if int(summary["procedural_surface_cull_recovered"]) <= 0:
            failures.append("the procedural surface recovered no 16:9 edge")
        if int(summary["procedural_identity_water_invalid"]) != 0:
            failures.append(
                "a procedural water vertex lacked intrinsic grid identity"
            )
    if int(summary["object_unattached_host_intersecting_faces"]) != 0:
        failures.append("a visible object mesh has no exact renderer attachment")
    if int(summary["object_unresolved_material_faces"]) != 0:
        failures.append("object faces remain without a certified material")
    if int(summary["object_material_mismatches"]) != 0:
        failures.append("decoded object material contradicts an emitted packet")
    if int(summary["object_color_mismatches"]) != 0:
        failures.append("decoded object color contradicts an emitted packet")
    if int(summary["object_unresolved_semi_faces"]) != 0:
        failures.append("object semitransparency remains unresolved")
    if int(summary["object_invalid_depth_faces"]) != 0:
        failures.append("object faces have invalid presentation depth")
    if (
        int(summary["object_ordered_semi_faces"]) > 0
        and int(summary["object_semi_draw_triangles"]) <= 0
    ):
        failures.append("ordered object semitransparency was not drawn")
    if int(summary["resident_unresolved_faces"]) != 0:
        failures.append("resident scenery faces remain unresolved")
    if int(summary.get("resident_color_mismatches", 0)) != 0:
        failures.append("decoded scenery color contradicts an emitted packet")
    if int(summary.get("resident_material_mismatches", 0)) != 0:
        failures.append("decoded scenery material contradicts an emitted packet")
    if int(summary.get("scenery_highest_material_unavailable", 0)) != 0:
        failures.append("an authored highest-detail scenery material was unavailable")
    if int(summary.get("scenery_detail_transport_invalid", 0)) != 0:
        failures.append("a highest-detail scenery material lost its exact face binding")
    if int(summary.get("scenery_detail_material_fallback", 0)) != 0:
        failures.append("a highest-detail scenery material could not reach presentation")
    for field in ("scenery_variant_path_invalid", "scenery_variant_path_contradiction",
                  "scenery_variant_source_selection_invalid",
                  "scenery_variant_missing_replacement", "scenery_variant_plan_not_committed"):
        if int(summary.get(field, 0)) != 0:
            failures.append(f"scenery variant selection failed: {field}")
    if int(summary.get("object_normal_constructor_rejected", 0)) != 0:
        failures.append("a selected normal-lighting constructor could not be decoded")
    if int(summary.get("object_source_preflight_rejected", 0)) != 0:
        failures.append("a source object bounding-box preflight could not be prepared")
    return failures


def _run_name(scenario_id: str, ordinal: int) -> str:
    timestamp = datetime.now(timezone.utc).strftime("%Y%m%dT%H%M%SZ")
    return f"{timestamp}-{ordinal:02d}-{scenario_id.replace('.', '-')}"


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("scenario", nargs="*", help="scenario ids; default: all")
    parser.add_argument("--manifest", type=Path, default=DEFAULT_MANIFEST)
    parser.add_argument("--list", action="store_true", help="list scenarios and exit")
    parser.add_argument(
        "--frames",
        type=int,
        help="override each scenario's manifest frame bound",
    )
    parser.add_argument("--scale", type=int, default=4)
    parser.add_argument("--paced", action="store_true", help="retain present VSync")
    parser.add_argument(
        "--benchmark", action="store_true",
        help="clean production Vulkan capacity preset: 1 ms host clock, no VSync, native raster retained",
    )
    parser.add_argument(
        "--minimum-rate", type=float,
        help="fail below this guest VBlank/s capacity (requires --benchmark)",
    )
    parser.add_argument(
        "--original", action="store_true", help="disable Crash host presentation"
    )
    parser.add_argument(
        "--headless",
        action="store_true",
        help=(
            "run presentation-free CPU/GTE/stream diagnostics; implies the "
            "original analysis preset and skips Vulkan completeness gates"
        ),
    )
    parser.add_argument(
        "--set-env",
        action="append",
        default=[],
        metavar="NAME=VALUE",
        help="override one runtime variable after the selected presentation preset",
    )
    parser.add_argument("--dry-run", action="store_true")
    parser.add_argument(
        "--output-root", type=Path, default=ROOT / "build/checkpoint-matrix"
    )
    parser.add_argument(
        "--runtime", type=Path, default=ROOT / "build/ps1Runtime/ps1Runtime"
    )
    parser.add_argument("--config", type=Path, default=ROOT / "crash3_config.toml")
    args = parser.parse_args(argv)

    try:
        if args.benchmark and (args.paced or args.headless or args.original):
            raise ValueError("--benchmark requires modern Vulkan without --paced/--headless/--original")
        if args.minimum_rate is not None and (
            not args.benchmark or not math.isfinite(args.minimum_rate)
            or args.minimum_rate <= 0
        ):
            raise ValueError("--minimum-rate requires --benchmark and a finite positive rate")
        environment_overrides = parse_environment_overrides(args.set_env)
        scenarios = load_manifest(args.manifest.resolve())
        by_id = {scenario.scenario_id: scenario for scenario in scenarios}
        if args.list:
            for scenario in scenarios:
                state = "ready" if scenario.checkpoint.is_file() else "missing"
                script = scenario.input_script.name if scenario.input_script else "autonomous"
                print(
                    f"{scenario.scenario_id:28} vblank={scenario.restored_vblank:<7} "
                    f"frames={scenario.frames:<5} {state:7} {script}"
                )
            return 0
        selected = scenarios if not args.scenario else []
        for scenario_id in args.scenario:
            if scenario_id not in by_id:
                raise ValueError(f"unknown scenario {scenario_id!r}")
            selected.append(by_id[scenario_id])
        if not args.runtime.is_file():
            raise FileNotFoundError(f"runtime not found: {args.runtime}")
        if not args.config.is_file():
            raise FileNotFoundError(f"config not found: {args.config}")
        implementation = {
            "runtime_binary_sha256": sha256_file(args.runtime),
            "configuration_sha256": sha256_file(args.config),
        }

        args.output_root.mkdir(parents=True, exist_ok=True)
        modern = not args.original and not args.headless
        exit_code = 0
        matrix_results: list[dict[str, Any]] = []
        for ordinal, scenario in enumerate(selected):
            frames = args.frames if args.frames is not None else scenario.frames
            run_directory = args.output_root / _run_name(scenario.scenario_id, ordinal)
            if args.dry_run:
                verify_scenario(scenario)
                print(
                    f"{scenario.scenario_id}: {args.runtime} --config {args.config} "
                    f"(VBlank {scenario.restored_vblank}.."
                    f"{scenario.restored_vblank + frames}, {args.scale}x)"
                )
                continue
            materialize_slot_zero(scenario, run_directory)
            environment = scenario_environment(
                scenario,
                run_directory,
                frames,
                args.scale,
                args.paced,
                modern,
                args.headless,
                args.benchmark,
            )
            environment.update(environment_overrides)
            environment.setdefault(
                "PS1_WINDOW_TITLE", diagnostic_window_title(scenario.scenario_id, environment)
            )
            contract = measurement_contract(environment)
            shaders = shader_implementation(args.runtime, environment) if modern else None
            executed_input = input_implementation(environment)
            with (run_directory / "run.log").open("w", encoding="utf-8") as log:
                completed = subprocess.run(
                    [str(args.runtime), "--config", str(args.config)],
                    cwd=ROOT,
                    env=environment,
                    stdout=log,
                    stderr=subprocess.STDOUT,
                    check=False,
                )
            print(
                f"{scenario.scenario_id}: exit={completed.returncode} "
                f"artifacts={run_directory}"
            )
            result: dict[str, Any] = {
                "scenario": scenario.scenario_id,
                "exit_code": completed.returncode,
                "frames_requested": frames,
                "internal_scale": args.scale,
                "modern": modern,
                "headless": args.headless,
                "checkpoint_sha256": scenario.checkpoint_sha256,
                "input_script_sha256": executed_input["sha256"],
                "input_script": executed_input["path"],
                "manifest_input_script_sha256": scenario.input_script_sha256,
                "environment_overrides": environment_overrides,
                "measurement": contract,
                "implementation": {**implementation, "vulkan_shaders": shaders},
            }
            metrics_path = run_directory / "metrics.json"
            metrics_document = None
            if metrics_path.is_file():
                with metrics_path.open(encoding="utf-8") as stream:
                    metrics_document = json.load(stream)
            failures: list[str] = []
            failures.extend(diagnostic_capture_failures(
                (run_directory / 'run.log').read_text(encoding='utf-8', errors='replace'),
                environment, metrics_document))
            if modern and shaders != shader_implementation(args.runtime, environment):
                failures.append("Vulkan shader modules changed while the scenario was running")
            if executed_input != input_implementation(environment):
                failures.append("Input script changed while the scenario was running")
            if completed.returncode != 0:
                failures.append(f"runtime exit {completed.returncode}")
            if metrics_document is not None:
                summary = summarize_metrics(metrics_document)
                failures.extend(rate_failures(summary, contract, args.minimum_rate))
                result["metrics"] = summary
                result["metrics_sha256"] = sha256_file(metrics_path)
                if modern:
                    switch_enabled = lambda name: environment.get(name, "") not in (
                        "",
                        "0",
                        "off",
                    )
                    full_scene_sidecar = switch_enabled(
                        "PS1_CRASH3_VULKAN_SCENE_SIDECAR"
                    )
                    depth_guide_requested = (
                        full_scene_sidecar
                        and switch_enabled("PS1_CRASH3_VULKAN_SIDECAR_DEPTH_GUIDE")
                        and not switch_enabled(
                            "PS1_CRASH3_VULKAN_SIDECAR_DISABLE_DEPTH_GUIDE"
                        )
                    )
                    vertex_interpolation_requested = switch_enabled(
                        "PS1_CRASH3_VULKAN_VERTEX_INTERPOLATION"
                    )
                    result["full_scene_sidecar"] = full_scene_sidecar
                    result["depth_guide_requested"] = depth_guide_requested
                    result["vertex_interpolation_requested"] = (
                        vertex_interpolation_requested
                    )
                    failures.extend(
                        modern_metric_failures(
                            summary,
                            scenario.tags,
                            full_scene_sidecar=full_scene_sidecar,
                            depth_guide_requested=depth_guide_requested,
                            vertex_interpolation_requested=(
                                vertex_interpolation_requested
                            ),
                        )
                    )
                print(
                    f"  M1 {summary['m1_complete_prefixes']}/"
                    f"{summary['m1_prefixes']} complete, "
                    f"vertices={summary['projected_vertices']}, "
                    f"unattached-visible="
                    f"{summary['object_unattached_host_intersecting_faces']}, "
                    f"{summary['effective_vblanks_per_second']:.2f} VBlank/s "
                    f"[{contract['kind']}]"
                )
            else:
                failures.append("metrics.json was not produced")
            result["failures"] = failures
            result["passed"] = not failures
            (run_directory / "result.json").write_text(
                json.dumps(
                    {"schema": RESULT_SCHEMA, **result}, indent=2, sort_keys=True
                )
                + "\n",
                encoding="utf-8",
            )
            matrix_results.append(result)
            if completed.returncode != 0:
                exit_code = 1
            if failures:
                exit_code = 1
                for failure in failures:
                    print(f"  FAIL: {failure}", file=sys.stderr)
        if not args.dry_run:
            (args.output_root / "matrix-summary.json").write_text(
                json.dumps(
                    {
                        "schema": RESULT_SCHEMA,
                        "internal_scale": args.scale,
                        "modern": modern,
                        "headless": args.headless,
                        "passed": exit_code == 0,
                        "results": matrix_results,
                    },
                    indent=2,
                    sort_keys=True,
                )
                + "\n",
                encoding="utf-8",
            )
        return exit_code
    except (FileNotFoundError, KeyError, OSError, ValueError, json.JSONDecodeError) as error:
        print(error, file=sys.stderr)
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
