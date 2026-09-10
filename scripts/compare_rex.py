#!/usr/bin/env python3
"""Compare original top-level stage payload hashes with a Rex extraction."""
import argparse
from collections import Counter
import json
from pathlib import Path
from mgs_assets import sha, stages, stage_members


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('stage_dir', type=Path)
    p.add_argument('rex_stage_folder', type=Path)
    p.add_argument('--out', type=Path, required=True)
    a = p.parse_args()
    expected = Counter((name, sha(payload)) for name, _, stage in stages(a.stage_dir.read_bytes())
                       for _, payload in stage_members(stage))
    actual = Counter((path.relative_to(a.rex_stage_folder).parts[0], sha(path.read_bytes()))
                     for path in a.rex_stage_folder.rglob('*') if path.is_file())
    missing, extra = expected - actual, actual - expected
    report = {'expected_files': sum(expected.values()), 'rex_files': sum(actual.values()),
              'matching_files': sum((expected & actual).values()),
              'missing_by_stage': dict(Counter(name for (name, _), count in missing.items() for _ in range(count))),
              'extra_files': sum(extra.values()),
              'note': 'Byte-level top-level comparison, not visual or gameplay validation.'}
    a.out.write_text(json.dumps(report, indent=2) + '\n')
    print(json.dumps(report, indent=2))
    return int(bool(missing or extra))


if __name__ == '__main__':
    raise SystemExit(main())
