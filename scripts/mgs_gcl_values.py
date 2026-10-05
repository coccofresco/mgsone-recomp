"""Read-only GCL value/expression decoder shared by campaign diagnostics.

Grammar follows FoxdieTeam/mgs_reversing libgcl/parse.c, variable.c and expr.c.
Extracted from the existing local GCL analysis utility; no retail bytecode is
embedded. Unknown operands remain explicit errors with their original bytes.
"""
from __future__ import annotations

GCL_TYPE_NAMES = {
    0x00: "end",
    0x01: "short",
    0x02: "byte",
    0x03: "char",
    0x04: "bool",
    0x05: "vector",
    0x06: "strid",
    0x07: "string",
    0x08: "procid",
    0x09: "int",
    0x0A: "symbol",
}
GCL_TAG_NAMES = {
    0x00: "const",
    0x10: "var",
    0x20: "array",
    0x30: "expr",
    0x31: "op",
    0x40: "arg",
    0x50: "option",
    0x60: "command",
    0x70: "proc",
}
GCL_OPERATORS = {
    0: ("end", 0),
    1: ("negate", 1),
    2: ("not", 1),
    3: ("bitwise_not", 1),
    4: ("add", 2),
    5: ("sub", 2),
    6: ("mul", 2),
    7: ("div", 2),
    8: ("mod", 2),
    9: ("eq", 2),
    10: ("ne", 2),
    11: ("lt", 2),
    12: ("le", 2),
    13: ("gt", 2),
    14: ("ge", 2),
    15: ("bitwise_or", 2),
    16: ("bitwise_and", 2),
    17: ("bitwise_xor", 2),
    18: ("logical_or", 2),
    19: ("logical_and", 2),
    20: ("set", 2),
}
GCL_OPERATOR_SYMBOLS = {
    1: "-", 2: "!", 3: "~", 4: "+", 5: "-", 6: "*", 7: "/", 8: "%",
    9: "==", 10: "!=", 11: "<", 12: "<=", 13: ">", 14: ">=",
    15: "|", 16: "&", 17: "^", 18: "||", 19: "&&", 20: "=",
}
GCL_UNARY_OPERATORS = {1, 2, 3}


def _signed(data: bytes) -> int:
    return int.from_bytes(data, "big", signed=True)


def _unsigned(data: bytes) -> int:
    return int.from_bytes(data, "big", signed=False)


def gcl_variable(raw: int) -> dict:
    """Return the identity fields derived by GCL_GetVar in mgs_reversing."""
    type_code = ((raw << 1) >> 25) & 0xF
    scope_bits = raw & 0xF00000
    if scope_bits == 0x800000:
        scope, storage = "link", "linkvarbuf"
    elif scope_bits == 0x400000:
        scope, storage = "system", "var_buf"
    elif scope_bits == 0x100000:
        scope, storage = "local", "var_buf"
    else:
        scope, storage = "global", "var_buf"
    result = {
        "raw": f"0x{raw:08x}",
        "scope": scope,
        "storage": storage,
        "offset": raw & 0xFFFF,
        "type_code": type_code,
        "type": GCL_TYPE_NAMES.get(type_code, f"type_0x{type_code:x}"),
    }
    if type_code == 0x04:
        result["flag_bit"] = 1 << (((raw << 1) >> 17) & 0xF)
    return result


def _literal(type_code: int, value, raw_hex: str) -> dict:
    return {
        "kind": "literal",
        "type_code": type_code,
        "type": GCL_TYPE_NAMES.get(type_code, f"type_0x{type_code:x}"),
        "value": value,
        "raw_hex": raw_hex,
    }


def _pretty(node: dict) -> str:
    if node.get("kind") == "variable":
        var = node["variable"]
        suffix = f"@bit{var['flag_bit']}" if "flag_bit" in var else ""
        return f"{var['scope']}[{var['offset']:#06x}:{var['type']}]{suffix}"
    if node.get("kind") == "literal":
        value = node.get("value")
        if isinstance(value, str):
            return repr(value)
        return str(value)
    if node.get("kind") == "opaque":
        return f"opaque({node.get('raw_hex', '')})"
    if node.get("kind") == "expr":
        return node.get("pretty", "expr(?)")
    if node.get("kind") == "unary":
        return f"{node['symbol']}{_pretty(node['operand'])}"
    if node.get("kind") == "binary":
        return f"({_pretty(node['left'])} {node['symbol']} {_pretty(node['right'])})"
    return "?"


