#!/usr/bin/env python3
"""Audit intrinsic GTE -> GP0 -> DMA/OT provenance and exact depth gauges.

This tool never clusters pixels or motion. It joins records by projection_id,
checks immutable DMA traversal provenance, and reports only affine transforms
which are exactly proportional in integer fixed-point space. A reported ratio
is still a candidate until guest dataflow, temporal identity and the post-lift
owner pass satisfy VISIBILITY_AUDIT.md.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import math
import sys
from collections import defaultdict
from fractions import Fraction
from pathlib import Path
from typing import Any, Iterable


TRACE_SCHEMA = "ps1trace/v1"
REPORT_SCHEMA = "ps1-depth-family-trace/v1"
LIKELY_MAXIMUM_PRESENTATION_FAMILIES = 4
AFFINE_FIELDS = (
    "r00", "r01", "r02",
    "r10", "r11", "r12",
    "r20", "r21", "r22",
    "tx", "ty", "tz",
)
RELEVANT_EVENTS = frozenset(
    ("mips.value_expr", "gte.projection", "gpu.gp0_word", "gpu.dma_node")
)
VALUE_OP = {
    "boundary": 0,
    "constant": 1,
    "memory_load": 2,
    "add": 3,
    "subtract": 4,
    "shift_left": 5,
    "shift_right_logical": 6,
    "shift_right_arithmetic": 7,
    "bit_and": 8,
    "bit_or": 9,
    "bit_xor": 10,
    "multiply_signed_low": 12,
    "multiply_unsigned_low": 14,
    "load_word_left": 22,
    "load_word_right": 23,
    "gte_data_write": 24,
    "gte_control_write": 25,
    "gte_command_input": 26,
    "gte_command_result": 27,
}
BOUNDARY_KIND = {
    1: "interpreter-entry",
    2: "untracked-mutation",
    3: "external-call",
    4: "existing-gte-state",
}
PROJECTION_EXPRESSION_FIELDS = (
    "vxy_expr", "vz_expr",
    "rt0_expr", "rt1_expr", "rt2_expr", "rt3_expr", "rt4_expr",
    "tr0_expr", "tr1_expr", "tr2_expr",
)
TRANSFORM_COMPONENTS = (
    ("r00", "rt0", 0, 16),
    ("r01", "rt0", 16, 16),
    ("r02", "rt1", 0, 16),
    ("r10", "rt1", 16, 16),
    ("r11", "rt2", 0, 16),
    ("r12", "rt2", 16, 16),
    ("r20", "rt3", 0, 16),
    ("r21", "rt3", 16, 16),
    ("r22", "rt4", 0, 16),
    ("tx", "tr0", 0, 32),
    ("ty", "tr1", 0, 32),
    ("tz", "tr2", 0, 32),
)
ROTATION_COMPONENTS = frozenset(field for field in AFFINE_FIELDS[:9])
TRANSLATION_COMPONENTS = frozenset(field for field in AFFINE_FIELDS[9:])
FULL_WORD_MASK = 0xFFFFFFFF
FINGERPRINT_MASK = (1 << 64) - 1
MODEL_FINGERPRINT_SCHEMA_VERSION = 1
FINGERPRINT_OFFSET = 14695981039346656037
FINGERPRINT_PRIME = 1099511628211
SOURCE_EXPRESSION_DOMAIN = 0x4D564653
PROGRAM_EXPRESSION_DOMAIN = 0x4D564650
SOURCE_MODEL_DOMAIN = 0x4D564D53
PROGRAM_MODEL_DOMAIN = 0x4D564D50
MODEL_VALUE_FLOW_PROVENANCE = 1 << 7
SEMANTIC_IMMEDIATE_OPS = frozenset((
    VALUE_OP["shift_left"],
    VALUE_OP["shift_right_logical"],
    VALUE_OP["shift_right_arithmetic"],
    VALUE_OP["load_word_left"],
    VALUE_OP["load_word_right"],
    VALUE_OP["gte_data_write"],
    VALUE_OP["gte_control_write"],
    VALUE_OP["gte_command_input"],
    VALUE_OP["gte_command_result"],
))
SHIFT_OPS = frozenset((
    VALUE_OP["shift_left"],
    VALUE_OP["shift_right_logical"],
    VALUE_OP["shift_right_arithmetic"],
))


def read_trace(path: Path) -> Iterable[dict[str, Any]]:
    relevant_records = 0
    with path.open(encoding="utf-8") as stream:
        for line_number, line in enumerate(stream, 1):
            if not line.strip():
                continue
            try:
                record = json.loads(line)
            except json.JSONDecodeError as error:
                raise ValueError(
                    f"{path}:{line_number}: invalid JSON: {error}"
                ) from error
            if record.get("schema") != TRACE_SCHEMA:
                raise ValueError(
                    f"{path}:{line_number}: expected {TRACE_SCHEMA!r}, "
                    f"got {record.get('schema')!r}"
                )
            if not isinstance(record.get("event"), str):
                raise ValueError(f"{path}:{line_number}: missing string event")
            if record["event"] in RELEVANT_EVENTS:
                relevant_records += 1
                yield record
    if relevant_records == 0:
        raise ValueError(f"{path}: no depth-family provenance events")


def unsigned64(value: int) -> int:
    return int(value) & ((1 << 64) - 1)


def hex64(value: int) -> str:
    return f"0x{unsigned64(value):016x}"


def affine_vector(record: dict[str, Any]) -> tuple[int, ...]:
    missing = [field for field in AFFINE_FIELDS if field not in record]
    if missing:
        raise ValueError(
            f"gte.projection seq {record.get('seq')} lacks {', '.join(missing)}"
        )
    return tuple(int(record[field]) for field in AFFINE_FIELDS)


def canonical_affine(vector: tuple[int, ...]) -> tuple[tuple[int, ...], int]:
    divisor = 0
    for value in vector:
        divisor = math.gcd(divisor, abs(value))
    if divisor == 0:
        raise ValueError("zero affine transform has no projective gauge")
    # Do not normalize sign: a negative ratio is not an admissible camera
    # gauge and must never merge with the positive family.
    return tuple(value // divisor for value in vector), divisor


def canonical_id(vector: tuple[int, ...]) -> str:
    encoded = json.dumps(vector, separators=(",", ":")).encode("ascii")
    return hashlib.sha256(encoded).hexdigest()[:16]


def lineage_digest(value: Any) -> str:
    """Return a full collision-auditable digest for categorical lineage."""
    encoded = json.dumps(value, separators=(",", ":")).encode("ascii")
    return hashlib.sha256(encoded).hexdigest()


def physical_address(address: int) -> int:
    return int(address) & 0x1FFFFFFF


def append_fingerprint(hash_value: int, value: int, byte_count: int) -> int:
    for byte in range(byte_count):
        hash_value ^= (int(value) >> (byte * 8)) & 0xFF
        hash_value = (hash_value * FINGERPRINT_PRIME) & FINGERPRINT_MASK
    return hash_value


def runtime_expression_fingerprint(
    expression_id: int,
    expressions: dict[int, dict[str, Any]],
    include_memory_addresses: bool,
    memo: dict[int, int | None],
    active: set[int] | None = None,
) -> int | None:
    """Mirror the runtime's value-independent Merkle fingerprint exactly."""
    if expression_id == 0:
        return None
    if expression_id in memo:
        return memo[expression_id]
    if active is None:
        active = set()
    if expression_id in active:
        memo[expression_id] = None
        return None
    expression = expressions.get(expression_id)
    if expression is None or int(expression.get("complete", 0)) != 1:
        memo[expression_id] = None
        return None
    op = int(expression.get("op", -1))
    if op == VALUE_OP["boundary"]:
        memo[expression_id] = None
        return None

    active.add(expression_id)
    children = []
    for field in ("lhs", "rhs"):
        child_id = int(expression.get(field, 0))
        child = (
            runtime_expression_fingerprint(
                child_id, expressions, include_memory_addresses, memo, active
            )
            if child_id
            else None
        )
        if child_id and child is None:
            active.remove(expression_id)
            memo[expression_id] = None
            return None
        children.append((child_id, child))
    active.remove(expression_id)

    fingerprint = FINGERPRINT_OFFSET
    fingerprint = append_fingerprint(
        fingerprint,
        SOURCE_EXPRESSION_DOMAIN
        if include_memory_addresses
        else PROGRAM_EXPRESSION_DOMAIN,
        4,
    )
    fingerprint = append_fingerprint(
        fingerprint, MODEL_FINGERPRINT_SCHEMA_VERSION, 4
    )
    fingerprint = append_fingerprint(fingerprint, op, 1)
    fingerprint = append_fingerprint(
        fingerprint, int(expression.get("pc", 0)) & 0xFFFFFFFF, 4
    )
    fingerprint = append_fingerprint(
        fingerprint, int(expression.get("raw", 0)) & 0xFFFFFFFF, 4
    )

    if op == VALUE_OP["constant"]:
        fingerprint = append_fingerprint(fingerprint, 1, 1)
        fingerprint = append_fingerprint(
            fingerprint, int(expression.get("value", 0)) & 0xFFFFFFFF, 4
        )
    elif op == VALUE_OP["memory_load"]:
        address = int(expression.get("address", -1))
        if address < 0:
            memo[expression_id] = None
            return None
        fingerprint = append_fingerprint(fingerprint, 2, 1)
        fingerprint = append_fingerprint(
            fingerprint, int(include_memory_addresses), 1
        )
        if include_memory_addresses:
            fingerprint = append_fingerprint(
                fingerprint, physical_address(address), 4
            )
        fingerprint = append_fingerprint(
            fingerprint, int(expression.get("width", 0)), 1
        )
        fingerprint = append_fingerprint(
            fingerprint, int(expression.get("signed", 0)) != 0, 1
        )
    elif op in SEMANTIC_IMMEDIATE_OPS:
        fingerprint = append_fingerprint(fingerprint, 3, 1)
        retain_immediate = not (op in SHIFT_OPS and children[1][0] != 0)
        fingerprint = append_fingerprint(fingerprint, int(retain_immediate), 1)
        if retain_immediate:
            fingerprint = append_fingerprint(
                fingerprint,
                unsigned64(int(expression.get("immediate", 0))),
                8,
            )
    else:
        fingerprint = append_fingerprint(fingerprint, 0, 1)

    for child_id, child in children:
        fingerprint = append_fingerprint(fingerprint, int(child_id != 0), 1)
        if child_id:
            fingerprint = append_fingerprint(fingerprint, int(child), 8)
    memo[expression_id] = fingerprint
    return fingerprint


