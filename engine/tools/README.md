# tools/

Scripts and helper binaries used during development and PsyQ signature
generation. Nothing here is required to build or run the project -- the main
pipeline lives in `ps1Analyzer/`, `ps1Recomp/` and `ps1Runtime/`.

## Pipeline tooling (user-facing)

| File | Purpose |
|------|---------|
| `extract_psyq_signatures.py` | Hash every function in a PsyQ `.LIB` archive and emit/update `ps1Analyzer/data/psyq_signatures.toml`. Run once per SDK release added. |
| `psyq_lib_extract.py` | Split a SN Systems `.LIB` archive into per-member `.OBJ` files (helper for the script above). |
| `cross_reference_psyz_symbols.py` | Cross-reference our signature DB against the [psyz](https://github.com/Xeeynamo/psyz) PSY-Q 4.0/4.7 symbol maps. Useful when checking gap coverage. |
| `validate_game.py` | End-to-end driver: build, run a game config for N seconds, snapshot VRAM, return a structured report. |
| `crash3_checkpoint_matrix.py` | Replay the immutable warp/new-game/stream/demo corpus. Accumulated failure counts retain transient problems even after a clean final frame. `--benchmark` selects clean production Vulkan 4x with the native raster retained and a 1 ms host clock; `--minimum-rate 50` makes insufficient capacity fail explicitly. Reports distinguish capacity from paced/diagnostic runs and record binary/configuration hashes. These structural checks do not certify final image coverage or motion. |
| `run_and_report.py` | Lightweight runtime probe used by `validate_game.py`. |
| `depth_family_trace.py` | Stream and join opt-in MIPS, GTE, GP0 and DMA/OT provenance; report exact fixed-point affine candidates plus complete source-sensitive and address-elided model-DAG digests. It recomputes the runtime Merkle pair, audits it against full SHA-256 payloads and verifies its GTE-to-GP0 transport. These are categorical construction records, explicitly not object/family identities. Runtime captures should use the exact-name `PS1_TRACE_EVENTS` allowlist documented in `VISIBILITY_AUDIT.md`. |
| `depth_capture_compare.py` | Certify that a heavy checkpoint replay has exactly the same ordered GTE, GP0 and DMA/OT guest stream as its light oracle while normalizing only process-local IDs, host timing metadata and the heavy pass's explicit value-flow fields/bit. It also requires complete MIPS value flow for every captured projection. |
| `visibility_audit_trace.py` | Validate dual-owner Vulkan JSONL traces, reconcile every per-frame classification and directed-pair sample count, reject empty captures when requested, and emit a compact summary. It explicitly performs no family clustering, identity inference or factor fitting. |
| `visibility_lineage_trace.py` | Validate every conflict-referenced `replay.visibility_owner` and any opt-in complete eligible-owner catalog, then measure categorical recurrence across disjoint discovery/holdout captures. Equivalent light/heavy GTE/GP0/DMA streams can add complete model-DAG SHA-256 payloads and exact numeric affines. For unambiguous recurrent triangles the tool verifies exact shared-projection incidence and cross-validates the same/different complete-affine relation while discarding process-local projection IDs and numeric matrices as persistent identities. It discovers frames whose conflict endpoints are wholly evidenced, and reports conservative exact D32 ratio inequalities between recurrent affine-relation components for later candidate rejection. Missing relations are never separation evidence; split/merge is rejected; neither those inequalities nor exact rational proportionality select or certify a gauge, and integer/tenth or family-count priors never filter the result. |
| `visibility_stack_trace.py` | Validate the opt-in Vulkan accepted-fragment stack, including capacity/overflow, spatial boundaries, exact final OT owner/D32 reconciliation, complete owner catalog and every closer physical owner. It joins all endpoints to intrinsic v2 runtime keys and an optional base topology, but reports unmapped shared-projection anchors only as capture-planning evidence. No fixed layer/family limit or multiplier fitting is used. |
| `visibility_stack_cross_validate.py` | Compare disjoint stack discovery and holdout reports. An unmapped endpoint is admitted only when its complete runtime key and nonempty exact vertex-incidence relation set are identical in both; variable, empty, duplicated and single-set endpoints remain explicit. |
| `visibility_stack_dispatch_refine.py` | Cross-audit admitted endpoint fingerprint pairs against complete MIPS DAGs in at least two heavy captures already certified equivalent by the base lineage report. Emit a v3 runtime-lookup refinement while retaining base component hashes only as immutable anchors. It does not rehash the base graph, infer an object/family count or choose a presentation multiplier. |
| `visibility_stack_context_refine.py` | Split otherwise-ambiguous v2 keys by their exact same-frame vertex incidence with already-static v3 owners. A contextual occurrence is admitted only when its complete nonempty relation recurs in discovery/holdout, has no within-frame duplicate and its compact fingerprints reproduce the same full MIPS DAGs in two trusted heavy captures. Recurrent multi-anchor contexts form a separately hashed topology quotient; projection-ID values are discarded, base hashes remain immutable, quotient labels are not families and the base key is never promoted to persistent identity. |
| `visibility_stack_constraints.py` | Apply the exact v3 base-plus-attachment lookup, plus an optional admitted frame-local context map, to every physical-occluder pair in complete discovery/holdout stacks. It emits exact-rational conservative D32 bounds, cross-set edge coverage and strongly connected components while leaving unresolved endpoints, one-set edges and self conflicts as explicit blockers. No observed component/owner count or integer/tenth prior limits or selects the graph. |
| `visibility_gauge_candidate.py` | Check a caller-authored exact-rational presentation multiplier for every component in a complete visibility-lineage constraint report. Missing/extra components, truncated reports, non-positive values and equality at a strict bound fail closed. The tool never proposes or rounds a factor; a pass only admits the candidate to the independent lifted Vulkan owner/depth oracle. |
| `widescreen_edge_trace.py` | Validate one complete visibility or presented-surface owner-schema-v3 catalog, intersect every accepted high-precision triangle (including semitransparent surfaces in the latter domain) with the requested 16:9 viewport edges, and report exact vertical coverage/gaps grouped by guest renderer PC and intrinsic lineage/DMA counts. It measures admitted geometry rather than inferring culling from final color pixels. |
| `crash3_resident_coverage.py` | Rasterize a traced complete Crash resident catalog before Vulkan, weld exact projected X/Y/W edges and optionally compare it with a physical color readback. This separates topology holes from later depth/material/discard loss and accepts catalog schema 1-4 plus face schema 1-3. Current schema-4 traces require the opt-in `PS1_TRACE_CRASH3_RESIDENT_FACES=1`. |
| `crash3_temporal_phase_validate.py` | Validate resolved Vulkan A/M/B scanout sequences: contiguous presentation serials, midpoint/endpoint alternation, visible midpoint distinctness on moving pairs, and optional byte-exact endpoint matching against an interpolation-off control capture. |
| `crash3_resident_transition_audit.py` | Reconstruct every resident-section transaction from sorted stable EID plus immutable-geometry keys. Schema v3 independently checks per-capture EID uniqueness, conflicts with committed/pending/cached geometry, partial-row payload/placement witnesses, consecutive individual admission, symmetric revocation, complete cohort debounce, active-generation commits, outgoing overlap and disjoint-scene clearing. It also reports cross-time topology/placement reuse and simultaneous placement conflicts as non-authorizing LOD diagnostics. No save slot, level ID, frame trigger, camera coordinate, EID list or expected section size is hard-coded; face records are prefiltered and are not required. |
| `crash3_model_variant_audit.py` | Classify attached object-model changes as program-only, animation topology, model-resource switch, multipart composition or unclassified topology. Face count remains observational: the report never authorizes “largest mesh wins” without intrinsic model-family and pose correspondence. |
| `crash3_depth_plane_audit.py` | Inspect lossless pre-correction D32, presented-surface R32 or operational-occlusion D32 PFM captures independently from the F8 palette. It preserves exact IEEE-754 identity, separates continuous scene depth, explicit far background and the exact 1.0 clear sentinel, reports log2(GTE W) quantiles and can fail a run whose scene really collapses to too few values. |

## Dev-only helpers (not needed for normal contributions)

For a repeatable capacity comparison without recompiling, run the same scenarios
with `--benchmark --frames 300`, then repeat with
`--set-env PS1_CRASH3_VULKAN_VERTEX_INTERPOLATION=0`. The preset clears inherited
`PS1_*` experiments; explicit `--set-env` values are applied afterwards. Do not
confuse `--paced`/VSync with the separate guest-refresh pacing: disabling VSync
alone still leaves the native 50 Hz clock active. Rates are guest VBlanks/s,
not unique displayed frames/s. Heavy traces/readbacks and omitted native raster
invalidate the optional capacity rate gate.

| Path | Purpose |
|------|---------|
| `ghidra/` | Ghidra scripts (`ExportPS1Functions.{java,py}`) to export discovered functions from a Ghidra project as a CSV that `ps1Analyzer` can ingest via `--ghidra-csv`. Plus `start_ghidra_mcp.sh` for the Ghidra MCP integration. |
| `pcsx_redux_mcp/` | Standalone MCP server + Lua scripts that drive PCSX-Redux for cross-checking emulator behaviour against the recompiled runtime. Optional. |
| `ps1_mcp_server.py` | MCP server exposing build/run/log/source tools to AI agents during development. Not part of the runtime build. |
| `demo_build.sh` | Quick smoke build script used while iterating locally. |
| `tests/` | Python tests for the trace/analyzer helpers and PsyQ tools. Run the complete set with `pytest tools/tests/`; the depth-family analyzer alone also runs with `python3 -m unittest tools.tests.test_depth_family_trace`. |

## Generated / external binaries (gitignored)

| Path | Reason |
|------|--------|
| `psyq-obj-parser-bin/psyq-obj-parser` | Built from the [pcsx-redux](https://github.com/grumpycoders/pcsx-redux) `psyq-obj-parser` source. Required by `psyq_lib_extract.py`. Build instructions in the pcsx-redux repo. |

## PsyQ SDK location

The signature generator expects PsyQ SDK `.LIB` archives **outside the repository**
(licensing -- we do not redistribute Sony binaries). Default search root is
`$HOME/psyq_sdks/CONSOLIDATED/<version>/lib/`. Override with `--sdk-root`.