def decode_gcl_value(data: bytes, pos: int = 0, limit: int | None = None) -> tuple[dict, int, list[str]]:
    """Decode the value grammar used by GCL_GetNextValue.

    The return value is (AST node, next position, errors).  Keeping errors in
    the result lets a malformed/unknown retail record stay in the tree with
    its exact bytes instead of making the whole catalogue fail.
    """
    if limit is None:
        limit = len(data)
    limit = min(limit, len(data))
    errors: list[str] = []
    if pos >= limit:
        return {"kind": "opaque", "raw_hex": "", "error": "value starts past expression"}, pos, ["value starts past expression"]
    type_byte = data[pos]
    tag = type_byte & 0xF0
    type_code = type_byte & 0x0F

    # GCL_GetVar receives the complete four-byte word beginning at type_byte.
    if tag == 0x10:
        if pos + 4 > limit:
            raw_hex = data[pos:limit].hex()
            error = "truncated GCL_VAR"
            return {"kind": "opaque", "raw_hex": raw_hex, "error": error}, limit, [error]
        raw = _unsigned(data[pos:pos + 4])
        variable = gcl_variable(raw)
        return {"kind": "variable", "variable": variable, "raw_hex": data[pos:pos + 4].hex()}, pos + 4, errors

    if tag != 0x00:
        if tag == 0x30:
            if pos + 2 > limit:
                error = "truncated GCL_EXPR header"
                return {"kind": "opaque", "raw_hex": data[pos:limit].hex(), "error": error}, limit, [error]
            length = data[pos + 1]
            expr_end = pos + 1 + length
            if expr_end > limit:
                errors.append(f"GCL_EXPR length {length} exceeds payload ({limit - pos - 1})")
                expr_end = limit
            ast, consumed, expr_errors, tokens = decode_gcl_expr(data, pos + 2, expr_end)
            errors.extend(expr_errors)
            result = {
                "kind": "expr",
                "length": length,
                "payload_start": pos + 2,
                "payload_end": expr_end,
                "consumed": consumed,
                "tokens": tokens,
                "ast": ast,
                "pretty": _pretty(ast) if ast else "expr(?)",
                "raw_hex": data[pos:expr_end].hex(),
            }
            return result, expr_end, errors
        if tag == 0x20:
            # Arrays are arguments resolved by GCL_GetArgs; preserve their
            # count and bytes because the runtime argument stack is external.
            if pos + 2 > limit:
                error = "truncated GCL_ARRAY"
                return {"kind": "opaque", "raw_hex": data[pos:limit].hex(), "error": error}, limit, [error]
            count = data[pos + 1]
            return {"kind": "opaque", "tag": "array", "count": count, "raw_hex": data[pos:pos + 2].hex()}, pos + 2, errors
        if tag == 0x40:
            if pos + 3 > limit:
                error = "truncated GCL_ARG"
                return {"kind": "opaque", "raw_hex": data[pos:limit].hex(), "error": error}, limit, [error]
            size = _unsigned(data[pos + 1:pos + 3])
            end = min(limit, pos + 1 + size)
            return {"kind": "opaque", "tag": "arg", "size": size, "raw_hex": data[pos:end].hex()}, end, errors
        if tag == 0x50:
            if pos + 3 > limit:
                error = "truncated GCL_OPTION"
                return {"kind": "opaque", "raw_hex": data[pos:limit].hex(), "error": error}, limit, [error]
            option = data[pos + 1]
            size = data[pos + 2]
            end = min(limit, pos + 3 + size - 1)
            return {"kind": "opaque", "tag": "option", "option": option, "size": size, "raw_hex": data[pos:end].hex()}, end, errors
        error = f"unsupported GCL tag 0x{tag:02x}"
        return {"kind": "opaque", "tag": GCL_TAG_NAMES.get(tag, f"tag_0x{tag:02x}"), "raw_hex": data[pos:pos + 1].hex(), "error": error}, pos + 1, [error]

    widths = {0x01: 2, 0x02: 1, 0x03: 1, 0x04: 1, 0x06: 2, 0x08: 2, 0x09: 4, 0x0A: 4}
    if type_code == 0x00:
        return {"kind": "literal", "type": "end", "value": None, "raw_hex": "00"}, pos + 1, errors
    if type_code == 0x07:
        if pos + 2 > limit:
            error = "truncated GCL_STRING"
            return {"kind": "opaque", "raw_hex": data[pos:limit].hex(), "error": error}, limit, [error]
        size = data[pos + 1]
        if pos + 2 + size > limit:
            errors.append(f"GCL_STRING length {size} exceeds payload ({limit - pos - 2})")
        end = min(limit, pos + 2 + size)
        raw = data[pos + 2:end]
        return _literal(type_code, raw.decode("latin-1", errors="replace"), data[pos:end].hex()), end, errors
    if type_code not in widths:
        # GCL_VECTOR is deliberately left raw: GCL_GetNextValue itself does
        # not implement it in the source used for the retail radio interpreter.
        error = f"unsupported GCL value type 0x{type_code:x}"
        return {"kind": "opaque", "type_code": type_code, "raw_hex": data[pos:pos + 1].hex(), "error": error}, pos + 1, [error]
    width = widths[type_code]
    if pos + 1 + width > limit:
        error = f"truncated GCL {GCL_TYPE_NAMES.get(type_code, type_code)}"
        return {"kind": "opaque", "raw_hex": data[pos:limit].hex(), "error": error}, limit, [error]
    raw = data[pos + 1:pos + 1 + width]
    if type_code in (0x01,):
        value = _signed(raw)
    elif type_code in (0x06, 0x08):
        value = _unsigned(raw)
    elif type_code in (0x09, 0x0A):
        value = _signed(raw)
    else:
        value = _unsigned(raw)
    return _literal(type_code, value, data[pos:pos + 1 + width].hex()), pos + 1 + width, errors


