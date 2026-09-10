#!/usr/bin/env python3
"""Validate an explicit homogeneous-depth candidate against measured bounds.

This tool never invents or rounds a multiplier. It consumes a complete
``visibility_lineage_trace.py`` report and a caller-authored exact-rational
assignment, then checks every directed component inequality with integer
arithmetic. Passing is only permission to run the independent post-lift raster
oracle; it is not gauge, family, or object-identity proof.
"""

from __future__ import annotations

import argparse
import json
import re
import sys
from fractions import Fraction
from pathlib import Path
from typing import Any


LINEAGE_SCHEMA = "ps1-visibility-lineage-audit/v1"
CANDIDATE_SCHEMA = "ps1-visibility-gauge-candidate/v1"
REPORT_SCHEMA = "ps1-visibility-gauge-candidate-audit/v1"


def _positive_fraction(payload: Any, context: str) -> Fraction:
    if not isinstance(payload, dict):
        raise ValueError(f"{context}: expected an object")
    numerator = payload.get("numerator")
    denominator = payload.get("denominator")
    if (
        isinstance(numerator, bool)
        or not isinstance(numerator, int)
        or isinstance(denominator, bool)
        or not isinstance(denominator, int)
        or numerator <= 0
        or denominator <= 0
    ):
        raise ValueError(
            f"{context}: numerator and denominator must be positive integers"
        )
    return Fraction(numerator, denominator)


def _fraction_report(value: Fraction) -> dict[str, Any]:
    return {
        "numerator": value.numerator,
        "denominator": value.denominator,
        "decimal_preview": format(float(value), ".12g"),
    }


def validate_visibility_gauge_candidate(
    lineage: Any, candidate: Any
) -> dict[str, Any]:
    errors: list[str] = []
    assignments: dict[str, Fraction] = {}
    constraints: list[dict[str, Any]] = []

    if not isinstance(lineage, dict):
        errors.append("lineage report must be an object")
    elif lineage.get("schema") != LINEAGE_SCHEMA:
        errors.append(
            f"lineage schema {lineage.get('schema')!r}, expected {LINEAGE_SCHEMA!r}"
        )
    elif not lineage.get("valid"):
        errors.append("lineage report is not valid")
    else:
        constraint_report = lineage.get("visibility_gauge_constraints")
        if not isinstance(constraint_report, dict):
            errors.append("lineage report has no visibility constraint system")
        else:
            if not constraint_report.get(
                "constraint_system_ready_for_candidate_testing"
            ):
                errors.append("visibility constraint system is not ready")
            if not constraint_report.get("constraint_graph_acyclic"):
                errors.append("visibility constraint graph is cyclic")
            constraints_value = constraint_report.get("constraints")
            if not isinstance(constraints_value, list):
                errors.append(
                    "lineage report does not contain the full constraint list"
                )
            else:
                constraints = constraints_value
                expected = constraint_report.get("directed_component_constraints")
                if (
                    isinstance(expected, bool)
                    or not isinstance(expected, int)
                    or expected != len(constraints)
                ):
                    errors.append("full constraint list count is inconsistent")

    if not isinstance(candidate, dict):
        errors.append("candidate must be an object")
    elif candidate.get("schema") != CANDIDATE_SCHEMA:
        errors.append(
            f"candidate schema {candidate.get('schema')!r}, expected "
            f"{CANDIDATE_SCHEMA!r}"
        )
    else:
        values = candidate.get("presentation_multipliers")
        if not isinstance(values, list):
            errors.append("candidate presentation_multipliers must be a list")
        else:
            for index, value in enumerate(values):
                context = f"candidate multiplier {index}"
                if not isinstance(value, dict):
                    errors.append(f"{context}: expected an object")
                    continue
                component = value.get("component_sha256")
                if not isinstance(component, str) or re.fullmatch(
                    r"[0-9a-f]{64}", component
                ) is None:
                    errors.append(f"{context}: invalid component_sha256")
                    continue
                if component in assignments:
                    errors.append(f"{context}: duplicate component_sha256")
                    continue
                try:
                    assignments[component] = _positive_fraction(value, context)
                except ValueError as exc:
                    errors.append(str(exc))

    required_components: set[str] = set()
    parsed_constraints = []
    for index, constraint in enumerate(constraints):
        context = f"constraint {index}"
        if not isinstance(constraint, dict):
            errors.append(f"{context}: expected an object")
            continue
        nearer = constraint.get("nearer_ot_component_sha256")
        farther = constraint.get("farther_depth_component_sha256")
        if not isinstance(nearer, str) or not isinstance(farther, str):
            errors.append(f"{context}: missing component labels")
            continue
        if nearer == farther:
            errors.append(f"{context}: self-component inequality")
            continue
        try:
            lower_bound = _positive_fraction(
                constraint.get("strict_lower_bound"),
                f"{context} strict_lower_bound",
            )
        except ValueError as exc:
            errors.append(str(exc))
            continue
        required_components.update((nearer, farther))
        parsed_constraints.append((nearer, farther, lower_bound))

    if not errors:
        missing = sorted(required_components - set(assignments))
        extra = sorted(set(assignments) - required_components)
        if missing:
            errors.append("candidate is missing components: " + ", ".join(missing))
        if extra:
            errors.append("candidate contains unknown components: " + ", ".join(extra))

    checks = []
    if not errors:
        for nearer, farther, lower_bound in parsed_constraints:
            ratio = assignments[farther] / assignments[nearer]
            satisfied = ratio > lower_bound
            checks.append(
                {
                    "nearer_ot_component_sha256": nearer,
                    "farther_depth_component_sha256": farther,
                    "candidate_farther_to_nearer_ratio": _fraction_report(ratio),
                    "strict_lower_bound": _fraction_report(lower_bound),
                    "strict_inequality_satisfied": satisfied,
                }
            )

    input_valid = not errors
    satisfies = input_valid and bool(checks) and all(
        check["strict_inequality_satisfied"] for check in checks
    )
    return {
        "schema": REPORT_SCHEMA,
        "input_valid": input_valid,
        "candidate_satisfies_all_measured_constraints": satisfies,
        "components": len(assignments),
        "constraints": len(checks),
        "normalized_presentation_multipliers": [
            {
                "component_sha256": component,
                **_fraction_report(multiplier),
            }
            for component, multiplier in sorted(assignments.items())
        ],
        "checks": checks,
        "proof_scope": {
            "candidate_selected_or_fitted_by_tool": False,
            "integer_or_tenth_prior_used": False,
            "family_count_prior_used": False,
            "passing_candidate_is_gauge_proof": False,
            "passing_candidate_is_family_identity_proof": False,
            "independent_post_lift_raster_validation_required": True,
        },
        "errors": errors,
    }


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("lineage_report", type=Path)
    parser.add_argument("candidate", type=Path)
    parser.add_argument("--output", type=Path)
    args = parser.parse_args()
    try:
        lineage = json.loads(args.lineage_report.read_text(encoding="utf-8"))
        candidate = json.loads(args.candidate.read_text(encoding="utf-8"))
        report = validate_visibility_gauge_candidate(lineage, candidate)
    except (OSError, json.JSONDecodeError) as exc:
        print(f"visibility_gauge_candidate: {exc}", file=sys.stderr)
        return 2
    payload = json.dumps(report, indent=2, sort_keys=True) + "\n"
    if args.output:
        args.output.write_text(payload, encoding="utf-8")
    else:
        print(payload, end="")
    if not report["input_valid"]:
        return 2
    return 0 if report["candidate_satisfies_all_measured_constraints"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