def runtime_model_fingerprints(
    projection: dict[str, Any], expressions: dict[int, dict[str, Any]]
) -> tuple[int, int] | None:
    roots = (
        int(projection.get("vxy_expr", 0)),
        int(projection.get("vz_expr", 0)),
    )
    if 0 in roots:
        return None
    result = []
    for include_addresses, domain in (
        (True, SOURCE_MODEL_DOMAIN),
        (False, PROGRAM_MODEL_DOMAIN),
    ):
        root_fingerprints = [
            runtime_expression_fingerprint(
                root, expressions, include_addresses, {}
            )
            for root in roots
        ]
        if any(value is None for value in root_fingerprints):
            return None
        fingerprint = append_fingerprint(FINGERPRINT_OFFSET, domain, 4)
        fingerprint = append_fingerprint(
            fingerprint, MODEL_FINGERPRINT_SCHEMA_VERSION, 4
        )
        for value in root_fingerprints:
            fingerprint = append_fingerprint(fingerprint, int(value), 8)
        result.append(fingerprint)
    return result[0], result[1]


def expression_lineage(
    expression_id: int,
    expressions: dict[int, dict[str, Any]],
    include_memory_addresses: bool,
    memo: dict[int, tuple[Any, ...] | None],
    active: set[int] | None = None,
) -> tuple[Any, ...] | None:
    """Canonicalize a complete expression DAG without sampled result values.

    The source-sensitive form retains physical memory addresses. The program
    form omits only those dynamic addresses and therefore describes an exact
    instruction/dataflow path, not an object identity. Constants remain part
    of both forms. Missing expressions, boundaries and incomplete nodes fail
    closed instead of receiving a synthetic lineage.
    """
    if expression_id == 0:
        return None
    if expression_id in memo:
        return memo[expression_id]
    if active is None:
        active = set()
    if expression_id in active:
        memo[expression_id] = None
        return None
    expression = expressions.get(expression_id)
    if expression is None or int(expression.get("complete", 0)) != 1:
        memo[expression_id] = None
        return None
    op = int(expression.get("op", -1))
    if op == VALUE_OP["boundary"]:
        memo[expression_id] = None
        return None

    active.add(expression_id)
    lhs_id = int(expression.get("lhs", 0))
    rhs_id = int(expression.get("rhs", 0))
    lhs = (
        expression_lineage(
            lhs_id, expressions, include_memory_addresses, memo, active
        )
        if lhs_id
        else ()
    )
    rhs = (
        expression_lineage(
            rhs_id, expressions, include_memory_addresses, memo, active
        )
        if rhs_id
        else ()
    )
    active.remove(expression_id)
    if (lhs_id and lhs is None) or (rhs_id and rhs is None):
        memo[expression_id] = None
        return None

    metadata: tuple[Any, ...] = ()
    if op == VALUE_OP["constant"]:
        metadata = ("constant", int(expression.get("value", 0)) & 0xFFFFFFFF)
    elif op == VALUE_OP["memory_load"]:
        address = int(expression.get("address", -1))
        metadata = (
            "memory",
            physical_address(address) if include_memory_addresses else None,
            int(expression.get("width", 0)),
            int(expression.get("signed", 0)),
        )
    elif op in (
        VALUE_OP["shift_left"],
        VALUE_OP["shift_right_logical"],
        VALUE_OP["shift_right_arithmetic"],
        VALUE_OP["load_word_left"],
        VALUE_OP["load_word_right"],
        VALUE_OP["gte_data_write"],
        VALUE_OP["gte_control_write"],
        VALUE_OP["gte_command_input"],
        VALUE_OP["gte_command_result"],
    ):
        # For variable shifts the RHS lineage carries the shift source and the
        # immediate field is only the sampled amount. Excluding that sample is
        # what keeps this a lineage rather than a value fingerprint.
        metadata = (
            "semantic-immediate",
            None
            if op in (
                VALUE_OP["shift_left"],
                VALUE_OP["shift_right_logical"],
                VALUE_OP["shift_right_arithmetic"],
            ) and rhs_id
            else int(expression.get("immediate", 0)),
        )

    node = (
        op,
        int(expression.get("pc", 0)) & 0xFFFFFFFF,
        int(expression.get("raw", 0)) & 0xFFFFFFFF,
        metadata,
        lhs,
        rhs,
    )
    memo[expression_id] = node
    return node


def model_value_flow_lineage(
    projection: dict[str, Any],
    expressions: dict[int, dict[str, Any]],
) -> dict[str, Any] | None:
    roots = (int(projection.get("vxy_expr", 0)),
             int(projection.get("vz_expr", 0)))
    if 0 in roots:
        return None
    source_nodes = tuple(
        expression_lineage(root, expressions, True, {}) for root in roots
    )
    program_nodes = tuple(
        expression_lineage(root, expressions, False, {}) for root in roots
    )
    if any(node is None for node in source_nodes + program_nodes):
        return None

    memory_sources = set()
    for root in roots:
        for expression_id in reachable_expressions(root, expressions):
            expression = expressions[expression_id]
            if int(expression.get("op", -1)) != VALUE_OP["memory_load"]:
                continue
            address = int(expression.get("address", -1))
            if address < 0:
                return None
            memory_sources.add(
                (
                    physical_address(address),
                    int(expression.get("width", 0)),
                    int(expression.get("signed", 0)),
                )
            )
    return {
        "source_payload": source_nodes,
        "program_payload": program_nodes,
        "source_digest": lineage_digest(source_nodes),
        "program_digest": lineage_digest(program_nodes),
        "memory_sources": sorted(memory_sources),
    }


def payload_contains(node: dict[str, Any], word_address: int) -> bool:
    if word_address < 0:
        return False
    address = int(node["node_address"]) & 0x1FFFFC
    payload_address = word_address & 0x1FFFFC
    words = int(node["words"])
    return any(
        ((address + index * 4) & 0x1FFFFC) == payload_address
        for index in range(1, words + 1)
    )


def distinct_sorted(values: Iterable[Any]) -> list[Any]:
    return sorted(set(values))


def signed32(value: int) -> int:
    value = int(value) & 0xFFFFFFFF
    return value - (1 << 32) if value & 0x80000000 else value