def decode_gcl_expr(data: bytes, start: int, end: int) -> tuple[dict | None, int, list[str], list[dict]]:
    stack: list[dict] = []
    tokens: list[dict] = []
    errors: list[str] = []
    pos = start
    while pos < end:
        if data[pos] == 0x31:
            if pos + 2 > end:
                errors.append("truncated GCL_OP")
                break
            op = data[pos + 1]
            name, arity = GCL_OPERATORS.get(op, (f"op_0x{op:02x}", -1))
            token = {"kind": "operator", "opcode": op, "name": name, "arity": arity, "raw_hex": data[pos:pos + 2].hex()}
            tokens.append(token)
            pos += 2
            if op == 0:
                break
            if op in GCL_UNARY_OPERATORS:
                # libgcl/expr.c always writes the result into sp[-2] and
                # decrements sp, even for OP_MNS/OP_NOT/OP_NEG.  The value at
                # sp[-2] is the conventional zero placeholder; the semantic
                # operand is sp[-1].
                token["stack_arity"] = 2
                if len(stack) < 2:
                    errors.append(f"GCL_OP {op} has fewer than two stack values")
                    continue
                operand = stack[-1]
                stack[-2:] = [{"kind": "unary", "opcode": op, "operator": name, "symbol": GCL_OPERATOR_SYMBOLS.get(op, name), "operand": operand}]
            elif arity == 2:
                if len(stack) < 2:
                    errors.append(f"GCL_OP {op} has fewer than two operands")
                    continue
                right = stack.pop()
                left = stack.pop()
                if op == 20:
                    stack.append({"kind": "binary", "opcode": op, "operator": name, "symbol": "=", "left": left, "right": right})
                else:
                    stack.append({"kind": "binary", "opcode": op, "operator": name, "symbol": GCL_OPERATOR_SYMBOLS.get(op, name), "left": left, "right": right})
            else:
                errors.append(f"unknown GCL_OP {op}")
            continue
        node, next_pos, value_errors = decode_gcl_value(data, pos, end)
        tokens.append({"kind": "value", "value": node, "raw_hex": data[pos:next_pos].hex()})
        stack.append(node)
        errors.extend(value_errors)
        if next_pos <= pos:
            errors.append("GCL value decoder made no progress")
            break
        pos = next_pos
    if not stack:
        errors.append("GCL expression has an empty stack")
        return None, pos, errors, tokens
    if len(stack) > 1:
        errors.append(f"GCL expression leaves {len(stack)} stack values")
    return stack[-1], pos, errors, tokens
