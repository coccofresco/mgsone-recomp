"""Discover USA stage code and callback candidates from original .b resources.

This is compilation coverage, not gameplay validation. Keep the byte hashes and
the derivation in the generated report; validate reached stages with native TAS
and the independent donor. No retail bytes are copied into tracked files.
"""
import hashlib
import struct

BASE = 0x800C5968


def code_extent(payload):
    """Stage linker layout: actor table, text, then strings/data/BSS.

    The final aligned JR RA and its delay slot define the candidate text end.
    This agrees with all five previously donor-verified USA stage extents.
    It is deliberately not a general-purpose MIPS code/data classifier.
    """
    returns = [i for i in range(0, len(payload) - 7, 4)
               if payload[i:i + 4] == b"\x08\x00\xe0\x03"]
    if not returns:
        raise ValueError("stage resource has no complete return/delay pair")
    return returns[-1] + 8


def callback_entries(payload, size):
    words = struct.unpack_from(f"<{size // 4}I", payload)
    found = set()

    def candidate(address, actor=False):
        offset = address - BASE
        if offset < 0 or offset >= size or offset % 4:
            return
        if actor or (offset >= 8 and words[offset // 4 - 2:offset // 4] == (0x03E00008, 0)):
            found.add(address)

    # The prefix is the CHARA {16-bit name hash, constructor pointer} table.
    table_size = 0
    while table_size + 8 <= size:
        name, address = struct.unpack_from('<II', payload, table_size)
        if name == address == 0:
            break
        if not 0 < name <= 0xFFFF or not 0x80010000 <= address < 0x80200000:
            break
        candidate(address, actor=True)
        table_size += 8
    if table_size == 0:
        raise ValueError('stage has no recognizable actor table')

    # Some overlays store leaf callbacks in pointer tables, without ever
    # materializing their addresses with LUI/ADDIU. s12c's snow renderer is
    # one example. Require the same standalone JR RA/NOP boundary as below:
    # interior jump-table labels must not become function entry candidates.
    for offset in range(table_size, len(payload) - 3, 4):
        candidate(struct.unpack_from('<I', payload, offset)[0])

    # Constant propagation only through straight-line LUI/ADDIU/ORI. A callback
    # lacking prologue/JAL still needs an explicit address materialization and
    # the preceding canonical return; do not split every jump-table handler.
    constants = [None] * 32
    constants[0] = 0
    for word in words[table_size // 4:]:
        op, rs, rt, rd = word >> 26, (word >> 21) & 31, (word >> 16) & 31, (word >> 11) & 31
        imm = word & 0xFFFF
        if op == 15:
            constants[rt] = imm << 16
        elif op in (9, 13):
            value = constants[rs]
            if value is not None:
                value = ((value + (imm if imm < 32768 else imm - 65536)) & 0xFFFFFFFF) if op == 9 else value | imm
                candidate(value)
            constants[rt] = value
        elif op == 0:
            if word & 63 in (8, 9):
                constants = [None] * 32
            else:
                constants[rd] = None
        elif op in (2, 3, 1, 4, 5, 6, 7):
            constants = [None] * 32
        elif op not in (40, 41, 42, 43, 46, 50, 58):
            constants[rt] = None
        constants[0] = 0
    return sorted(found), table_size


def describe(name, payload):
    size = code_extent(payload)
    callbacks, table_size = callback_entries(payload, size)
    return {"stage": name, "sha256": hashlib.sha256(payload).hexdigest(),
            "resource_size": len(payload), "code_size": size,
            "code_end": f"0x{BASE + size:08X}", "actor_table_size": table_size,
            "additional_functions": callbacks,
            "derivation": "last aligned return plus actor/constant callback references",
            "playability": "unverified"}