def constant_value(
    expression_id: int,
    expressions: dict[int, dict[str, Any]],
) -> int | None:
    expression = expressions.get(expression_id)
    if expression is None or int(expression.get("op", -1)) != VALUE_OP["constant"]:
        return None
    return int(expression.get("value", 0)) & 0xFFFFFFFF


def reachable_expressions(
    root: int,
    expressions: dict[int, dict[str, Any]],
) -> set[int]:
    reached: set[int] = set()
    pending = [root] if root else []
    while pending:
        expression_id = pending.pop()
        if expression_id in reached:
            continue
        expression = expressions.get(expression_id)
        if expression is None:
            continue
        reached.add(expression_id)
        for field in ("lhs", "rhs"):
            dependency = int(expression.get(field, 0))
            if dependency:
                pending.append(dependency)
    return reached


def fixed_point_scale_candidate(
    expression: dict[str, Any],
    expressions: dict[int, dict[str, Any]],
) -> dict[str, Any] | None:
    shift_op = int(expression.get("op", -1))
    if shift_op not in (
        VALUE_OP["shift_right_logical"],
        VALUE_OP["shift_right_arithmetic"],
    ):
        return None
    # Variable shifts record the sampled amount in immediate for diagnostics,
    # but their RHS expression proves it was not a fixed projective factor.
    if int(expression.get("rhs", 0)) != 0:
        return None
    shift = int(expression.get("immediate", -1))
    if shift <= 0 or shift >= 32:
        return None
    multiply_id = int(expression.get("lhs", 0))
    multiply = expressions.get(multiply_id)
    if multiply is None:
        return None
    multiply_op = int(multiply.get("op", -1))
    if multiply_op not in (
        VALUE_OP["multiply_signed_low"],
        VALUE_OP["multiply_unsigned_low"],
    ):
        return None

    lhs_id = int(multiply.get("lhs", 0))
    rhs_id = int(multiply.get("rhs", 0))
    lhs_constant = constant_value(lhs_id, expressions)
    rhs_constant = constant_value(rhs_id, expressions)
    if (lhs_constant is None) == (rhs_constant is None):
        return None
    multiplier = lhs_constant if lhs_constant is not None else rhs_constant
    source_id = rhs_id if lhs_constant is not None else lhs_id
    if multiplier is None or multiplier == 0:
        return None

    source = expressions.get(source_id)
    source_value = int(source.get("value", 0)) if source is not None else 0
    if multiply_op == VALUE_OP["multiply_signed_low"]:
        signed_multiplier = signed32(multiplier)
        if signed_multiplier <= 0:
            return None
        factor = Fraction(signed_multiplier, 1 << shift)
        full_product = signed32(source_value) * signed_multiplier
        multiplication_wrapped = not (-(1 << 31) <= full_product < (1 << 31))
    else:
        factor = Fraction(multiplier, 1 << shift)
        full_product = (source_value & 0xFFFFFFFF) * multiplier
        multiplication_wrapped = full_product > 0xFFFFFFFF
    if shift_op == VALUE_OP["shift_right_arithmetic"]:
        shifted_input = signed32(int(multiply.get("value", 0)))
        rounding = "arithmetic-floor"
    else:
        shifted_input = int(multiply.get("value", 0)) & 0xFFFFFFFF
        rounding = "logical-truncate"
    denominator = 1 << shift
    shifted_result = (
        signed32(int(expression.get("value", 0)))
        if shift_op == VALUE_OP["shift_right_arithmetic"]
        else int(expression.get("value", 0)) & 0xFFFFFFFF
    )
    discarded = shifted_input - shifted_result * denominator

    return {
        "shift_expression_id": int(expression["expression_id"]),
        "multiply_expression_id": multiply_id,
        "source_expression_id": source_id,
        "constant_expression_id": (
            lhs_id if lhs_constant is not None else rhs_id
        ),
        "factor_numerator": factor.numerator,
        "factor_denominator": factor.denominator,
        "shift": shift,
        "rounding": rounding,
        "discarded_remainder": discarded,
        "multiplication_wrapped": multiplication_wrapped,
        "expression_complete": int(expression.get("complete", 0)) == 1,
    }


def bit_range_mask(offset: int, width: int) -> int:
    if offset < 0 or width <= 0 or offset + width > 32:
        return 0
    if width == 32:
        return FULL_WORD_MASK
    return ((1 << width) - 1) << offset


def expression_support_mask(
    expression_id: int,
    expressions: dict[int, dict[str, Any]],
    memo: dict[int, int],
    active: set[int] | None = None,
) -> int:
    """Return bits which may structurally depend on a non-constant leaf.

    This is deliberately conservative. It is used only to prove that packed
    OR/XOR branches occupy disjoint bit lanes; an unknown or arithmetic value
    owns all 32 bits and therefore prevents an unsafe attribution.
    """
    if expression_id == 0:
        return 0
    if expression_id in memo:
        return memo[expression_id]
    if active is None:
        active = set()
    if expression_id in active:
        return FULL_WORD_MASK
    expression = expressions.get(expression_id)
    if expression is None:
        return FULL_WORD_MASK
    active.add(expression_id)
    op = int(expression.get("op", -1))
    lhs_id = int(expression.get("lhs", 0))
    rhs_id = int(expression.get("rhs", 0))

    if op == VALUE_OP["constant"]:
        support = 0
    elif op == VALUE_OP["memory_load"]:
        width = int(expression.get("width", 0))
        if int(expression.get("signed", 0)) == 1 and 0 < width < 4:
            support = FULL_WORD_MASK
        elif 0 < width <= 4:
            support = (1 << (width * 8)) - 1 if width < 4 else FULL_WORD_MASK
        else:
            support = FULL_WORD_MASK
    elif op == VALUE_OP["boundary"]:
        support = FULL_WORD_MASK
    elif op in (VALUE_OP["gte_data_write"], VALUE_OP["gte_control_write"]):
        support = expression_support_mask(lhs_id, expressions, memo, active)
    elif op in (VALUE_OP["bit_and"], VALUE_OP["bit_or"],
                VALUE_OP["bit_xor"]):
        lhs_support = expression_support_mask(lhs_id, expressions, memo, active)
        rhs_support = expression_support_mask(rhs_id, expressions, memo, active)
        lhs_constant = constant_value(lhs_id, expressions)
        rhs_constant = constant_value(rhs_id, expressions)
        if op == VALUE_OP["bit_and"] and lhs_constant is not None:
            support = rhs_support & lhs_constant
        elif op == VALUE_OP["bit_and"] and rhs_constant is not None:
            support = lhs_support & rhs_constant
        else:
            support = lhs_support | rhs_support
    elif op in (VALUE_OP["shift_left"],
                VALUE_OP["shift_right_logical"],
                VALUE_OP["shift_right_arithmetic"]) and rhs_id == 0:
        source_support = expression_support_mask(
            lhs_id, expressions, memo, active
        )
        shift = int(expression.get("immediate", -1))
        if not 0 <= shift < 32:
            support = FULL_WORD_MASK
        elif op == VALUE_OP["shift_left"]:
            support = (source_support << shift) & FULL_WORD_MASK
        elif op == VALUE_OP["shift_right_logical"]:
            support = source_support >> shift
        else:
            support = source_support >> shift
            if shift and source_support & 0x80000000:
                support |= (FULL_WORD_MASK << (32 - shift)) & FULL_WORD_MASK
    elif op in (VALUE_OP["load_word_left"], VALUE_OP["load_word_right"]):
        # The exact byte merge is retained in the DAG, but it is not needed to
        # prove the downstream packed control-register lanes. Keep it
        # conservative here rather than deriving facts from the sampled value.
        support = FULL_WORD_MASK
    elif lhs_id or rhs_id:
        support = FULL_WORD_MASK
    else:
        support = 0

    active.remove(expression_id)
    memo[expression_id] = support & FULL_WORD_MASK
    return memo[expression_id]


def expression_reaches_candidate(
    expression_id: int,
    expressions: dict[int, dict[str, Any]],
    candidates: dict[int, dict[str, Any]],
    memo: dict[int, bool],
    active: set[int] | None = None,
) -> bool:
    if expression_id in candidates:
        return True
    if expression_id == 0:
        return False
    if expression_id in memo:
        return memo[expression_id]
    if active is None:
        active = set()
    if expression_id in active:
        return True
    expression = expressions.get(expression_id)
    if expression is None:
        return False
    active.add(expression_id)
    reached = any(
        expression_reaches_candidate(
            int(expression.get(field, 0)), expressions, candidates, memo,
            active,
        )
        for field in ("lhs", "rhs")
    )
    active.remove(expression_id)
    memo[expression_id] = reached
    return reached


