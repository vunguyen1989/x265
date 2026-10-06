#!/usr/bin/env python3
"""Integration check: FPA fields/IRAP placement, SEI grouping, and decoded pixels."""
import argparse
import json
import re
import subprocess
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]

def run(args):
    return subprocess.run(list(map(str, args)), check=True, capture_output=True)

def inspect(path):
    data = run(['ffmpeg', '-v', 'verbose', '-i', path, '-c', 'copy',
                '-bsf:v', 'trace_headers', '-f', 'null', '-']).stderr.decode()
    fields = {}
    current = None
    pending = False
    iraps = fpas = 0
    for line in data.splitlines():
        if 'Frame Packing Arrangement' in line:
            assert not pending, 'duplicate FPA before picture'
            pending = True
            current = {}
            fpas += 1
        match = re.search(r'\b([a-z][a-z0-9_]+)\s+[01]+\s+=\s+(\d+)\s*$', line)
        if not match:
            continue
        key, value = match[1], int(match[2])
        if key.startswith('fp_arrangement_'):
            key = key.replace('fp_arrangement_', 'frame_packing_arrangement_', 1)
        elif key.startswith('fp_'):
            key = key[3:]
        if current is not None and key in EXPECTED:
            current[key] = value
            if key == 'upsampled_aspect_ratio_flag':
                assert current == EXPECTED, current
                fields = current
                current = None
        if key == 'nal_unit_type' and value <= 31:
            if 16 <= value <= 23:
                iraps += 1
                assert pending, 'IRAP missing FPA'
            else:
                assert not pending, 'FPA before non-IRAP'
            pending = False
    assert not pending
    return fpas, iraps, fields

EXPECTED = dict(frame_packing_arrangement_id=0,
    frame_packing_arrangement_cancel_flag=0, frame_packing_arrangement_type=3,
    quincunx_sampling_flag=0, content_interpretation_type=1,
    spatial_flipping_flag=0, frame0_flipped_flag=0, field_views_flag=0,
    current_frame_is_frame0_flag=0, frame0_self_contained_flag=0,
    frame1_self_contained_flag=0, frame0_grid_position_x=0,
    frame0_grid_position_y=0, frame1_grid_position_x=0,
    frame1_grid_position_y=0, frame_packing_arrangement_reserved_byte=0,
    frame_packing_arrangement_persistence_flag=1, upsampled_aspect_ratio_flag=0)

def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--frames', type=int, default=48)
    opts = parser.parse_args()
    binary = ROOT / 'build/debug/x265'
    fixture = ROOT / 'issues/970_worklog/step1/1.3.1_sbs-test.y4m'
    base = [binary, '--frames', opts.frames, '--preset', 'ultrafast', '--qp', 28,
            '--bframes', 0, '--keyint', 24, '--min-keyint', 24, '--scenecut', 0,
            '--frame-threads', 1, '--pools', 'none', '--no-wpp', '--no-progress']
    results = {}
    with tempfile.TemporaryDirectory(prefix='970-fpa-') as tmp:
        outputs = {}
        for name, extra in [('control', []), ('sbs', ['--frame-packing', 3]),
                            ('single', ['--frame-packing', 3, '--single-sei']),
                            ('single-repeat', ['--frame-packing', 3, '--single-sei', '--repeat-headers']),
                            ('closed', ['--frame-packing', 3, '--no-open-gop']),
                            ('disabled', ['--frame-packing', -1])]:
            path = Path(tmp) / (name + '.h265')
            run(base + extra + ['-o', path, fixture])
            outputs[name] = path
            if name in ('control', 'disabled'):
                trace = run(['ffmpeg', '-v', 'verbose', '-i', path, '-c', 'copy',
                             '-bsf:v', 'trace_headers', '-f', 'null', '-']).stderr
                assert b'Frame Packing Arrangement' not in trace
            else:
                count, iraps, fields = inspect(path)
                assert count == iraps == (opts.frames + 23) // 24, (name, count, iraps)
                results[name] = {'fpa_count': count, 'irap_count': iraps, 'fields': fields}
        hashes = {}
        for name, path in outputs.items():
            md5 = run(['ffmpeg', '-v', 'error', '-i', path, '-f', 'framemd5', '-']).stdout
            hashes[name] = [line for line in md5.splitlines() if not line.startswith(b'#')]
            assert len(hashes[name]) == opts.frames
        assert all(value == hashes['control'] for value in hashes.values())
        for value in ['0', '4', '5', '7', '-2', 'abc', '3abc']:
            result = subprocess.run(list(map(str, base + ['--frame-packing', value,
                '-o', Path(tmp) / 'invalid.h265', fixture])), capture_output=True)
            assert result.returncode != 0, value
    print(json.dumps({'frames': opts.frames, 'results': results,
                      'decoded_pixels_equal': True, 'invalid_values_rejected': True}, indent=2))

if __name__ == '__main__':
    main()
