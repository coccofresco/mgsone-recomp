#!/usr/bin/env python3
"""Compare Crash source catalogues/programs with an independently captured RAM.

The left input is a checksum-validated PS1S; the right is a raw 2 MiB emulator
RAM dump. No savestate conversion, game writes, camera synchronization, decoded
SLST equivalence or texture-pixel equivalence is implied by this source check.
Reports contain hashes/identities only, not game assets.
"""
import argparse
import hashlib
import json
import struct
from pathlib import Path

from crash3_scenery_variant_audit import RAM_SIZE, inspect_ram, load_ram


def ordered_programs(ram, eid):
    """Find all valid relocated type-4 entries; preserve item order/terminal."""
    if len(ram) != RAM_SIZE:
        raise ValueError("oracle RAM must be exactly 2 MiB")
    signature = struct.pack("<III", 0x0100ffff, eid, 4)
    found = []
    cursor = 0
    while (address := ram.find(signature, cursor)) >= 0:
        cursor = address + 1
        if address % 4 or address + 16 > len(ram):
            continue
        count, = struct.unpack_from("<I", ram, address+12)
        if not 2 <= count <= 4097 or address+16+4*count > len(ram):
            continue
        items = []
        for index in range(count):
            pointer, = struct.unpack_from("<I", ram, address+16+4*index)
            physical = pointer & 0x1fffffff
            if physical % 2 or physical+4 > len(ram):
                break
            words, kind = struct.unpack_from("<HH", ram, physical)
            if (kind != (0 if index in (0, count-1) else 1) or
                    physical+4+2*words > len(ram) or (kind == 1 and words < 2)):
                break
            items.append(ram[physical:physical+4+2*words])
        if len(items) == count:
            found.append(dict(entry_offset=address, state_count=count-1,
                              program_sha256=hashlib.sha256(b"".join(items)).hexdigest(),
                              ordered_item_sha256=[hashlib.sha256(i).hexdigest() for i in items]))
    return found


def compare_program(left, right, eid):
    a, b = ordered_programs(left, eid), ordered_programs(right, eid)
    # Multiple resident copies are allowed only when their full contents agree.
    identities = lambda rows: {(r["state_count"], r["program_sha256"]) for r in rows}
    return dict(program_eid=eid, checkpoint=a, donor=b,
                matches=bool(a and b and len(identities(a)) == 1 and
                             identities(a) == identities(b)))


def compare_catalogues(left, right):
    a, b = (inspect_ram(r, include_source_faces=True) for r in (left, right))
    digest = lambda r: hashlib.sha256(json.dumps(
        {k:v for k,v in r.items() if k != "world"},
        sort_keys=True, separators=(",", ":")).encode()).hexdigest()
    records = []
    for resource in a:
        matches = [r for r in b if r["resource_id"] == resource["resource_id"]]
        records.append(dict(resource_id=resource["resource_id"],
                            geometry_fingerprint=resource["geometry_fingerprint"],
                            source_faces=len(resource["source_faces"]),
                            checkpoint_catalogue_sha256=digest(resource),
                            donor_catalogue_sha256=[digest(r) for r in matches],
                            matches=len(matches) == 1 and digest(resource) == digest(matches[0])))
    return records


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("checkpoint", type=Path)
    parser.add_argument("donor_ram", type=Path)
    parser.add_argument("--program-eid", action="append", required=True,
                        type=lambda v: int(v, 0))
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    if args.output.resolve() in {args.checkpoint.resolve(), args.donor_ram.resolve()}:
        parser.error("output must not overwrite an input")
    try:
        left, right = load_ram(args.checkpoint), args.donor_ram.read_bytes()
        if len(right) != RAM_SIZE:
            raise ValueError("donor RAM must be exactly 2 MiB, not an emulator savestate")
        resources = compare_catalogues(left, right)
        programs = [compare_program(left, right, eid) for eid in args.program_eid]
    except (ValueError, OSError, struct.error) as error:
        parser.error(str(error))
    report = dict(schema="crash3-independent-source-oracle/v1",
                  checkpoint_sha256=hashlib.sha256(args.checkpoint.read_bytes()).hexdigest(),
                  checkpoint_ram_sha256=hashlib.sha256(left).hexdigest(),
                  donor_ram_sha256=hashlib.sha256(right).hexdigest(),
                  resources=resources, programs=programs,
                  passed=all(r["matches"] for r in resources+programs),
                  scope="source catalogues and ordered raw programs; not pose/frame or decoded-state equivalence")
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(report, indent=2)+"\n")
    print(json.dumps(dict(passed=report["passed"], resources=len(resources), programs=len(programs))))
    return 0 if report["passed"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