def route_component_scale(
    expression_id: int,
    offset: int,
    width: int,
    expressions: dict[int, dict[str, Any]],
    candidates: dict[int, dict[str, Any]],
    support_memo: dict[int, int],
    candidate_reach_memo: dict[int, bool],
    route_memo: dict[tuple[int, int, int], tuple[dict[str, Any] | None, str]],
    active: set[tuple[int, int, int]] | None = None,
) -> tuple[dict[str, Any] | None, str]:
    """Route one affine component through provably transparent packing ops.

    A route succeeds only when the complete component can be attributed to a
    single fixed-point operation. Concrete zero bits are never used to choose
    a branch: only constants, masks and fixed shifts may establish ownership.
    """
    key = (expression_id, offset, width)
    if key in route_memo:
        route, reason = route_memo[key]
        return (dict(route) if route is not None else None), reason
    if active is None:
        active = set()
    if key in active:
        return None, "expression-cycle"
    active.add(key)

    def finish(
        route: dict[str, Any] | None, reason: str
    ) -> tuple[dict[str, Any] | None, str]:
        active.remove(key)
        route_memo[key] = (dict(route) if route is not None else None, reason)
        return route, reason

    expression = expressions.get(expression_id)
    requested_mask = bit_range_mask(offset, width)
    if expression is None or requested_mask == 0:
        return finish(None, "missing-expression")
    if expression_id in candidates:
        if offset != 0 or width not in (16, 32):
            return finish(None, "candidate-slice-is-not-a-scalar")
        return finish(
            {
                "candidate_expression_id": expression_id,
                "post_scale_sign": 1,
                "route_expression_ids": [expression_id],
            },
            "exact",
        )
    if not expression_reaches_candidate(
        expression_id, expressions, candidates, candidate_reach_memo
    ):
        return finish(None, "no-candidate-in-component")

    op = int(expression.get("op", -1))
    lhs_id = int(expression.get("lhs", 0))
    rhs_id = int(expression.get("rhs", 0))

    def descend(
        child: int, child_offset: int = offset, child_width: int = width
    ) -> tuple[dict[str, Any] | None, str]:
        route, reason = route_component_scale(
            child, child_offset, child_width, expressions, candidates,
            support_memo, candidate_reach_memo, route_memo, active,
        )
        if route is not None:
            route["route_expression_ids"] = [
                expression_id, *route["route_expression_ids"]
            ]
        return route, reason

    if op in (VALUE_OP["gte_data_write"], VALUE_OP["gte_control_write"]):
        route, reason = descend(lhs_id)
        return finish(route, reason)

    lhs_constant = constant_value(lhs_id, expressions)
    rhs_constant = constant_value(rhs_id, expressions)
    if op == VALUE_OP["bit_and"]:
        if lhs_constant is not None and rhs_constant is None:
            mask, child = lhs_constant, rhs_id
        elif rhs_constant is not None and lhs_constant is None:
            mask, child = rhs_constant, lhs_id
        else:
            return finish(None, "non-constant-bit-mask")
        if requested_mask & ~mask:
            return finish(None, "component-partly-masked-to-constant")
        route, reason = descend(child)
        return finish(route, reason)

    if op in (VALUE_OP["bit_or"], VALUE_OP["bit_xor"]):
        if lhs_constant is not None or rhs_constant is not None:
            constant = lhs_constant if lhs_constant is not None else rhs_constant
            child = rhs_id if lhs_constant is not None else lhs_id
            if constant & requested_mask:
                return finish(None, "non-zero-constant-in-component")
            route, reason = descend(child)
            return finish(route, reason)
        lhs_support = expression_support_mask(
            lhs_id, expressions, support_memo
        ) & requested_mask
        rhs_support = expression_support_mask(
            rhs_id, expressions, support_memo
        ) & requested_mask
        if lhs_support and rhs_support:
            return finish(None, "overlapping-bit-sources")
        if lhs_support == requested_mask:
            route, reason = descend(lhs_id)
            return finish(route, reason)
        if rhs_support == requested_mask:
            route, reason = descend(rhs_id)
            return finish(route, reason)
        return finish(None, "component-source-is-partial-or-constant")

    if op in (VALUE_OP["shift_left"],
              VALUE_OP["shift_right_logical"],
              VALUE_OP["shift_right_arithmetic"]):
        if rhs_id != 0:
            return finish(None, "variable-shift")
        shift = int(expression.get("immediate", -1))
        if not 0 <= shift < 32:
            return finish(None, "invalid-shift")
        if op == VALUE_OP["shift_left"]:
            if offset < shift:
                return finish(None, "component-contains-shifted-zero-bits")
            route, reason = descend(lhs_id, offset - shift, width)
            return finish(route, reason)
        if offset + width > 32 - shift:
            reason = (
                "component-contains-sign-fill"
                if op == VALUE_OP["shift_right_arithmetic"]
                else "component-contains-shifted-zero-bits"
            )
            return finish(None, reason)
        route, reason = descend(lhs_id, offset + shift, width)
        return finish(route, reason)

    if op == VALUE_OP["subtract"]:
        if lhs_constant == 0 and offset == 0 and width in (16, 32):
            route, reason = descend(rhs_id)
            if route is not None:
                route["post_scale_sign"] *= -1
            return finish(route, reason)
        if rhs_constant == 0:
            route, reason = descend(lhs_id)
            return finish(route, reason)
        return finish(None, "non-transparent-subtract")

    if op == VALUE_OP["add"]:
        if lhs_constant == 0:
            route, reason = descend(rhs_id)
            return finish(route, reason)
        if rhs_constant == 0:
            route, reason = descend(lhs_id)
            return finish(route, reason)
        return finish(None, "non-transparent-add")

    return finish(None, "no-transparent-route")


