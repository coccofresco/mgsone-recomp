#!/usr/bin/env python3
"""Audit Crash 3 resident-section handoffs from intrinsic resource identity.

The runtime emits one catalog plus one row per resident world at every scenery
construction.  This tool validates the complete sequence without knowing a
level, section, frame number, camera coordinate or expected resource count.
It independently reconstructs the sorted EID/geometry sets, their overlaps,
candidate debounce and atomic presentation policy.  A trace from any gameplay
route can therefore extend the same regression gate with no transition-specific
code.
"""

from __future__ import annotations

import argparse
import collections
import json
import sys
from pathlib import Path
from typing import Any


TRACE_SCHEMA = "ps1trace/v1"
REPORT_SCHEMA = "ps1-crash3-resident-transition-audit/v3"
CATALOG_EVENT = "game.crash3.resident_projection_catalog"
WORLD_EVENT = "game.crash3.resident_projection_world"
MASK32 = (1 << 32) - 1
MASK64 = (1 << 64) - 1
FNV_OFFSET = 14695981039346656037
FNV_PRIME = 1099511628211


def _u64(value: Any) -> int:
    return int(value) & MASK64


def _s32(value: Any) -> int:
    decoded = int(value) & MASK32
    return decoded - (1 << 32) if decoded & (1 << 31) else decoded


def _hash32(state: int, value: int) -> int:
    for shift in range(0, 32, 8):
        state ^= (value >> shift) & 0xFF
        state = (state * FNV_PRIME) & MASK64
    return state


def resource_set_fingerprint(keys: tuple[tuple[int, int], ...]) -> int:
    state = _hash32(FNV_OFFSET, len(keys))
    for resource_id, geometry in keys:
        state = _hash32(state, resource_id)
        state = _hash32(state, geometry & MASK32)
        state = _hash32(state, geometry >> 32)
    return state


def _fail(path: Path, frame: int, message: str) -> ValueError:
    return ValueError(f"{path}: frame {frame}: {message}")


def _load_trace(
    path: Path,
) -> list[tuple[int, dict[str, Any], list[dict[str, Any]]]]:
    # A fast renderer can publish two immutable scenery constructions during
    # one guest trace frame.  `frame` is therefore a presentation timestamp,
    # not the transaction identity; capture_id is the monotonic identity
    # shared by the catalog and all of its world rows.
    catalogs: dict[tuple[int, int], dict[str, Any]] = {}
    worlds: dict[tuple[int, int], list[dict[str, Any]]] = collections.defaultdict(list)
    with path.open(encoding="utf-8") as stream:
        for lineno, line in enumerate(stream, 1):
            if not line.strip():
                continue
            # A face-enabled trace can contain millions of unrelated records.
            # The event-name prefilter preserves exact JSON validation for the
            # transaction records while keeping the normal catalog audit fast.
            if CATALOG_EVENT not in line and WORLD_EVENT not in line:
                continue
            try:
                record = json.loads(line)
            except json.JSONDecodeError as exc:
                raise ValueError(f"{path}:{lineno}: invalid JSON: {exc}") from exc
            if record.get("schema") != TRACE_SCHEMA:
                raise ValueError(
                    f"{path}:{lineno}: schema {record.get('schema')!r}, "
                    f"expected {TRACE_SCHEMA!r}"
                )
            frame = int(record.get("frame", -1))
            capture = int(record.get("capture_id", -1))
            key = (frame, capture)
            if record.get("event") == CATALOG_EVENT:
                if capture < 0:
                    raise _fail(path, frame, "catalog has no capture identity")
                if key in catalogs:
                    raise _fail(path, frame, f"duplicate catalog capture={capture}")
                catalogs[key] = record
            elif record.get("event") == WORLD_EVENT:
                if capture < 0:
                    raise _fail(path, frame, "world row has no capture identity")
                worlds[key].append(record)
    if not catalogs:
        raise ValueError(f"{path}: no {CATALOG_EVENT} records")

    result = []
    for (frame, capture), catalog in sorted(catalogs.items()):
        if int(catalog.get("catalog_schema", 0)) not in (3, 4):
            raise _fail(path, frame, "transition audit requires catalog schema 3 or 4")
        members = worlds.get((frame, capture), [])
        expected = int(catalog.get("world_count", -1))
        if len(members) != expected:
            raise _fail(
                path,
                frame,
                f"incomplete world catalog: expected={expected}, records={len(members)}",
            )
        if any(
            int(item.get("world_schema", 0)) != 2
            or int(item.get("capture_id", -2)) != capture
            for item in members
        ):
            raise _fail(path, frame, "mixed or unsupported world catalog")
        ordered = sorted(members, key=lambda item: int(item.get("world", -1)))
        if [int(item.get("world", -1)) for item in ordered] != list(range(expected)):
            raise _fail(path, frame, "world ordinals are not complete and unique")
        result.append((frame, catalog, ordered))
    unexpected = sorted(set(worlds) - set(catalogs))
    if unexpected:
        raise ValueError(
            f"{path}: world rows without catalogs at "
            f"{[(frame, capture) for frame, capture in unexpected]}"
        )
    return result


