import hashlib
import json
import os
import tempfile
import unittest
from pathlib import Path
from unittest import mock

from tools.crash3_checkpoint_matrix import (
    SCHEMA,
    diagnostic_window_title,
    diagnostic_capture_failures,
    load_manifest,
    input_implementation,
    materialize_slot_zero,
    measurement_contract,
    modern_metric_failures,
    parse_environment_overrides,
    rate_failures,
    scenario_environment,
    shader_implementation,
    summarize_metrics,
)


class Crash3CheckpointMatrixTest(unittest.TestCase):
    def test_raw_endpoint_loss_is_not_a_complete_modern_frame(self):
        key = 'host.renderer.crash3_profile.resident_raw_endpoint_unavailable'
        summary = summarize_metrics({'counters': {key: 4}, 'state': {key: 0}})
        self.assertEqual(summary['resident_raw_endpoint_unavailable'], 4)
        failures = modern_metric_failures(summary, full_scene_sidecar=True)
        self.assertIn('resident scene lost both raw and projected modern endpoints', failures)

    def test_missing_or_rejected_visual_capture_is_not_a_structural_pass(self):
        env={'PS1_VULKAN_REPLAY_READBACK':'all',
             'PS1_VULKAN_REPLAY_READBACK_DUMP_DIRECTORY':'captures'}
        self.assertTrue(diagnostic_capture_failures('normal successful execution',env))
        self.assertTrue(diagnostic_capture_failures('[RendererVK] replay readback rejected: invalid',env))
        self.assertEqual(diagnostic_capture_failures('[RendererVK] replay readback frame 123 captured',env),[])
        self.assertEqual(diagnostic_capture_failures('ordinary non-visual test',{}),[])

    def test_quiet_gpu_capture_uses_completed_writes_not_stdout_or_stale_files(self):
        env = {'PS1_VULKAN_REPLAY_READBACK': 'all',
               'PS1_VULKAN_REPLAY_READBACK_DUMP_DIRECTORY': 'captures'}
        prefix = 'host.renderer.vulkan_replay_readback.'
        counters = {prefix + 'completed': 2, prefix + 'dump_written': 2}
        self.assertEqual(diagnostic_capture_failures('', env, {'counters': counters}), [])
        marker = '[RendererVK] replay readback frame 123 captured'
        self.assertTrue(diagnostic_capture_failures(marker, env, {'counters': {}}))
        self.assertTrue(diagnostic_capture_failures('', env, {'counters': {
            **counters, prefix + 'dump_failed': 1}}))
        env['PS1_VULKAN_REPLAY_READBACK_DEPTH_DUMP_DIRECTORY'] = 'depth'
        self.assertTrue(diagnostic_capture_failures('', env, {'counters': counters}))
        counters[prefix + 'depth_dump_written'] = 2
        self.assertEqual(diagnostic_capture_failures('', env, {'counters': counters}), [])

    def test_executed_input_overrides_are_fingerprinted(self):
        with tempfile.TemporaryDirectory() as temporary:
            path = Path(temporary) / "orbit.inputs"
            path.write_text("3728 4026 left\n")
            result = input_implementation({"PS1_INPUT_SCRIPT": str(path)})
            self.assertEqual(result["path"], str(path))
            self.assertEqual(result["sha256"], hashlib.sha256(path.read_bytes()).hexdigest())
            self.assertIsNone(input_implementation({})["sha256"])

    def test_automatic_controls_are_named_in_the_window(self):
        environment = {
            "PS1_INTERNAL_RES_SCALE": "4",
            "PS1_CRASH3_VULKAN_VERTEX_INTERPOLATION": "2",
            "PS1_CRASH3_VULKAN_SCENE_SIDECAR": "1",
        }
        title = diagnostic_window_title("newgame.full_intro", environment)
        self.assertIn("AUTOMATED TEST", title)
        self.assertIn("newgame.full_intro", title)
        self.assertIn("Vulkan 4x", title)
        self.assertNotIn("ALTERNATIVE", title)
        control = diagnostic_window_title("demo", {
            **environment, "PS1_CRASH3_VULKAN_VERTEX_INTERPOLATION": "0",
            "PS1RECOMP_VULKAN_SHADER_DIR": "/tmp/old-shaders",
        })
        self.assertIn("interpolation 0x", control)
        self.assertIn("ALTERNATIVE SHADERS", control)
        isolated = diagnostic_window_title("demo", {
            **environment, "PS1_CRASH3_VULKAN_TEMPORAL_DISABLE_BACKGROUND": "1",
        })
        self.assertIn("BACKGROUND INTERPOLATION OFF", isolated)

    def test_shader_provenance_changes_without_executable_rebuild(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            runtime = root / "runtime"
            shaders = root / "shaders"
            shaders.mkdir()
            module = shaders / "replay_precise.vert.spv"
            module.write_bytes(b"before")
            before = shader_implementation(runtime, {})
            module.write_bytes(b"after")
            after = shader_implementation(runtime, {})
            self.assertNotEqual(before["sha256"], after["sha256"])
            self.assertEqual(after["files"][module.name], hashlib.sha256(b"after").hexdigest())
            missing = shader_implementation(runtime, {"PS1RECOMP_VULKAN_SHADER_DIR": str(root / "absent")})
            self.assertEqual(missing["resolution"], "unknown_compiled_fallback")
            self.assertIsNone(missing["sha256"])

    def test_surface_coverage_distinguishes_all_camera_selected_walkers(self):
        counts = {
            "terrain_forward": 11, "terrain_reverse": 13,
            "water_forward": 17, "water_reverse": 19,
        }
        summary = summarize_metrics({"counters": {
            "host.crash3.presentation_cull." + walker: count
            for walker, count in counts.items()
        }})
        self.assertEqual(summary["procedural_surface_cull_walkers"], counts)
        self.assertEqual(
            summarize_metrics({})["procedural_surface_cull_walkers"],
            dict.fromkeys(counts, 0),
        )

    def test_temporal_construction_errors_are_cumulative_but_atomic_snap_is_safe(self):
        summary = summarize_metrics({"counters": {
            "host.renderer.vertex_interpolation.replay_primitive_atomic_snap_triangle": 7,
            "host.renderer.vertex_interpolation.replay_batch_shape_mismatch": 1,
            "host.crash3.procedural_identity.terrain_invalid_walker": 2,
            "host.crash3.procedural_identity.terrain_invalid_grid": 3,
            "host.crash3.procedural_identity.terrain_grid": 42,
        }})
        self.assertEqual(summary["vertex_interpolation_replay_atomic_snapped_triangles"], 7)
        self.assertEqual(summary["procedural_identity_terrain_invalid"], 5)
        self.assertEqual(summary["procedural_identity_terrain_grid"], 42)
        failures = modern_metric_failures(summary)
        self.assertIn("temporal replay metadata disagreed with the current vertex batch", failures)
        self.assertIn("a procedural terrain vertex lacked intrinsic grid identity", failures)
        safe = summarize_metrics({"counters": {
            "host.renderer.vertex_interpolation.replay_primitive_atomic_snap_triangle": 7,
        }})
        self.assertEqual(modern_metric_failures(safe), modern_metric_failures(summarize_metrics({})))

    def test_intermittent_failures_survive_a_clean_final_frame(self):
        expected = {
            "object_supplement_material_mismatches": "decoded object material contradicts an emitted packet",
            "object_supplement_color_mismatches": "decoded object color contradicts an emitted packet",
            "object_supplement_unattached_host_intersecting_faces": "a visible object mesh has no exact renderer attachment",
            "object_supplement_unresolved_material": "object faces remain without a certified material",
            "object_supplement_unresolved_semi_faces": "object semitransparency remains unresolved",
            "object_supplement_invalid_depth": "object faces have invalid presentation depth",
            "resident_supplement_unresolved_faces": "resident scenery faces remain unresolved",
            "resident_color_mismatch": "decoded scenery color contradicts an emitted packet",
            "resident_material_type_mismatch": "decoded scenery material contradicts an emitted packet",
            "resident_material_uv_mismatch": "decoded scenery material contradicts an emitted packet",
        }
        for suffix, failure in expected.items():
            with self.subTest(metric=suffix):
                key = "host.renderer.crash3_profile." + suffix
                summary = summarize_metrics({"counters": {key: 4}, "state": {key: 0}})
                self.assertIn(failure, modern_metric_failures(summary))

    def test_resident_gate_reads_the_actual_runtime_key_for_legacy_reports(self):
        key = "host.renderer.crash3_profile.resident_supplement_unresolved_faces"
        summary = summarize_metrics({"state": {key: 7}})
        self.assertEqual(summary["resident_unresolved_faces"], 7)
        # Final-frame count is already included in the accumulated occurrences.
        summary = summarize_metrics({"counters": {key: 9}, "state": {key: 7}})
        self.assertEqual(summary["resident_unresolved_faces"], 9)

    def test_resident_material_channels_accumulate_without_double_counting_state(self):
        keys = ["host.renderer.crash3_profile.resident_material_" + kind + "_mismatch"
                for kind in ("type", "tpage", "clut", "uv", "domain")]
        summary = summarize_metrics({"counters": dict.fromkeys(keys, 3),
                                     "state": dict.fromkeys(keys, 1)})
        self.assertEqual(summary["resident_material_mismatches"], 15)
        self.assertIn("decoded scenery material contradicts an emitted packet",
                      modern_metric_failures(summary))

    def test_disabling_vsync_does_not_prove_unpaced_capacity(self):
        contract = measurement_contract({"PS1_PRESENT_VSYNC": "0"})
        self.assertEqual(contract["kind"], "paced_or_diagnostic")
        self.assertTrue(contract["native_refresh_pacing"])
        self.assertTrue(rate_failures({"effective_vblanks_per_second": 200}, contract, 50))

    def test_capacity_gate_keeps_native_raster_and_diagnostics_explicit(self):
        environment = {"PS1_PRESENT_VSYNC": "0", "PS1_VBLANK_PERIOD_US": "1000"}
        contract = measurement_contract(environment)
        self.assertEqual(contract["kind"], "capacity")
        self.assertTrue(rate_failures({"effective_vblanks_per_second": 42}, contract, 50))
        self.assertEqual(rate_failures({"effective_vblanks_per_second": 60}, contract, 50), [])
        for flag in ("PS1_TRACE_CRASH3_MODEL_VARIANTS", "PS1_VULKAN_REPLAY_READBACK",
                     "PS1_VULKAN_OMIT_PROJECTED_NATIVE_RASTER_DIAGNOSTIC"):
            with self.subTest(flag=flag):
                contaminated = measurement_contract({**environment, flag: "1"})
                self.assertNotEqual(contaminated["kind"], "capacity")

    def test_benchmark_removes_inherited_experiments_and_matches_production(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            scenario = load_manifest(self.fixture(root))[0]
            with mock.patch.dict(os.environ, {
                "PS1_VULKAN_OMIT_PROJECTED_NATIVE_RASTER_DIAGNOSTIC": "1",
                "PS1_VULKAN_REPLAY_READBACK": "all",
                "PS1_CRASH3_VULKAN_TEMPORAL_DISABLE_OBJECT": "1",
                "PS1RECOMP_VULKAN_SHADER_DIR": "/tmp/inherited-old-shaders",
            }):
                environment = scenario_environment(
                    scenario, root / "run", 300, 4, False, True, benchmark=True
                )
            self.assertNotIn("PS1_VULKAN_REPLAY_READBACK", environment)
            self.assertNotIn("PS1_CRASH3_VULKAN_TEMPORAL_DISABLE_OBJECT", environment)
            self.assertNotIn("PS1RECOMP_VULKAN_SHADER_DIR", environment)
            self.assertEqual(environment["PS1_VULKAN_OMIT_PROJECTED_NATIVE_RASTER_DIAGNOSTIC"], "0")
            self.assertEqual(environment["PS1_CRASH3_VULKAN_VERTEX_INTERPOLATION"], "2")
            self.assertEqual(measurement_contract(environment)["kind"], "capacity")

    def fixture(self, root: Path, *, duplicate: bool = False) -> Path:
        checkpoint = root / "checkpoint.ps1s"
        checkpoint.write_bytes(b"PS1S fixture")
        script = root / "move.inputs"
        script.write_text("102 120 up\n", encoding="utf-8")
        digest = hashlib.sha256(checkpoint.read_bytes()).hexdigest()
        input_digest = hashlib.sha256(script.read_bytes()).hexdigest()
        scenarios = [
            {
                "id": "warp.left",
                "checkpoint": str(checkpoint),
                "checkpoint_sha256": digest,
                "restored_vblank": 100,
                "frames": 75,
                "input_script": str(script),
                "input_script_sha256": input_digest,
                "tags": ["warp"],
            }
        ]
        if duplicate:
            scenarios.append(dict(scenarios[0]))
        manifest = root / "manifest.json"
        manifest.write_text(
            json.dumps({"schema": SCHEMA, "scenarios": scenarios}),
            encoding="utf-8",
        )
        return manifest

    def test_materializes_verified_private_slot_and_modern_4x_environment(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            scenario = load_manifest(self.fixture(root))[0]
            self.assertEqual(scenario.frames, 75)
            run = root / "run"
            slot = materialize_slot_zero(scenario, run)
            self.assertEqual(slot.read_bytes(), b"PS1S fixture")
            self.assertNotEqual(slot.stat().st_ino, scenario.checkpoint.stat().st_ino)
            slot.write_bytes(b"diagnostic autosave")
            self.assertEqual(scenario.checkpoint.read_bytes(), b"PS1S fixture")
            environment = scenario_environment(
                scenario, run, frames=75, scale=4, paced=False, modern=True
            )
            self.assertEqual(environment["PS1_EXIT_AFTER_LOAD_VBLANKS"], "75")
            self.assertEqual(environment["PS1_INTERNAL_RES_SCALE"], "4")
            self.assertEqual(environment["PS1_PRESENT_VSYNC"], "0")
            self.assertEqual(environment["PS1_INPUT_SCRIPT"], str(scenario.input_script))
            self.assertEqual(
                environment["PS1_CRASH3_VULKAN_RESIDENT_SEMI_REPLACE"], "1"
            )
            self.assertEqual(environment["PS1_CRASH3_VULKAN_SCENE_SIDECAR"], "1")
            self.assertEqual(
                environment["PS1_CRASH3_VULKAN_SIDECAR_DEPTH"], "intrinsic"
            )
            self.assertEqual(environment["PS1_CRASH3_VULKAN_OBJECT_REPLACE"], "1")
            self.assertEqual(
                environment["PS1_CRASH3_VULKAN_OBJECT_SEMI_REPLACE"], "1"
            )
            self.assertEqual(
                environment["PS1_CRASH3_VULKAN_RESIDENT_GPU_OPAQUE_BATCH"], "1"
            )
            self.assertEqual(environment["PS1_CRASH3_VULKAN_GPU_RAW_SCENE"], "1")
            self.assertEqual(
                environment["PS1_CRASH3_VULKAN_GPU_DIRECT_MATERIAL"], "1"
            )
            self.assertEqual(environment["PS1_CRASH3_VULKAN_GPU_SEMI_BATCH"], "1")
            self.assertEqual(environment["PS1_CRASH3_VULKAN_GPU_RAW_SEMI"], "1")
            self.assertEqual(environment["PS1_CRASH3_OBJECT_MATCH_VERIFY"], "1")

    def test_rejects_duplicate_scenario_ids(self):
        with tempfile.TemporaryDirectory() as temporary:
            with self.assertRaisesRegex(ValueError, "duplicate scenario"):
                load_manifest(self.fixture(Path(temporary), duplicate=True))

    def test_matrix_prefers_native_wayland_over_exported_xwayland(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            (root / "wayland-test").touch()
            scenario = load_manifest(self.fixture(root))[0]
            with mock.patch.dict(
                os.environ,
                {
                    "WAYLAND_DISPLAY": "wayland-test",
                    "XDG_RUNTIME_DIR": str(root),
                    "DISPLAY": ":0",
                },
                clear=True,
            ):
                environment = scenario_environment(
                    scenario,
                    root / "run",
                    frames=75,
                    scale=4,
                    paced=False,
                    modern=True,
                )
            self.assertEqual(environment["SDL_VIDEODRIVER"], "wayland")
            self.assertEqual(environment["SDL_AUDIODRIVER"], "dummy")

    def test_headless_environment_keeps_real_runtime_without_presentation(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            scenario = load_manifest(self.fixture(root))[0]
            environment = scenario_environment(
                scenario,
                root / "run",
                frames=8,
                scale=1,
                paced=False,
                modern=False,
                headless=True,
            )
            self.assertEqual(environment["PS1_DIAGNOSTIC_HEADLESS"], "1")
            self.assertNotIn("PS1_CRASH3_VULKAN_SCENE_SIDECAR", environment)
            self.assertEqual(environment["PS1_EXIT_AFTER_LOAD_VBLANKS"], "8")

    def test_parses_repeatable_environment_overrides(self):
        self.assertEqual(
            parse_environment_overrides(["PS1_A=0", "PS1_B=debug", "PS1_A=1"]),
            {"PS1_A": "1", "PS1_B": "debug"},
        )
        with self.assertRaisesRegex(ValueError, "NAME=VALUE"):
            parse_environment_overrides(["PS1_A"])

    def test_summarizes_and_gates_complete_modern_metrics(self):
        counters = {
            "timing.vblank": 250,
            "host.renderer.vulkan_replay_route.m1_geometry": 30,
            "host.renderer.vulkan_replay_route.m1_latch": 220,
            "host.renderer.replay_coverage.m1_submitted.prefixes": 250,
            "host.renderer.replay_coverage.m1_submitted."
            "high_precision_complete_prefixes": 250,
            "host.renderer.replay_coverage.m1_submitted.projected_vertices": 99,
            "host.renderer.replay_coverage.m1_submitted."
            "projected_xy_precise_vertices": 99,
            "host.renderer.replay_coverage.m1_submitted."
            "projected_w_valid_vertices": 99,
            "host.renderer.vertex_interpolation."
            "object_model_variant_barrier": 2,
            "host.renderer.vertex_interpolation."
            "object_model_variant_snapped_triangle": 335,
            "host.renderer.vertex_interpolation."
            "object_model_program_transition": 7,
        }
        state = {
            "host.elapsed_us": 5_000_000,
            "timing.vblank_debt_peak": 2,
            "host.renderer.crash3_profile."
            "object_supplement_unattached_host_intersecting_faces": 0,
            "host.renderer.vertex_interpolation."
            "object_model_multipart_count": 1,
        }
        summary = summarize_metrics({"counters": counters, "state": state})
        self.assertEqual(summary["effective_vblanks_per_second"], 50.0)
        self.assertEqual(summary["object_unattached_host_intersecting_faces"], 0)
        self.assertEqual(
            summary["vertex_interpolation_object_model_variant_barriers"], 2
        )
        self.assertEqual(
            summary[
                "vertex_interpolation_object_model_variant_snapped_triangles"
            ],
            335,
        )
        self.assertEqual(
            summary["vertex_interpolation_object_model_program_transitions"], 7
        )
        self.assertEqual(
            summary["vertex_interpolation_object_model_multipart"], 1
        )
        self.assertEqual(modern_metric_failures(summary), [])

        full_scene_summary = dict(summary)
        full_scene_summary["object_replacement_requested"] = 1
        full_scene_summary["object_replacement_record_active"] = 1
        full_scene_summary["object_replacement_active"] = 1
        full_scene_summary["object_intrinsic_depth_proofs"] = 1
        full_scene_summary["object_original_color_shadowed"] = 20
        full_scene_summary["object_supplement_draw_groups"] = 4
        full_scene_summary["presentation_latch_submits_elided"] = 0
        full_scene_summary["object_margin_only_draw_groups"] = 0
        full_scene_summary["object_semi_draws"] = 5
        full_scene_summary["object_margin_only_semi_draws"] = 0
        full_scene_summary["presentation_projected_epochs"] = 1
        self.assertEqual(
            modern_metric_failures(
                full_scene_summary, full_scene_sidecar=True
            ),
            [],
        )
        interpolated_summary = dict(full_scene_summary)
        interpolated_summary.update(
            {
                "presentation_latch_pages_hidden": 5,
                "presentation_latch_submits_elided": 1,
                "vertex_interpolation_operational": 1,
                "vertex_interpolation_endpoint_pending": 0,
                "vertex_interpolation_current_ambiguous_vertices": 0,
                "vertex_interpolation_current_ambiguous_replay_vertices": 0,
                "vertex_interpolation_current_ambiguous_non_replay_vertices": 0,
                "vertex_interpolation_interpolated_triangles": 300,
                "vertex_interpolation_midpoints_presented": 4,
                "vertex_interpolation_midpoints_resolved": 4,
                "vertex_interpolation_endpoints_presented": 4,
                "vertex_interpolation_endpoints_resolved": 4,
                "vertex_interpolation_backups_recorded": 4,
                "vertex_interpolation_restores_recorded": 4,
            }
        )
        self.assertEqual(
            modern_metric_failures(
                interpolated_summary,
                full_scene_sidecar=True,
                vertex_interpolation_requested=True,
            ),
            [],
        )
        geometry_summary = dict(interpolated_summary)
        geometry_summary["presentation_latch_pages_hidden"] = 4
        geometry_summary["vertex_interpolation_endpoints_with_geometry"] = 1
        self.assertEqual(modern_metric_failures(
            geometry_summary, full_scene_sidecar=True,
            vertex_interpolation_requested=True), [])
        # A consecutive source scene can defer a real endpoint without adding
        # another midpoint. It still must be resolved and presented exactly.
        geometry_summary.update({
            "vertex_interpolation_consecutive_endpoints_deferred": 1,
            "vertex_interpolation_endpoints_presented": 5,
            "vertex_interpolation_endpoints_resolved": 5,
            "vertex_interpolation_endpoints_with_geometry": 2,
        })
        self.assertEqual(modern_metric_failures(
            geometry_summary, full_scene_sidecar=True,
            vertex_interpolation_requested=True), [])
        interpolated_summary["vertex_interpolation_endpoints_presented"] = 3
        self.assertEqual(
            modern_metric_failures(
                interpolated_summary,
                full_scene_sidecar=True,
                vertex_interpolation_requested=True,
            ),
            [
                "an endpoint was presented without an exact resolve",
                "midpoint and endpoint presentation counts diverged",
                "a canonical latch page has no proven presentation route",
            ],
        )
        procedural_summary = dict(interpolated_summary)
        procedural_summary["vertex_interpolation_endpoints_presented"] = 4
        procedural_summary["procedural_surface_cull_prepared"] = 1
        procedural_summary["procedural_surface_cull_recovered"] = 1
        procedural_summary["procedural_identity_water_invalid"] = 1
        self.assertIn(
            "a procedural water vertex lacked intrinsic grid identity",
            modern_metric_failures(
                procedural_summary,
                tags=("procedural-surface",),
                full_scene_sidecar=True,
                vertex_interpolation_requested=True,
            ),
        )
        full_scene_summary["object_supplement_page_rejects"] = 1
        full_scene_summary["object_depth_guide_active"] = 1
        full_scene_summary["object_depth_guide_draws"] = 100
        self.assertEqual(
            modern_metric_failures(
                full_scene_summary, full_scene_sidecar=True
            ),
            [
                "a full-scene object capture was rejected",
                "the diagnostic object depth guide ran operationally",
            ],
        )
        full_scene_summary["object_supplement_page_rejects"] = 0
        full_scene_summary["object_depth_guide_active"] = 0
        full_scene_summary["object_depth_guide_draws"] = 0
        full_scene_summary["presentation_canvas_discontinuities"] = 1
        full_scene_summary["presentation_native_fallback_visible"] = 1
        self.assertEqual(
            modern_metric_failures(
                full_scene_summary, full_scene_sidecar=True
            ),
            [
                "the full-scene canvas changed inside one 3D epoch",
                "a native page became visible inside the modern epoch",
            ],
        )

        summary["projected_w_valid_vertices"] = 98
        summary["fallback_packets"] = 1
        summary["object_distance_cue_conflicts"] = 2
        summary["object_match_spatial_false_misses"] = 3
        summary["object_intrinsic_depth_mismatches"] = 4
        summary["object_intrinsic_depth_uncertified_source_absent_faces"] = 5
        summary["object_exact_face_depth_fallbacks"] = 6
        summary["object_replacement_requested"] = 1
        summary["object_replacement_record_active"] = 1
        summary["object_original_color_shadowed"] = 2
        summary["object_supplement_draw_groups"] = 4
        summary["object_margin_only_draw_groups"] = 3
        summary["object_semi_draws"] = 5
        summary["object_margin_only_semi_draws"] = 4
        summary["procedural_surface_cull_bound_mismatches"] = 1
        summary["object_unattached_host_intersecting_faces"] = 7
        summary["object_ordered_semi_faces"] = 4
        self.assertEqual(
            modern_metric_failures(summary),
            [
                "not every Projected3D vertex has authentic W",
                "fallback packets reached M1",
                "an object used multiple DPCT tuples in one node",
                "the object spatial index missed an exact face match",
                "intrinsic object OT depth contradicted an emitted packet",
                "source-absent object faces lack intrinsic OT depth",
                "object faces fell back to per-packet depth",
                "the operational matrix requested object replacement",
                "object replacement reached Vulkan recording",
                "an original object color face was shadowed",
                "an opaque host object draw escaped the 16:9 margins",
                "a semitransparent host object draw escaped the 16:9 margins",
                "a procedural surface used an unexpected cull bound",
                "a visible object mesh has no exact renderer attachment",
                "ordered object semitransparency was not drawn",
            ],
        )

    def test_uses_only_post_load_interval_for_performance(self):
        counters = {
            "timing.vblank": 250,
            "host.renderer.replay_coverage.m1_submitted.prefixes": 1,
            "host.renderer.replay_coverage.m1_submitted."
            "high_precision_complete_prefixes": 1,
        }
        state = {
            "host.elapsed_us": 10_000_000,
            "host.elapsed_after_load_us": 2_000_000,
            "timing.vblanks_after_load": 120,
        }
        summary = summarize_metrics({"counters": counters, "state": state})
        self.assertEqual(summary["effective_vblanks_per_second"], 60.0)

    def test_highest_detail_losses_are_explicit_gate_failures(self):
        cases = {
            "host.crash3.resident_sidecar.highest_material_unavailable":
                "an authored highest-detail scenery material was unavailable",
            "host.renderer.crash3_profile.scenery_texture_detail_invalid":
                "a highest-detail scenery material lost its exact face binding",
            "host.renderer.crash3_profile.scenery_texture_detail_fallback":
                "a highest-detail scenery material could not reach presentation",
        }
        for metric, failure in cases.items():
            with self.subTest(metric=metric):
                summary = summarize_metrics({"counters": {metric: 1}})
                self.assertIn(failure, modern_metric_failures(summary))

    def test_variant_selection_cannot_silently_restore_the_low_face(self):
        for suffix, prefix in (
            ("path_invalid", "host.crash3.scenery_variants."),
            ("source_selection_invalid", "host.crash3.scenery_variants."),
            ("path_contradiction", "host.crash3.scenery_variants."),
            ("missing_replacement", "host.renderer.crash3_profile.scenery_variant_"),
            ("plan_not_committed", "host.renderer.crash3_profile.scenery_variant_"),
        ):
            with self.subTest(suffix=suffix):
                summary = summarize_metrics({"counters": {prefix + suffix: 1}})
                self.assertIn(
                    f"scenery variant selection failed: scenery_variant_{suffix}",
                    modern_metric_failures(summary),
                )

    def test_object_construction_failures_remain_visible(self):
        for metric, failure in {
            "host.crash3.object_normal_constructor_rejected":
                "a selected normal-lighting constructor could not be decoded",
            "host.crash3.object_source_preflight_rejected":
                "a source object bounding-box preflight could not be prepared",
        }.items():
            with self.subTest(metric=metric):
                rejected = summarize_metrics({"counters": {metric: 1}})
                self.assertIn(failure, modern_metric_failures(rejected))

    def test_requires_intrinsic_surface_coverage_for_tagged_checkpoints(self):
        counters = {
            "timing.vblank": 10,
            "host.renderer.vulkan_replay_route.m1_geometry": 10,
            "host.renderer.replay_coverage.m1_submitted.prefixes": 10,
            "host.renderer.replay_coverage.m1_submitted."
            "high_precision_complete_prefixes": 10,
            "host.renderer.replay_coverage.m1_submitted.projected_vertices": 30,
            "host.renderer.replay_coverage.m1_submitted."
            "projected_xy_precise_vertices": 30,
            "host.renderer.replay_coverage.m1_submitted."
            "projected_w_valid_vertices": 30,
        }
        summary = summarize_metrics(
            {"counters": counters, "state": {"host.elapsed_us": 1000}}
        )
        self.assertEqual(
            modern_metric_failures(summary, ("procedural-surface",)),
            [
                "the procedural surface cull path was not exercised",
                "the procedural surface recovered no 16:9 edge",
            ],
        )
        summary["procedural_surface_cull_prepared"] = 8
        summary["procedural_surface_cull_recovered"] = 3
        self.assertEqual(
            modern_metric_failures(summary, ("procedural-surface",)), []
        )


if __name__ == "__main__":
    unittest.main()
