#!/usr/bin/env python3
"""Audit the curated effects census against a pinned FoxdieTeam source tree.

This is a source inventory, not an execution/visual coverage test. It neither
loads a disc nor modifies the donor checkout, game state, or renderer.
"""

import argparse
import hashlib
import json
from pathlib import Path
import re
import sys
from collections import Counter


ROOT = Path(__file__).resolve().parents[1]
# Deliberately narrow, reproducible first pass. Indirect emitters, packet
# mutations and ASM are accounted for separately by the curated source lists.
GRAPHICS = re.compile(
    r"\b(?:DG_GetPrim|DG_GetPrimBuffer|DG_SetChanlSystemUnits|"
    r"DG_SetExtPaletteMakeFunc|DG_SetRGB|DG_SetObjectTexture|SetDrawSTP|"
    r"setDrawStp|SetDrawMove|MoveImage|StoreImage|LoadImage|setPolyFT4|"
    r"setPolyGT4|setPolyG4|setSprt|setTile|SetSemiTrans|setSemiTrans|"
    r"gte_dpq\w*|DG_SetAmbient|DG_SetLight\w*)\b"
)
FUNCTION = re.compile(
    r"^\s*(?:(?:static|inline|extern)\s+)*"
    r"(?:void|int|short|long|u_short|GV_ACT|DG_[A-Z_]+|POLY_[A-Z0-9]+)"
    r"(?:\s*\*+\s*|\s+)([A-Za-z_]\w*)\s*\([^;{}]*\)\s*\{", re.M
)
REGISTRY = re.compile(
    r"^#define\s+((?:CHARA|DEMO)_\w+)\s+\{\s*(0x[0-9a-fA-F]+),"
    r"\s*([\w?]+)\s*\}.*$", re.M
)


def digest(data):
    return hashlib.sha256(data).hexdigest()