def _world_key(world: dict[str, Any]) -> tuple[int, int]:
    return (
        int(world.get("resource_id", 0)) & MASK32,
        _u64(world.get("geometry_fingerprint", 0)),
    )


def _world_identity_valid(world: dict[str, Any]) -> bool:
    resource_id, _ = _world_key(world)
    return (
        int(world.get("resource_pair_valid", 0)) == 1
        and int(world.get("identity_valid", 0)) == 1
        and bool(resource_id & 1)
    )


def _world_structural_placement(
    world: dict[str, Any],
) -> tuple[bool, tuple[int, int, int]]:
    origin = tuple(_s32(world[f"info_origin_{axis}"]) for axis in "xyz")
    translation = tuple(_s32(world[f"translation_{axis}"]) for axis in "xyz")
    offset = tuple(translation[index] - origin[index] // 4 for index in range(3))
    flags = int(world.get("info_flags", -1))
    valid = all(component % 4 == 0 for component in origin) and flags in (0, 1)
    if flags == 1:
        valid = valid and offset == (0, 0, 0)
    return valid, offset


def _world_authored_placement(world: dict[str, Any]) -> tuple[int, int, int, int]:
    return (
        *(_s32(world[f"info_origin_{axis}"]) for axis in "xyz"),
        int(world.get("info_flags", -1)),
    )


def _validate_worlds(
    path: Path, frame: int, catalog: dict[str, Any], worlds: list[dict[str, Any]]
) -> tuple[tuple[int, int], ...]:
    keys = []
    offsets = []
    placement_valid = True
    for world in worlds:
        resource_id = int(world.get("resource_id", 0)) & MASK32
        geometry = _u64(world.get("geometry_fingerprint", 0))
        identity = (
            int(world.get("resource_pair_valid", 0)) == 1
            and int(world.get("identity_valid", 0)) == 1
            and resource_id & 1
        )
        if not identity:
            placement_valid = False
        origin = tuple(_s32(world[f"info_origin_{axis}"]) for axis in "xyz")
        translation = tuple(_s32(world[f"translation_{axis}"]) for axis in "xyz")
        flags = int(world.get("info_flags", -1))
        aligned = all(component % 4 == 0 for component in origin)
        offset = tuple(translation[index] - origin[index] // 4 for index in range(3))
        valid = aligned and flags in (0, 1)
        if flags == 1:
            valid = valid and offset == (0, 0, 0)
        elif flags == 0:
            offsets.append(offset)
        valid = valid and int(world.get("placement_validated", 0)) == 1
        placement_valid = placement_valid and valid
        keys.append((resource_id, geometry))
    keys.sort()
    if len({key[0] for key in keys}) != len(keys):
        placement_valid = False
    if offsets and any(offset != offsets[0] for offset in offsets[1:]):
        placement_valid = False
    traced_placement = int(catalog.get("placement_validated", 0)) == 1
    if traced_placement != placement_valid:
        raise _fail(
            path,
            frame,
            f"placement verdict differs: trace={traced_placement}, audit={placement_valid}",
        )
    result = tuple(keys)
    expected_fingerprint = resource_set_fingerprint(result)
    traced_fingerprint = _u64(catalog.get("resource_set_fingerprint", 0))
    if traced_fingerprint != expected_fingerprint:
        raise _fail(
            path,
            frame,
            "resource-set fingerprint does not match sorted EID/geometry catalog",
        )
    return result


def analyze_transition_trace(path: Path) -> dict[str, Any]:
    rows = _load_trace(path)
    keys_by_fingerprint: dict[int, tuple[tuple[int, int], ...]] = {}
    decoded = []
    for frame, catalog, worlds in rows:
        keys = _validate_worlds(path, frame, catalog, worlds)
        fingerprint = _u64(catalog["resource_set_fingerprint"])
        previous = keys_by_fingerprint.setdefault(fingerprint, keys)
        if previous != keys:
            raise _fail(path, frame, "resource-set hash collision or mixed catalog")
        decoded.append((frame, catalog, keys, worlds))

    geometry_by_observed_id: dict[int, set[int]] = collections.defaultdict(set)
    resources_by_authored_placement: dict[
        tuple[int, int, int, int], set[tuple[int, int]]
    ] = collections.defaultdict(set)
    simultaneous_placement_conflicts: list[dict[str, Any]] = []
    for frame, catalog, _, worlds in decoded:
        capture_placements: dict[
            tuple[int, int, int, int], set[tuple[int, int]]
        ] = collections.defaultdict(set)
        for world in worlds:
            key = _world_key(world)
            placement = _world_authored_placement(world)
            geometry_by_observed_id[key[0]].add(key[1])
            resources_by_authored_placement[placement].add(key)
            capture_placements[placement].add(key)
        simultaneous_placement_conflicts.extend(
            {
                "frame": frame,
                "capture_id": int(catalog.get("capture_id", 0)),
                "placement": list(placement),
                "resources": [list(key) for key in sorted(keys)],
            }
            for placement, keys in sorted(capture_placements.items())
            if len(keys) > 1
        )

    resource_geometry_variants = [
        {
            "resource_id": resource_id,
            "geometry_fingerprints": sorted(geometries),
        }
        for resource_id, geometries in sorted(geometry_by_observed_id.items())
        if len(geometries) > 1
    ]
    authored_placement_resource_variants = [
        {
            "placement": list(placement),
            "resources": [list(key) for key in sorted(keys)],
        }
        for placement, keys in sorted(resources_by_authored_placement.items())
        if len(keys) > 1
    ]

    transitions = []
    invalid_holds = 0
    candidate_captures = 0
    previous_active: int | None = None
    previous_generation: int | None = None
    expected_candidate: tuple[tuple[int, int], ...] | None = None
    expected_observations = 0
    geometry_by_id: dict[int, int] = {}
    pending_resources: dict[tuple[int, int], tuple[int, int]] = {}
    # Values are [role, last observed capture, consecutive absences].
    scene_resources: dict[tuple[int, int], list[Any]] = {}
    individually_validated_captures = 0
    individually_admitted_resources = 0
    individually_revoked_resources = 0
    maximum_scene_resources = 0
    catalog_schemas = {
        int(catalog.get("catalog_schema", 0)) for _, catalog, _, _ in decoded
    }
    if len(catalog_schemas) != 1:
        raise ValueError(f"{path}: mixed catalog schemas {sorted(catalog_schemas)}")
    catalog_schema = next(iter(catalog_schemas))

    for row_index, (frame, catalog, current_keys, worlds) in enumerate(decoded):
        current_fingerprint = _u64(catalog["resource_set_fingerprint"])
        active_fingerprint = _u64(catalog["active_cohort_fingerprint"])
        generation = int(catalog.get("active_cohort_generation", 0))
        validated = int(catalog.get("resource_set_validated", 0)) == 1
        conflict = int(catalog.get("resource_identity_conflict", 0)) == 1
        candidate = int(catalog.get("cohort_candidate", 0)) == 1
        committed = int(catalog.get("cohort_committed", 0)) == 1
        enabled = int(catalog.get("cohort_presentation_enabled", 0)) == 1
        presentation_valid = int(catalog.get("cohort_presentation_valid", 0)) == 1

        if conflict and validated:
            raise _fail(path, frame, "identity conflict was accepted as valid")
        if validated and not int(catalog.get("placement_validated", 0)):
            raise _fail(path, frame, "validated set has invalid placement")
        for resource_id, geometry in current_keys:
            known = geometry_by_id.get(resource_id)
            if validated and known is not None and known != geometry:
                raise _fail(path, frame, "known EID changed immutable geometry")

        old_active = previous_active
        old_keys = keys_by_fingerprint.get(old_active, ()) if old_active else ()
        shared = len(set(old_keys).intersection(current_keys))
        traced_shared = int(catalog.get("shared_active_resources", 0))

        expected_admissions = 0
        expected_revocations = 0
        individually_valid_keys: set[tuple[int, int]] = set()
        if catalog_schema >= 4:
            capture_id = int(catalog.get("capture_id", 0))
            valid_resource_id_counts: dict[int, int] = {}
            for world in worlds:
                if _world_identity_valid(world):
                    resource_id, _ = _world_key(world)
                    valid_resource_id_counts[resource_id] = (
                        valid_resource_id_counts.get(resource_id, 0) + 1
                    )

            def identity_conflicts(key: tuple[int, int]) -> bool:
                known = geometry_by_id.get(key[0])
                if known is not None and known != key[1]:
                    return True
                return any(
                    candidate[0] == key[0] and candidate != key
                    for candidate in (*scene_resources, *pending_resources)
                )

            source_offsets = {
                _world_structural_placement(world)[1]
                for world in worlds
                if int(world.get("individual_source_witness", 0)) == 1
                and int(world.get("info_flags", -1)) == 0
                and _world_identity_valid(world)
                and valid_resource_id_counts.get(_world_key(world)[0], 0) == 1
                and not identity_conflicts(_world_key(world))
                and _world_structural_placement(world)[0]
                and int(world.get("individual_payload_complete", 0)) == 1
            }
            for world in worlds:
                key = _world_key(world)
                structural, offset = _world_structural_placement(world)
                payload = int(world.get("individual_payload_complete", 0)) == 1
                placement_witness = structural and (
                    int(world.get("info_flags", -1)) == 1
                    or validated
                    or offset in source_offsets
                )
                if bool(int(world.get("individual_placement_witness", 0))) != placement_witness:
                    raise _fail(path, frame, "per-resource placement witness differs")
                individually_valid = (
                    _world_identity_valid(world)
                    and valid_resource_id_counts.get(key[0], 0) == 1
                    and payload
                    and placement_witness
                    and not identity_conflicts(key)
                )
                if bool(int(world.get("individually_validated", 0))) != individually_valid:
                    raise _fail(path, frame, "per-resource validation verdict differs")
                expected_resource_observations = 0
                if individually_valid:
                    individually_valid_keys.add(key)
                    if row_index == 0:
                        expected_resource_observations = int(
                            world.get("individual_observations", 0)
                        )
                        if expected_resource_observations not in (1, 2):
                            raise _fail(
                                path, frame, "invalid initial per-resource debounce"
                            )
                        if expected_resource_observations == 1:
                            pending_resources[key] = (capture_id, 1)
                        else:
                            scene_resources[key] = ["incoming", capture_id, 0]
                            if committed and generation == 1:
                                expected_admissions += 1
                    elif key in old_keys or key in scene_resources:
                        expected_resource_observations = 2
                        if key in scene_resources:
                            scene_resources[key][1] = capture_id
                            scene_resources[key][2] = 0
                    else:
                        previous_capture, observations = pending_resources.get(
                            key, (0, 0)
                        )
                        observations = (
                            observations + 1
                            if previous_capture + 1 == capture_id
                            else 1
                        )
                        pending_resources[key] = (capture_id, observations)
                        expected_resource_observations = observations
                        if observations >= 2:
                            scene_resources[key] = ["incoming", capture_id, 0]
                            del pending_resources[key]
                            expected_admissions += 1
                if int(world.get("individual_observations", 0)) != expected_resource_observations:
                    raise _fail(path, frame, "per-resource observation debounce differs")

            pending_resources = {
                key: value
                for key, value in pending_resources.items()
                if value[0] + 1 >= capture_id
            }
            for key, state in list(scene_resources.items()):
                if state[0] == "outgoing" or key in individually_valid_keys:
                    continue
                state[2] = state[2] + 1 if state[1] + 1 == capture_id else 1
                state[1] = capture_id
                if state[2] >= 2:
                    del scene_resources[key]
                    expected_revocations += 1

            if int(catalog.get("individually_validated_resources", -1)) != len(
                individually_valid_keys
            ):
                raise _fail(path, frame, "validated-resource aggregate differs")
            if int(catalog.get("individually_admitted_resources", -1)) != expected_admissions:
                raise _fail(path, frame, "admitted-resource aggregate differs")
            if int(catalog.get("individually_revoked_resources", -1)) != expected_revocations:
                raise _fail(path, frame, "revoked-resource aggregate differs")
            if individually_valid_keys:
                individually_validated_captures += 1
            individually_admitted_resources += expected_admissions
            individually_revoked_resources += expected_revocations

        if row_index and generation < int(previous_generation):
            raise _fail(path, frame, "active generation moved backwards")
        if previous_generation is not None and generation != previous_generation:
            if not committed or generation != previous_generation + 1:
                raise _fail(path, frame, "generation changed without one atomic commit")
        if old_active is not None and not committed and active_fingerprint != old_active:
            raise _fail(path, frame, "active fingerprint changed without commit")

        current_differs = bool(old_keys) and current_keys != old_keys
        if not validated:
            invalid_holds += 1
            expected_candidate = None
            expected_observations = 0
            if candidate or int(catalog.get("candidate_observations", 0)) != 0:
                raise _fail(path, frame, "invalid table advanced a candidate")
            if enabled and old_keys:
                if not presentation_valid or not int(
                    catalog.get("active_cohort_reprojected", 0)
                ):
                    raise _fail(path, frame, "invalid table did not reproject active cohort")
                if int(catalog.get("reprojected_active_resources", -1)) != len(
                    old_keys
                ):
                    raise _fail(path, frame, "active reproject resource count differs")
        elif not old_keys or current_differs:
            candidate_captures += 1
            if not candidate:
                raise _fail(path, frame, "new valid set is not marked as candidate")
            if expected_candidate != current_keys:
                expected_candidate = current_keys
                expected_observations = 1
            else:
                expected_observations += 1
            traced_observations = int(catalog.get("candidate_observations", 0))
            if row_index and traced_observations != expected_observations:
                raise _fail(path, frame, "candidate observation count is not consecutive")
            if old_keys:
                if traced_shared != shared:
                    raise _fail(path, frame, "shared-resource count differs")
                disjoint = shared == 0
                if bool(int(catalog.get("cohort_disjoint", 0))) != disjoint:
                    raise _fail(path, frame, "disjoint-transition classification differs")
                if enabled and not committed and not int(
                    catalog.get("candidate_cohort_presented", 0)
                ):
                    raise _fail(path, frame, "valid incoming candidate was not presented")
                if enabled and not committed and disjoint and int(
                    catalog.get("retained_resources", 0)
                ):
                    raise _fail(path, frame, "disjoint candidate retained the old scene")
            if committed:
                if traced_observations < 2:
                    raise _fail(path, frame, "candidate committed before debounce")
                if active_fingerprint != current_fingerprint:
                    raise _fail(path, frame, "commit did not activate current resource set")
                if old_keys:
                    outgoing = len(set(old_keys).difference(current_keys))
                    if shared == 0:
                        expected_retained = 0
                    else:
                        expected_retained = outgoing
                    if enabled and int(catalog.get("retained_resources", -1)) != expected_retained:
                        raise _fail(path, frame, "committed outgoing resource count differs")
                    transitions.append(
                        {
                            "frame": frame,
                            "capture_id": int(catalog.get("capture_id", 0)),
                            "from_fingerprint": old_active,
                            "to_fingerprint": current_fingerprint,
                            "shared_resources": shared,
                            "outgoing_resources": outgoing,
                            "incoming_resources": len(
                                set(current_keys).difference(old_keys)
                            ),
                            "disjoint": shared == 0,
                        }
                    )
                for resource_id, geometry in current_keys:
                    geometry_by_id[resource_id] = geometry
                expected_candidate = None
                expected_observations = 0
        else:
            expected_candidate = None
            expected_observations = 0
            if candidate or committed or int(catalog.get("candidate_observations", 0)):
                raise _fail(path, frame, "stable active set retained candidate state")
            if active_fingerprint != current_fingerprint:
                raise _fail(path, frame, "stable set does not equal active fingerprint")

        if committed and not old_keys:
            for resource_id, geometry in current_keys:
                geometry_by_id[resource_id] = geometry

        if catalog_schema >= 4:
            if committed:
                if not old_keys or shared == 0:
                    scene_resources.clear()
                    pending_resources.clear()
                else:
                    scene_resources = {
                        key: ["outgoing", int(catalog.get("capture_id", 0)), 0]
                        for key in set(old_keys).difference(current_keys)
                    }
            presented_active = current_keys if committed else old_keys
            expected_scene_count = len(presented_active) + len(scene_resources)
            traced_scene_count = int(catalog.get("scene_cache_resources", -1))
            if traced_scene_count != expected_scene_count:
                raise _fail(
                    path,
                    frame,
                    f"scene-cache size differs: trace={traced_scene_count}, "
                    f"audit={expected_scene_count}",
                )
            presented_keys = set(presented_active).union(scene_resources)
            for world in worlds:
                expected_member = _world_key(world) in presented_keys
                if bool(int(world.get("scene_cache_member", 0))) != expected_member:
                    raise _fail(path, frame, "per-resource scene membership differs")
            maximum_scene_resources = max(maximum_scene_resources, expected_scene_count)
        previous_active = active_fingerprint
        previous_generation = generation

    return {
        "schema": REPORT_SCHEMA,
        "source": str(path),
        "captures": len(decoded),
        "invalid_partial_table_holds": invalid_holds,
        "candidate_captures": candidate_captures,
        "committed_transitions": len(transitions),
        "overlapping_transitions": sum(not item["disjoint"] for item in transitions),
        "disjoint_transitions": sum(item["disjoint"] for item in transitions),
        "final_active_generation": previous_generation,
        "final_active_fingerprint": previous_active,
        "individually_validated_captures": individually_validated_captures,
        "individually_admitted_resources": individually_admitted_resources,
        "individually_revoked_resources": individually_revoked_resources,
        "maximum_scene_resources": maximum_scene_resources,
        "resource_geometry_variant_ids": len(resource_geometry_variants),
        "resource_geometry_variants": resource_geometry_variants,
        "authored_placement_resource_variant_count": len(
            authored_placement_resource_variants
        ),
        "authored_placement_resource_variants": (
            authored_placement_resource_variants
        ),
        "simultaneous_authored_placement_conflict_count": len(
            simultaneous_placement_conflicts
        ),
        "simultaneous_authored_placement_conflicts": (
            simultaneous_placement_conflicts
        ),
        "transitions": transitions,
        "qualification": (
            "Every result is derived from sorted stable EID plus immutable-geometry "
            "keys. Frames, level IDs, coordinates and expected section sizes are "
            "reported only as observations and never select or validate a handoff. "
            "Placement reuse and topology variants are diagnostics, not automatic "
            "LOD authorization."
        ),
    }


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("trace", type=Path)
    parser.add_argument("--output", type=Path)
    parser.add_argument("--compact", action="store_true")
    args = parser.parse_args()
    try:
        report = analyze_transition_trace(args.trace)
    except (OSError, ValueError) as exc:
        print(f"crash3_resident_transition_audit: {exc}", file=sys.stderr)
        return 2
    payload = json.dumps(report, indent=None if args.compact else 2, sort_keys=True)
    if args.output:
        args.output.write_text(payload + "\n", encoding="utf-8")
    else:
        print(payload)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