def analyze(records: Iterable[dict[str, Any]]) -> dict[str, Any]:
    projections: dict[int, dict[str, Any]] = {}
    projection_collisions: list[dict[str, Any]] = []
    expressions: dict[int, dict[str, Any]] = {}
    expression_collisions: list[int] = []
    submitted: list[dict[str, Any]] = []
    current_node: dict[int, dict[str, Any]] = {}
    dma_nodes = 0
    explicit_dma_projection_words = 0
    dma_word_provenance_mismatches: list[dict[str, Any]] = []
    trace_records = 0

    for record in records:
        trace_records += 1
        event = record["event"]
        if event == "mips.value_expr":
            expression_id = int(record.get("expression_id", 0))
            if expression_id == 0 or expression_id in expressions:
                expression_collisions.append(expression_id)
            else:
                expressions[expression_id] = record
        elif event == "gte.projection":
            projection_id = int(record.get("projection_id", 0))
            if projection_id == 0:
                projection_collisions.append(
                    {"projection_id": 0, "reason": "reserved-zero"}
                )
                continue
            previous = projections.get(projection_id)
            if previous is not None:
                projection_collisions.append(
                    {
                        "projection_id": projection_id,
                        "reason": (
                            "duplicate-same-affine"
                            if affine_vector(previous) == affine_vector(record)
                            else "duplicate-different-affine"
                        ),
                    }
                )
            else:
                projections[projection_id] = record
        elif event == "gpu.dma_node":
            dma_nodes += 1
            current_node[int(record.get("submission_id", 0))] = record
        elif event == "gpu.gp0_word" and int(record.get("projection_id", 0)) != 0:
            submission_id = int(record.get("submission_id", 0))
            node = current_node.get(submission_id)
            word_address = int(record.get("word_address", -1))
            if "dma_provenance" in record:
                if int(record.get("dma_provenance", 0)) == 1:
                    explicit_dma_projection_words += 1
                    reasons = []
                    if node is None:
                        reasons.append("missing-current-node")
                    else:
                        if int(record.get("dma_list_start", -1)) != int(
                            node.get("list_start", -1)
                        ):
                            reasons.append("list-start-mismatch")
                        if int(record.get("dma_node_address", -1)) != int(
                            node.get("node_address", -1)
                        ):
                            reasons.append("node-address-mismatch")
                        if int(record.get("dma_traversal_ordinal", -1)) != int(
                            node.get("traversal_ordinal", -1)
                        ):
                            reasons.append("traversal-ordinal-mismatch")
                        if not payload_contains(node, word_address):
                            reasons.append("word-outside-node-payload")
                    if reasons:
                        dma_word_provenance_mismatches.append(
                            {
                                "seq": int(record.get("seq", 0)),
                                "submission_id": submission_id,
                                "word_address": word_address,
                                "reasons": reasons,
                            }
                        )
                        node = None
                else:
                    # A schema-new direct/HLE word explicitly denies DMA
                    # provenance; never inherit the preceding node by address.
                    node = None
            submitted.append(
                {
                    "word": record,
                    "node": (
                        node
                        if node is not None and payload_contains(node, word_address)
                        else None
                    ),
                }
            )

    joined: list[dict[str, Any]] = []
    missing_projection_ids: list[int] = []
    out_of_window_projection_words = 0
    submitted_ids: set[int] = set()
    captured_first = min(projections, default=0)
    captured_last = max(projections, default=0)
    for use in submitted:
        projection_id = int(use["word"]["projection_id"])
        submitted_ids.add(projection_id)
        projection = projections.get(projection_id)
        if projection is None:
            if captured_first <= projection_id <= captured_last:
                missing_projection_ids.append(projection_id)
            else:
                out_of_window_projection_words += 1
            continue
        joined.append({**use, "projection": projection})
    submitted_uses_by_projection: dict[int, int] = defaultdict(int)
    for use in joined:
        submitted_uses_by_projection[
            int(use["projection"]["projection_id"])
        ] += 1
    complete_value_flow_joined_words = sum(
        int(use["projection"].get("value_flow_complete", 0)) == 1
        for use in joined
    )
    gpu_model_fingerprint_joined_words = 0
    gpu_model_fingerprint_mismatches: list[dict[str, Any]] = []
    gpu_fingerprint_schema_present = any(
        any(
            field in use["word"]
            for field in (
                "model_source_fingerprint",
                "model_program_fingerprint",
            )
        )
        for use in submitted
    )
    for use in joined:
        word = use["word"]
        word_fingerprint_fields_present = all(
            field in word
            for field in (
                "model_source_fingerprint",
                "model_program_fingerprint",
            )
        )
        if (
            not word_fingerprint_fields_present
            and not gpu_fingerprint_schema_present
        ):
            # Pre-schema-7 traces remain analyzable but cannot certify this
            # transport edge.
            continue
        projection = use["projection"]
        projection_id = int(projection["projection_id"])
        gpu_evidence = (
            int(word.get("intrinsic_provenance", 0))
            & MODEL_VALUE_FLOW_PROVENANCE
        ) != 0
        projection_evidence = int(
            projection.get("model_fingerprint_complete", 0)
        ) == 1
        gpu_source = unsigned64(
            int(word.get("model_source_fingerprint", 0))
        )
        gpu_program = unsigned64(
            int(word.get("model_program_fingerprint", 0))
        )
        reasons = []
        if not word_fingerprint_fields_present:
            reasons.append("missing-gpu-fingerprint-fields")
        if not all(
            field in projection
            for field in (
                "model_fingerprint_complete",
                "model_fingerprint_schema",
                "model_source_fingerprint",
                "model_program_fingerprint",
            )
        ):
            reasons.append("missing-projection-fingerprint-schema")
        elif gpu_evidence != projection_evidence:
            reasons.append("model-flow-evidence-mismatch")
        if gpu_evidence and word_fingerprint_fields_present:
            if gpu_source != unsigned64(
                int(projection.get("model_source_fingerprint", 0))
            ):
                reasons.append("source-fingerprint-transport-mismatch")
            if gpu_program != unsigned64(
                int(projection.get("model_program_fingerprint", 0))
            ):
                reasons.append("program-fingerprint-transport-mismatch")
        elif not gpu_evidence and (gpu_source != 0 or gpu_program != 0):
            reasons.append("fingerprint-values-without-gpu-evidence")
        if reasons:
            gpu_model_fingerprint_mismatches.append({
                "seq": int(word.get("seq", 0)),
                "projection_id": projection_id,
                "reasons": reasons,
            })
        elif gpu_evidence:
            gpu_model_fingerprint_joined_words += 1

    model_rooted_projections = 0
    model_flow_by_projection: dict[int, dict[str, Any]] = {}
    source_payloads: dict[str, set[str]] = defaultdict(set)
    program_payloads: dict[str, set[str]] = defaultdict(set)
    source_lineages: dict[str, dict[str, Any]] = {}
    program_lineages: dict[str, dict[str, Any]] = {}
    runtime_model_fingerprint_projections = 0
    runtime_fingerprint_mismatches: list[dict[str, Any]] = []
    runtime_source_payloads: dict[int, set[str]] = defaultdict(set)
    runtime_program_payloads: dict[int, set[str]] = defaultdict(set)
    runtime_fingerprint_fields = (
        "model_fingerprint_complete",
        "model_fingerprint_schema",
        "model_source_fingerprint",
        "model_program_fingerprint",
    )
    runtime_fingerprint_schema_present = any(
        any(field in projection for field in runtime_fingerprint_fields)
        for projection in projections.values()
    )
    for projection in projections.values():
        projection_id = int(projection["projection_id"])
        if int(projection.get("vxy_expr", 0)) and int(
            projection.get("vz_expr", 0)
        ):
            model_rooted_projections += 1
        flow = model_value_flow_lineage(projection, expressions)
        runtime_fields_complete = all(
            field in projection
            for field in runtime_fingerprint_fields
        )
        runtime_complete = int(
            projection.get("model_fingerprint_complete", 0)
        ) == 1
        runtime_schema = int(projection.get("model_fingerprint_schema", 0))
        runtime_source = unsigned64(
            int(projection.get("model_source_fingerprint", 0))
        )
        runtime_program = unsigned64(
            int(projection.get("model_program_fingerprint", 0))
        )
        expected_runtime = runtime_model_fingerprints(projection, expressions)
        runtime_reasons = []
        if runtime_fingerprint_schema_present:
            if not runtime_fields_complete:
                runtime_reasons.append("missing-runtime-fingerprint-fields")
            if (
                "model_fingerprint_schema" in projection
                and runtime_schema != MODEL_FINGERPRINT_SCHEMA_VERSION
            ):
                runtime_reasons.append("fingerprint-schema-mismatch")
            if runtime_complete:
                if expected_runtime is None:
                    runtime_reasons.append("runtime-evidence-without-complete-dag")
                elif runtime_fields_complete:
                    runtime_model_fingerprint_projections += 1
                    if runtime_source != expected_runtime[0]:
                        runtime_reasons.append("source-fingerprint-mismatch")
                    if runtime_program != expected_runtime[1]:
                        runtime_reasons.append("program-fingerprint-mismatch")
            else:
                if runtime_source != 0 or runtime_program != 0:
                    runtime_reasons.append("fingerprint-values-without-evidence")
                if expected_runtime is not None:
                    runtime_reasons.append("complete-dag-without-runtime-evidence")
        if runtime_reasons:
            runtime_fingerprint_mismatches.append({
                "projection_id": projection_id,
                "reasons": runtime_reasons,
            })
        if flow is None:
            continue
        model_flow_by_projection[projection_id] = flow
        source_digest = flow["source_digest"]
        program_digest = flow["program_digest"]
        if (runtime_fields_complete and runtime_complete and
                expected_runtime is not None and not runtime_reasons):
            runtime_source_payloads[runtime_source].add(source_digest)
            runtime_program_payloads[runtime_program].add(program_digest)
        source_payloads[source_digest].add(
            json.dumps(flow["source_payload"], separators=(",", ":"))
        )
        program_payloads[program_digest].add(
            json.dumps(flow["program_payload"], separators=(",", ":"))
        )
        direct_source = int(projection.get("model_source", -1))
        source = source_lineages.setdefault(
            source_digest,
            {
                "source_digest": source_digest,
                "program_digests": set(),
                "frames": set(),
                "projection_ids": set(),
                "memory_sources": set(),
                "direct_model_sources": set(),
                "transform_lineages": set(),
                "transform_hashes": set(),
                "submitted_uses": 0,
            },
        )
        source["program_digests"].add(program_digest)
        source["frames"].add(int(projection.get("frame", 0)))
        source["projection_ids"].add(projection_id)
        source["memory_sources"].update(flow["memory_sources"])
        source["direct_model_sources"].add(direct_source)
        source["transform_lineages"].add(
            unsigned64(int(projection.get("transform_lineage", 0)))
        )
        source["transform_hashes"].add(
            unsigned64(int(projection.get("transform_hash", 0)))
        )
        source["submitted_uses"] += submitted_uses_by_projection.get(
            projection_id, 0
        )

        program = program_lineages.setdefault(
            program_digest,
            {
                "program_digest": program_digest,
                "source_digests": set(),
                "frames": set(),
                "projection_ids": set(),
                "memory_sources": set(),
                "submitted_uses": 0,
            },
        )
        program["source_digests"].add(source_digest)
        program["frames"].add(int(projection.get("frame", 0)))
        program["projection_ids"].add(projection_id)
        program["memory_sources"].update(flow["memory_sources"])
        program["submitted_uses"] += submitted_uses_by_projection.get(
            projection_id, 0
        )

    def format_memory_sources(values: Iterable[tuple[int, int, int]]):
        return [
            {
                "physical_address": address,
                "width": width,
                "signed": bool(signed_value),
            }
            for address, width, signed_value in sorted(values)
        ]

    model_lineage_report = []
    for source in source_lineages.values():
        projection_ids = sorted(source.pop("projection_ids"))
        source["program_digests"] = sorted(source["program_digests"])
        source["frames"] = sorted(source["frames"])
        source["memory_sources"] = format_memory_sources(
            source["memory_sources"]
        )
        source["direct_model_sources"] = sorted(
            source["direct_model_sources"]
        )
        source["transform_lineages"] = sorted(
            hex64(value) for value in source["transform_lineages"]
        )
        source["transform_hashes"] = sorted(
            hex64(value) for value in source["transform_hashes"]
        )
        source["projection_count"] = len(projection_ids)
        source["projection_first"] = projection_ids[0]
        source["projection_last"] = projection_ids[-1]
        source["source_digest_is_object_identity"] = False
        model_lineage_report.append(source)
    model_lineage_report.sort(key=lambda item: item["source_digest"])

    model_program_report = []
    for program in program_lineages.values():
        projection_ids = sorted(program.pop("projection_ids"))
        program["source_digests"] = sorted(program["source_digests"])
        program["frames"] = sorted(program["frames"])
        program["memory_sources"] = format_memory_sources(
            program["memory_sources"]
        )
        program["projection_count"] = len(projection_ids)
        program["projection_first"] = projection_ids[0]
        program["projection_last"] = projection_ids[-1]
        program["program_digest_is_object_identity"] = False
        model_program_report.append(program)
    model_program_report.sort(key=lambda item: item["program_digest"])

    model_digest_collisions = [
        {"kind": "source", "digest": digest, "payload_variants": len(payloads)}
        for digest, payloads in sorted(source_payloads.items())
        if len(payloads) > 1
    ] + [
        {"kind": "program", "digest": digest, "payload_variants": len(payloads)}
        for digest, payloads in sorted(program_payloads.items())
        if len(payloads) > 1
    ]
    runtime_fingerprint_collisions = [
        {
            "kind": "source",
            "fingerprint": hex64(fingerprint),
            "sha256_payloads": sorted(payloads),
        }
        for fingerprint, payloads in sorted(runtime_source_payloads.items())
        if len(payloads) > 1
    ] + [
        {
            "kind": "program",
            "fingerprint": hex64(fingerprint),
            "sha256_payloads": sorted(payloads),
        }
        for fingerprint, payloads in sorted(runtime_program_payloads.items())
        if len(payloads) > 1
    ]

    snapshots: dict[tuple[int, ...], dict[str, Any]] = {}
    hash_vectors: dict[int, set[tuple[int, ...]]] = defaultdict(set)
    for projection in projections.values():
        vector = affine_vector(projection)
        transform_hash = unsigned64(int(projection.get("transform_hash", 0)))
        hash_vectors[transform_hash].add(vector)
        snapshot = snapshots.setdefault(
            vector,
            {
                "vector": vector,
                "transform_hashes": set(),
                "projection_ids": set(),
                "submitted_uses": 0,
            },
        )
        snapshot["transform_hashes"].add(transform_hash)
        snapshot["projection_ids"].add(int(projection["projection_id"]))
    for use in joined:
        snapshots[affine_vector(use["projection"])]["submitted_uses"] += 1

    normalized: dict[
        tuple[int, ...], list[tuple[tuple[int, ...], int]]
    ] = defaultdict(list)
    zero_transforms = 0
    for vector in snapshots:
        try:
            canonical, divisor = canonical_affine(vector)
        except ValueError:
            zero_transforms += 1
            continue
        normalized[canonical].append((vector, divisor))

    gauge_groups: list[dict[str, Any]] = []
    all_reported_gauges_in_tenths = True
    largest_candidate_group = 0
    for canonical, members in normalized.items():
        if len(members) < 2:
            continue
        members.sort(key=lambda item: (item[1], item[0]))
        anchor = members[0][1]
        gauges = []
        all_tenths = True
        for vector, divisor in members:
            ratio = Fraction(divisor, anchor)
            gauge_tenths = (
                ratio.numerator * 10 // ratio.denominator
                if (ratio.numerator * 10) % ratio.denominator == 0
                else None
            )
            all_tenths &= gauge_tenths is not None
            gauges.append(
                {
                    "transform_hashes": sorted(
                        hex64(value)
                        for value in snapshots[vector]["transform_hashes"]
                    ),
                    "projection_count": len(snapshots[vector]["projection_ids"]),
                    "submitted_uses": snapshots[vector]["submitted_uses"],
                    "relative_numerator": ratio.numerator,
                    "relative_denominator": ratio.denominator,
                    "relative_gauge_tenths": gauge_tenths,
                }
            )
        gauge_groups.append(
            {
                "canonical_affine_id": canonical_id(canonical),
                "all_relative_gauges_in_tenths": all_tenths,
                "members": gauges,
            }
        )
        all_reported_gauges_in_tenths &= all_tenths
        largest_candidate_group = max(largest_candidate_group, len(gauges))
    gauge_groups.sort(key=lambda group: group["canonical_affine_id"])

    lineages: dict[int, dict[str, Any]] = {}
    source_complete = 0
    dma_joined = 0
    for use in joined:
        projection = use["projection"]
        lineage = unsigned64(int(projection.get("transform_lineage", 0)))
        summary = lineages.setdefault(
            lineage,
            {
                "frames": set(),
                "transforms": set(),
                "models": set(),
                "model_value_flow_lineages": set(),
                "model_value_flow_programs": set(),
                "nodes": set(),
                "projection_ids": set(),
                "submitted_uses": 0,
            },
        )
        summary["submitted_uses"] += 1
        summary["projection_ids"].add(int(projection["projection_id"]))
        summary["frames"].add(int(projection.get("frame", 0)))
        summary["transforms"].add(
            unsigned64(int(projection.get("transform_hash", 0)))
        )
        model_source = int(projection.get("model_source", -1))
        transform_source = int(projection.get("transform_source", -1))
        summary["models"].add(model_source)
        model_flow = model_flow_by_projection.get(
            int(projection["projection_id"])
        )
        if model_flow is not None:
            summary["model_value_flow_lineages"].add(
                model_flow["source_digest"]
            )
            summary["model_value_flow_programs"].add(
                model_flow["program_digest"]
            )
        if use["node"] is not None:
            summary["nodes"].add(int(use["node"]["node_address"]))
            dma_joined += 1
        if (
            int(projection.get("lineage_complete", 0)) == 1
            and model_source >= 0
            and transform_source >= 0
            and use["node"] is not None
        ):
            source_complete += 1

    lineage_report = []
    for lineage, values in sorted(lineages.items()):
        lineage_report.append(
            {
                "transform_lineage": hex64(lineage),
                "frames": distinct_sorted(values["frames"]),
                "transform_hashes": sorted(
                    hex64(value) for value in values["transforms"]
                ),
                "model_sources": distinct_sorted(values["models"]),
                "model_value_flow_lineages": sorted(
                    values["model_value_flow_lineages"]
                ),
                "model_value_flow_programs": sorted(
                    values["model_value_flow_programs"]
                ),
                "dma_nodes": distinct_sorted(values["nodes"]),
                "dma_node_count": len(values["nodes"]),
                "projection_count": len(values["projection_ids"]),
                "submitted_uses": values["submitted_uses"],
            }
        )

    hash_collisions = [
        {"transform_hash": hex64(key), "affine_variants": len(vectors)}
        for key, vectors in sorted(hash_vectors.items())
        if len(vectors) > 1
    ]
    missing_expression_dependencies: set[int] = set()
    for expression in expressions.values():
        for field in ("lhs", "rhs"):
            dependency = int(expression.get(field, 0))
            if dependency and dependency not in expressions:
                missing_expression_dependencies.add(dependency)

    missing_projection_expression_roots: set[int] = set()
    rooted_projections = 0
    complete_value_flow_projections = 0
    candidate_uses: dict[int, dict[str, Any]] = {}
    factor_coverage_groups: dict[tuple[Any, ...], dict[str, Any]] = {}
    component_factor_groups: dict[tuple[Any, ...], dict[str, Any]] = {}
    component_route_failures: dict[tuple[str, str, str], dict[str, Any]] = {}
    exact_component_scale_routes = 0
    boundary_uses: dict[tuple[Any, ...], dict[str, Any]] = {}
    incomplete_roots_without_boundary: dict[str, int] = defaultdict(int)
    local_candidates = {
        int(expression["expression_id"]): candidate
        for expression in expressions.values()
        if (candidate := fixed_point_scale_candidate(expression, expressions))
        is not None
    }
    support_memo: dict[int, int] = {}
    candidate_reach_memo: dict[int, bool] = {}
    route_memo: dict[
        tuple[int, int, int], tuple[dict[str, Any] | None, str]
    ] = {}
    for projection in projections.values():
        roots = {
            field.removesuffix("_expr"): int(projection.get(field, 0))
            for field in PROJECTION_EXPRESSION_FIELDS
        }
        if any(roots.values()):
            rooted_projections += 1
        if int(projection.get("value_flow_complete", 0)) == 1:
            complete_value_flow_projections += 1
        reached_by_slot: dict[str, set[int]] = {}
        projection_factor_coverage: dict[
            tuple[int, int], dict[str, set[Any]]
        ] = {}
        for slot, root in roots.items():
            if root and root not in expressions:
                missing_projection_expression_roots.add(root)
                continue
            reached = reachable_expressions(root, expressions)
            reached_by_slot[slot] = reached
            root_expression = expressions.get(root)
            if root_expression is not None and int(
                root_expression.get("complete", 0)
            ) != 1:
                found_boundary = False
                for expression_id in reached:
                    expression = expressions[expression_id]
                    if int(expression.get("op", -1)) != VALUE_OP["boundary"]:
                        continue
                    found_boundary = True
                    boundary_kind = int(expression.get("immediate", 0))
                    key = (
                        slot,
                        boundary_kind,
                        int(expression.get("pc", 0)),
                        int(expression.get("address", -1)),
                    )
                    boundary = boundary_uses.setdefault(
                        key,
                        {
                            "root_slot": slot,
                            "boundary_kind": boundary_kind,
                            "boundary_name": BOUNDARY_KIND.get(
                                boundary_kind, "unknown"
                            ),
                            "pc": int(expression.get("pc", 0)),
                            "source_slot": int(expression.get("address", -1)),
                            "expression_ids": set(),
                            "projection_ids": set(),
                        },
                    )
                    boundary["expression_ids"].add(expression_id)
                    boundary["projection_ids"].add(
                        int(projection["projection_id"])
                    )
                if not found_boundary:
                    incomplete_roots_without_boundary[slot] += 1

            for candidate_id in reached & local_candidates.keys():
                use = candidate_uses.setdefault(
                    candidate_id,
                    {
                        **local_candidates[candidate_id],
                        "projection_ids": set(),
                        "root_slots": set(),
                    },
                )
                use["projection_ids"].add(int(projection["projection_id"]))
                use["root_slots"].add(slot)
                candidate = local_candidates[candidate_id]
                factor = (
                    candidate["factor_numerator"],
                    candidate["factor_denominator"],
                )
                coverage = projection_factor_coverage.setdefault(
                    factor, {"root_slots": set(), "expression_ids": set()}
                )
                coverage["root_slots"].add(slot)
                coverage["expression_ids"].add(candidate_id)

        projection_component_factors: dict[
            tuple[int, int], dict[str, Any]
        ] = {}
        for component, root_slot, offset, width in TRANSFORM_COMPONENTS:
            root = roots[root_slot]
            candidate_descendants = (
                reached_by_slot.get(root_slot, set()) & local_candidates.keys()
            )
            if not root or not candidate_descendants:
                continue
            route, reason = route_component_scale(
                root, offset, width, expressions, local_candidates,
                support_memo, candidate_reach_memo, route_memo,
            )
            if route is None:
                if reason == "no-candidate-in-component":
                    continue
                failure_key = (component, root_slot, reason)
                failure = component_route_failures.setdefault(
                    failure_key,
                    {
                        "component": component,
                        "root_slot": root_slot,
                        "reason": reason,
                        "projection_ids": set(),
                        "candidate_expression_ids": set(),
                    },
                )
                failure["projection_ids"].add(int(projection["projection_id"]))
                failure["candidate_expression_ids"].update(
                    candidate_descendants
                )
                continue

            candidate_id = int(route["candidate_expression_id"])
            candidate = local_candidates[candidate_id]
            factor = (
                candidate["factor_numerator"],
                candidate["factor_denominator"],
            )
            root_expression = expressions.get(root)
            route_complete = (
                root_expression is not None
                and int(root_expression.get("complete", 0)) == 1
                and candidate["expression_complete"]
                and not candidate["multiplication_wrapped"]
            )
            component_factor = projection_component_factors.setdefault(
                factor,
                {
                    "components": {},
                    "expression_ids": set(),
                    "all_routes_complete": True,
                },
            )
            component_factor["components"][component] = int(
                route["post_scale_sign"]
            )
            component_factor["expression_ids"].add(candidate_id)
            component_factor["all_routes_complete"] &= route_complete
            exact_component_scale_routes += 1

        for factor, coverage in projection_component_factors.items():
            signature = tuple(sorted(coverage["components"].items()))
            key = (*factor, signature)
            aggregate = component_factor_groups.setdefault(
                key,
                {
                    "factor_numerator": factor[0],
                    "factor_denominator": factor[1],
                    "components": [component for component, _ in signature],
                    "post_scale_signs": {
                        component: sign for component, sign in signature
                    },
                    "projection_ids": set(),
                    "expression_ids": set(),
                    "all_routes_complete": True,
                },
            )
            aggregate["projection_ids"].add(int(projection["projection_id"]))
            aggregate["expression_ids"].update(coverage["expression_ids"])
            aggregate["all_routes_complete"] &= coverage[
                "all_routes_complete"
            ]

        for factor, coverage in projection_factor_coverage.items():
            root_slots = tuple(sorted(coverage["root_slots"]))
            key = (*factor, root_slots)
            aggregate = factor_coverage_groups.setdefault(
                key,
                {
                    "factor_numerator": factor[0],
                    "factor_denominator": factor[1],
                    "root_slots": list(root_slots),
                    "projection_ids": set(),
                    "expression_ids": set(),
                },
            )
            aggregate["projection_ids"].add(int(projection["projection_id"]))
            aggregate["expression_ids"].update(coverage["expression_ids"])

    fixed_point_candidates = []
    for candidate in candidate_uses.values():
        candidate["projection_ids"] = sorted(candidate["projection_ids"])
        candidate["root_slots"] = sorted(candidate["root_slots"])
        candidate["single_expression_reaches_all_transform_word_roots"] = set(
            candidate["root_slots"]
        ).issuperset({"rt0", "rt1", "rt2", "rt3", "rt4",
                      "tr0", "tr1", "tr2"})
        fixed_point_candidates.append(candidate)
    fixed_point_candidates.sort(key=lambda item: item["shift_expression_id"])

    fixed_point_factor_coverage = []
    required_transform_roots = {
        "rt0", "rt1", "rt2", "rt3", "rt4", "tr0", "tr1", "tr2"
    }
    for coverage in factor_coverage_groups.values():
        coverage["projection_ids"] = sorted(coverage["projection_ids"])
        coverage["expression_ids"] = sorted(coverage["expression_ids"])
        coverage["operation_count"] = len(coverage["expression_ids"])
        coverage["submitted_projection_count"] = sum(
            submitted_uses_by_projection.get(projection_id, 0) != 0
            for projection_id in coverage["projection_ids"]
        )
        coverage["submitted_uses"] = sum(
            submitted_uses_by_projection.get(projection_id, 0)
            for projection_id in coverage["projection_ids"]
        )
        coverage["covers_all_transform_word_roots"] = set(
            coverage["root_slots"]
        ).issuperset(required_transform_roots)
        coverage["word_root_coverage_is_gauge_proof"] = False
        fixed_point_factor_coverage.append(coverage)
    fixed_point_factor_coverage.sort(
        key=lambda item: (
            Fraction(item["factor_numerator"], item["factor_denominator"]),
            tuple(item["root_slots"]),
        )
    )

    fixed_point_component_coverage = []
    for coverage in component_factor_groups.values():
        coverage["projection_ids"] = sorted(coverage["projection_ids"])
        coverage["expression_ids"] = sorted(coverage["expression_ids"])
        coverage["operation_count"] = len(coverage["expression_ids"])
        coverage["component_count"] = len(coverage["components"])
        coverage["submitted_projection_count"] = sum(
            submitted_uses_by_projection.get(projection_id, 0) != 0
            for projection_id in coverage["projection_ids"]
        )
        coverage["submitted_uses"] = sum(
            submitted_uses_by_projection.get(projection_id, 0)
            for projection_id in coverage["projection_ids"]
        )
        components = set(coverage["components"])
        coverage["covers_all_rotation_components"] = (
            components.issuperset(ROTATION_COMPONENTS)
        )
        coverage["covers_all_translation_components"] = (
            components.issuperset(TRANSLATION_COMPONENTS)
        )
        coverage["covers_full_affine_transform"] = (
            components.issuperset(AFFINE_FIELDS)
        )
        # Exact component ownership closes the packed-word ambiguity, but a
        # repeated source identity and holdout/post-lift parity are still
        # required before this can become a presentation gauge.
        coverage["component_routing_is_gauge_proof"] = False
        fixed_point_component_coverage.append(coverage)
    fixed_point_component_coverage.sort(
        key=lambda item: (
            Fraction(item["factor_numerator"], item["factor_denominator"]),
            tuple(item["components"]),
            tuple(item["post_scale_signs"].items()),
        )
    )

    component_routing_failures = []
    for failure in component_route_failures.values():
        failure["projection_ids"] = sorted(failure["projection_ids"])
        failure["candidate_expression_ids"] = sorted(
            failure["candidate_expression_ids"]
        )
        failure["projection_count"] = len(failure["projection_ids"])
        component_routing_failures.append(failure)
    component_routing_failures.sort(
        key=lambda item: (
            item["component"], item["root_slot"], item["reason"]
        )
    )

    value_flow_boundaries = []
    for boundary in boundary_uses.values():
        projection_ids = sorted(boundary.pop("projection_ids"))
        boundary["expression_ids"] = sorted(boundary["expression_ids"])
        boundary["projection_count"] = len(projection_ids)
        boundary["projection_first"] = projection_ids[0]
        boundary["projection_last"] = projection_ids[-1]
        value_flow_boundaries.append(boundary)
    value_flow_boundaries.sort(
        key=lambda item: (
            item["root_slot"], item["boundary_kind"], item["pc"],
            item["source_slot"],
        )
    )

    orphan_ids = sorted(set(projections) - submitted_ids)
    return {
        "schema": REPORT_SCHEMA,
        "counts": {
            "trace_records": trace_records,
            "mips_value_expressions": len(expressions),
            "gte_projections": len(projections),
            "value_flow_rooted_projections": rooted_projections,
            "complete_value_flow_projections": complete_value_flow_projections,
            "complete_value_flow_joined_words": (
                complete_value_flow_joined_words
            ),
            "model_value_flow_rooted_projections": model_rooted_projections,
            "complete_model_value_flow_projections": len(
                model_flow_by_projection
            ),
            "distinct_model_value_flow_lineages": len(source_lineages),
            "distinct_model_value_flow_programs": len(program_lineages),
            "runtime_model_fingerprint_projections": (
                runtime_model_fingerprint_projections
            ),
            "gpu_model_fingerprint_joined_words": (
                gpu_model_fingerprint_joined_words
            ),
            "dma_nodes": dma_nodes,
            "explicit_dma_projection_words": explicit_dma_projection_words,
            "submitted_projection_words": len(submitted),
            "joined_projection_words": len(joined),
            "dma_joined_projection_words": dma_joined,
            "out_of_window_projection_words": out_of_window_projection_words,
            "source_complete_projection_words": source_complete,
            "distinct_affine_transforms": len(snapshots),
            "distinct_transform_lineages": len(lineages),
            "exact_affine_gauge_groups": len(gauge_groups),
            "exact_component_scale_routes": exact_component_scale_routes,
            "component_factor_coverage_groups": len(
                fixed_point_component_coverage
            ),
            "component_routing_failure_groups": len(
                component_routing_failures
            ),
        },
        "integrity": {
            "projection_id_collisions": projection_collisions,
            "expression_id_collisions": sorted(set(expression_collisions)),
            "missing_expression_dependencies": sorted(
                missing_expression_dependencies
            ),
            "missing_projection_expression_roots": sorted(
                missing_projection_expression_roots
            ),
            "transform_hash_collisions": hash_collisions,
            "model_value_flow_digest_collisions": model_digest_collisions,
            "runtime_model_fingerprint_mismatches": (
                runtime_fingerprint_mismatches
            ),
            "runtime_model_fingerprint_collisions": (
                runtime_fingerprint_collisions
            ),
            "gpu_model_fingerprint_mismatches": (
                gpu_model_fingerprint_mismatches
            ),
            "dma_word_provenance_mismatches": dma_word_provenance_mismatches,
            "missing_projection_ids": sorted(set(missing_projection_ids)),
            "unsubmitted_projection_ids": orphan_ids,
            "zero_affine_transforms": zero_transforms,
        },
        "exact_affine_gauge_groups": gauge_groups,
        "fixed_point_scale_candidates": fixed_point_candidates,
        "fixed_point_factor_coverage": fixed_point_factor_coverage,
        "fixed_point_component_coverage": fixed_point_component_coverage,
        "component_routing_failures": component_routing_failures,
        "value_flow_boundaries": value_flow_boundaries,
        "model_value_flow_lineages": model_lineage_report,
        "model_value_flow_programs": model_program_report,
        "incomplete_roots_without_boundary": dict(
            sorted(incomplete_roots_without_boundary.items())
        ),
        "transform_lineages": lineage_report,
        "prior_assessment": {
            "likely_maximum_family_count": (
                LIKELY_MAXIMUM_PRESENTATION_FAMILIES
            ),
            "family_count_is_correctness_limit": False,
            "tenths_are_correctness_requirement": False,
            # Exact-affine groups are candidate relations, not certified
            # temporal object families. This size can only raise a review
            # signal; it cannot reject or merge a proved ratio.
            "exact_affine_group_count_is_family_count": False,
            "largest_candidate_group_member_count": largest_candidate_group,
            "candidate_group_exceeds_likely_family_count": (
                largest_candidate_group
                > LIKELY_MAXIMUM_PRESENTATION_FAMILIES
            ),
            "all_reported_relative_gauges_expressible_in_tenths": (
                all_reported_gauges_in_tenths
            ),
        },
        "activation_note": (
            "Exact affine ratios are candidates only; certification additionally "
            "requires guest dataflow or canonical affine proof, complete stable "
            "identity, independent holdout and post-lift owner parity. Complete "
            "model DAG digests identify exact source/program lineages but are not "
            "by themselves object identities. The likely "
            "count of four and integer/tenth ratios are advisory priors, never "
            "acceptance limits."
        ),
    }


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("trace", type=Path, help="ps1trace/v1 JSONL capture")
    parser.add_argument("--output", type=Path, help="write report JSON here")
    parser.add_argument("--compact", action="store_true", help="omit indentation")
    args = parser.parse_args()

    try:
        report = analyze(read_trace(args.trace))
    except (OSError, ValueError) as error:
        parser.error(str(error))

    encoded = json.dumps(
        report,
        indent=None if args.compact else 2,
        sort_keys=True,
    ) + "\n"
    if args.output is None:
        sys.stdout.write(encoded)
    else:
        args.output.write_text(encoded, encoding="utf-8")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