def build(catalog, source):
    errors = []
    effects = catalog["effects"]
    identifiers = [effect["id"] for effect in effects]
    if len(identifiers) != len(set(identifiers)):
        errors.append("Duplicate effect IDs")
    tree = {p.relative_to(source).as_posix(): p.read_bytes()
            for p in sorted(source.rglob("*")) if p.is_file()}
    if not tree:
        raise ValueError(f"No source files in {source}")
    tree_hash = digest(b"".join(
        name.encode() + b"\0" + digest(data).encode() + b"\n"
        for name, data in tree.items()))
    if tree_hash != catalog["upstream"]["source_tree_sha256"]:
        errors.append("Source tree differs from the catalog's pinned snapshot")
    texts = {name: data.decode("utf-8", errors="replace") for name, data in tree.items()}
    refs = {}
    functions = {}
    presentation = []
    for effect in effects:
        for field in ("id", "title", "domain", "priority", "status", "evidence",
                      "source_files", "native", "parameters", "vulkan", "constraints", "verification"):
            if not effect.get(field):
                errors.append(f"{effect.get('id')}: missing {field}")
        if effect["status"] not in catalog["status_legend"]:
            errors.append(f"{effect['id']}: unknown status")
        if effect["priority"] not in catalog["priority_legend"]:
            errors.append(f"{effect['id']}: unknown priority")
        contract = effect.get("presentation_contract", {})
        status = contract.get("status")
        if (contract.get("equivalence") != "overall_visual_result"
                or contract.get("native_logic_unchanged") is not True
                or status not in ("implemented", "partial", "missing")):
            errors.append(f"{effect['id']}: missing/invalid presentation contract")
        evidence = []
        for item in contract.get("runtime_evidence", []):
            path = ROOT / item["file"]
            if not path.is_file() or item["anchor"] not in path.read_text():
                errors.append(f"{effect['id']}: missing runtime evidence {item}")
            else:
                evidence.append(item | {"sha256": digest(path.read_bytes())})
        if status == "implemented" and not evidence:
            errors.append(f"{effect['id']}: implemented without runtime evidence")
        adapter_evidence=contract.get("adapter_evidence")
        if adapter_evidence and not (ROOT / adapter_evidence).is_file():
            errors.append(f"{effect['id']}: missing adapter validation report")
        if contract.get("complete_scene_coverage"):
            scene_evidence = contract.get("scene_evidence", [])
            if (status != "implemented" or not scene_evidence or
                    any(not (ROOT / name).is_file() for name in scene_evidence)):
                errors.append(f"{effect['id']}: complete without scene evidence")
        presentation.append({"id": effect["id"], "status": status,
                             "validation": contract.get("validation"),
                             "adapter_evidence": adapter_evidence,
                             "complete_scene_coverage": contract.get("complete_scene_coverage", False),
                             "runtime_evidence": evidence})
        for name in effect["source_files"]:
            refs.setdefault(name, []).append(effect["id"])
            if name not in tree:
                errors.append(f"{effect['id']}: missing source {name}")
        for anchor in effect.get("anchors", []):
            if anchor["file"] not in effect["source_files"]:
                errors.append(f"{effect['id']}: anchor outside source_files")
            if anchor["text"] not in texts.get(anchor["file"], ""):
                errors.append(f"{effect['id']}: missing anchor {anchor}")

    exclusions = {}
    for group in catalog["scan_exclusions"]:
        for name in group["files"]:
            if name in exclusions or name in refs:
                errors.append(f"Overlapping exclusion: {name}")
            if name not in tree:
                errors.append(f"Missing excluded file: {name}")
            exclusions[name] = group["reason"]

    candidates = {}
    for name, text in texts.items():
        if not name.endswith(".c"):
            continue
        hits = [{"line": i, "api": sorted(set(m.group(0) for m in GRAPHICS.finditer(line)))}
                for i, line in enumerate(text.splitlines(), 1) if GRAPHICS.search(line)]
        if hits:
            candidates[name] = hits
        for match in FUNCTION.finditer(text):
            functions.setdefault(match[1], []).append(
                {"file": name, "line": text.count("\n", 0, match.start(1)) + 1})

    registrations = []
    registry_text = texts["include/charalst.h"]
    for match in REGISTRY.finditer(registry_text):
        macro, class_id, constructor = match.groups()
        # These are table registrations, NOT proof that a script executes the
        # actor. Demo opcodes and chara class IDs occupy different namespaces.
        stages = []
        token = re.compile(r"\b" + re.escape(macro) + r"\b")
        for name, text in texts.items():
            if (name.startswith("stage/") and name.endswith(".c")
                    and not Path(name).stem.startswith("_")):
                # _stage.c is a registry/template, not a playable stage.
                clean = re.sub(r"/\*.*?\*/", lambda m: "\n" * m[0].count("\n"),
                               text, flags=re.S)
                clean = re.sub(r"//[^\n]*", "", clean)
                for i, line in enumerate(clean.splitlines(), 1):
                    if token.search(line):
                        stages.append({"stage": Path(name).stem, "file": name, "line": i})
        registrations.append({
            "macro": macro, "id": class_id, "namespace": macro.split("_", 1)[0],
            "constructor": constructor,
            "registry_line": registry_text.count("\n", 0, match.start()) + 1,
            "definitions": functions.get(constructor, []), "stages": stages,
            "pc_only_comment": "PC-ONLY" in match[0],
        })

    base = catalog["upstream"]["repository"] + "/blob/" + catalog["upstream"]["commit"] + "/source/"
    inventory = []
    for name in sorted(set(refs) | set(candidates) | set(exclusions)):
        if name not in texts:
            continue
        text = texts[name]
        entries = []
        for registration in registrations:
            if any(d["file"] == name for d in registration["definitions"]):
                entries.append(registration)
        anchors = []
        for effect in effects:
            for anchor in effect.get("anchors", []):
                if anchor["file"] == name and anchor["text"] in text:
                    line = text.count("\n", 0, text.index(anchor["text"])) + 1
                    anchors.append({"effect": effect["id"], "text": anchor["text"],
                                    "line": line, "url": base + name + "#L" + str(line)})
        inventory.append({
            "file": name, "sha256": digest(tree[name]), "url": base + name,
            "effects": sorted(refs.get(name, [])),
            "exclusion": exclusions.get(name),
            "asm_stubs": text.count("#pragma INCLUDE_ASM"),
            "graphics_signals": candidates.get(name, []),
            "anchors": anchors,
            "entry_points": [
                {"name": match[1], "line": text.count("\n", 0, match.start(1)) + 1}
                for match in FUNCTION.finditer(text)
                if match[1].startswith(("New", "AN_")) or "Act" in match[1]
            ],
            "registrations": entries,
        })
    unmatched = sorted(set(candidates) - set(refs) - set(exclusions))
    if unmatched:
        errors.append(f"Unclassified graphics candidates: {', '.join(unmatched)}")
    return {
        "schema_version": 1,
        "kind": "static_source_audit_not_visual_validation",
        "catalog_sha256": digest(json.dumps(catalog, sort_keys=True, ensure_ascii=False).encode()),
        "upstream_commit": catalog["upstream"]["commit"],
        "source_tree_sha256": tree_hash,
        "presentation_coverage": {
            "required": len(effects),
            "adapters_complete": all(p["status"] == "implemented" for p in presentation),
            "renderer_status": dict(sorted(Counter(p["status"] for p in presentation if p["status"]).items())),
            "without_complete_renderer": [p["id"] for p in presentation if p["status"] != "implemented"],
            "without_complete_scene_validation": [p["id"] for p in presentation if not p["complete_scene_coverage"]],
            "complete": all(p["status"] == "implemented" and p["complete_scene_coverage"] for p in presentation),
        },
        "presentation": presentation,
        "coverage": {
            "c_files_scanned": sum(name.endswith(".c") for name in tree),
            "effect_families": len(effects),
            "curated_source_files": len(refs),
            "graphics_candidate_files": len(candidates),
            "candidate_files_in_effects": len(set(candidates) & set(refs)),
            "candidate_files_excluded": len(set(candidates) & set(exclusions)),
            "unclassified_candidate_files": unmatched,
            "registered_constructors_unresolved": sum(r["constructor"] == "?" for r in registrations),
        },
        "limitations": catalog["limitations"],
        "source_inventory": inventory,
        "unresolved_registry": [r for r in registrations if r["constructor"] == "?"],
        "errors": errors,
    }


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--catalog", type=Path, default=ROOT / "docs/mgs-effects-catalog.json")
    parser.add_argument("--source", type=Path, default=ROOT / ".local/effects-research/mgs-upstream/source")
    parser.add_argument("--output", type=Path, default=ROOT / "docs/mgs-effects-source-index.json")
    parser.add_argument("--check", action="store_true", help="Validate without writing; compare the saved index")
    parser.add_argument("--require-complete", action="store_true",
                        help="Fail until every effect has a modern renderer AND scene evidence")
    parser.add_argument("--require-adapters", action="store_true",
                        help="Require every renderer adapter; does not certify all scenes")
    args = parser.parse_args()
    try:
        report = build(json.loads(args.catalog.read_text()), args.source)
        if report["errors"]:
            for error in report["errors"]:
                print(error, file=sys.stderr)
            return 1
        if args.check:
            if json.loads(args.output.read_text()) != report:
                print("Saved index differs; inspect changes and regenerate", file=sys.stderr)
                return 1
        else:
            args.output.write_text(json.dumps(report, ensure_ascii=False, indent=2) + "\n")
        print(json.dumps(report["coverage"], ensure_ascii=False, indent=2))
        presentation = report["presentation_coverage"]
        print(json.dumps({"presentation": presentation["renderer_status"],
                          "adapters_complete": presentation["adapters_complete"],
                          "complete_scene_validation": presentation["required"] - len(presentation["without_complete_scene_validation"]),
                          "complete": presentation["complete"]}, ensure_ascii=False, indent=2))
        if args.require_adapters and not presentation["adapters_complete"]:
            print("Renderer adapters are incomplete", file=sys.stderr)
            return 2
        if args.require_complete and not presentation["complete"]:
            print("Modern presentation coverage is incomplete; consult presentation_coverage in the index", file=sys.stderr)
            return 2
        return 0
    except (ValueError, OSError, KeyError) as exc:
        print(str(exc), file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
