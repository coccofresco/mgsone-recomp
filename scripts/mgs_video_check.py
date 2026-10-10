"""Shared acceptance gate for continuous original-asset Vulkan recordings.

Submission counts alone cannot establish completeness. Reuse the presentation
contract and retain hashed evidence so an edit cannot silently use an older,
incomplete checkpoint recording. This gate does not certify artistic fidelity.
"""
import csv
import json
from pathlib import Path
import re

from mgs_presentation_check import audit_metrics
from mgs_tas import sha256


def audit_video(metrics, log, expected_frames, fps=30, content='scene'):
    failures = audit_metrics(metrics, require_hardware=True, require_mesh=content!='codec')
    counters, state = metrics.get('counters', {}), metrics.get('state', {})
    if fps not in (30, 60): failures.append('unsupported recording frame rate')
    if fps == 60:
        if state.get('mgs.interpolation.enabled') != 1:
            failures.append('runtime source interpolation not enabled')
        if content != 'codec' and (counters.get('host.renderer.video.interpolated_frames', 0) <= 0 or
                                   counters.get('mgs.interpolation.moving_faces', 0) <= 0):
            failures.append('no moving source geometry interpolated in the recording')
    if content == 'codec' and counters.get('mgs.screen_effect.pass.codec_screen', 0) <= 0:
        failures.append('no Codec screen rendered')
    count = metrics.get('counters', {}).get('host.renderer.video.frames', 0)
    if expected_frames <= 0 or count != expected_frames:
        failures.append(f'incomplete continuous video: {count}/{expected_frames}')
    if '[Shutdown] guest and ticker joined cleanly' not in log:
        failures.append('missing clean shutdown')
    if re.search(r'Validation Error|\(ERROR /|invalid_depth=[1-9]', log):
        failures.append('Vulkan validation/depth errors')
    return failures



def audit_timeline(path, start, seconds, fps, interpolated):
    with Path(path).open() as source:
        rows=list(csv.DictReader(source))
    if [int(r['vblank']) for r in rows] != list(range(start,start+seconds*60,60//fps)):
        return ['recorded timeline has missing, repeated or reordered VBlanks']
    if any(r['presentation'] not in ('ordinary','midpoint','endpoint') for r in rows):
        return ['unknown recorded presentation phase']
    if sum(r['presentation']=='midpoint' for r in rows)!=interpolated:
        return ['interpolated frame count differs from the recorded timeline']
    return []


def verify_capture(video):
    """Fail closed before editing: validate both the receipt and its witnesses."""
    video = Path(video).resolve()
    report = json.loads((video.parent / 'capture.json').read_text())
    if report.get('format') != 'mgs-vulkan-video-1' or report.get('status') != 'complete':
        raise ValueError(f'video has no successful presentation audit: {video}')
    required = (video.name, 'metrics.json', 'runtime.log', 'runtime.toml')
    if report.get('fps',30)==60: required += ('video-timeline.csv',)
    for name in required:
        expected = report.get('artifacts', {}).get(name)
        path = video.parent / name
        if not expected or not path.is_file() or sha256(path) != expected:
            raise ValueError(f'video evidence changed or absent: {path}')
    metrics = json.loads((video.parent / 'metrics.json').read_text())
    failures = audit_video(metrics, (video.parent / 'runtime.log').read_text(),
                           report['seconds'] * report.get('fps', 30), report.get('fps', 30), report.get('content', 'scene'))
    if report.get('fps',30)==60:
        failures += audit_timeline(video.parent/'video-timeline.csv',report['start_vblank'],
            report['seconds'],report['fps'],report['interpolated_video_frames'])
    if failures:
        raise ValueError(f'video presentation rejected: {video}: {failures}')
    return report
