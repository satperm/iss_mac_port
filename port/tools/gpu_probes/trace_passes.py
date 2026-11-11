#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""GPU time per kind of render pass from a Metal System Trace recorded with TLG_PASS_LABELS=1 (the
patched MoltenVK names each render encoder after its "tlgpass WxH [formats] d:depth vs:.. ps:.." label):

    python3 trace_passes.py x.xml <frames in the trace> [--by-shader]

x.xml: the metal-gpu-intervals table (export as for trace_busy.py). Prints ms per frame per pass kind
(size, targets, depth; with --by-shader also the first draw's shaders), vertex and fragment separately,
how many such passes a frame, and the compute/blit work by encoder label."""
import collections
import re
import sys
import xml.etree.ElementTree as ET


def main():
    frames = float(sys.argv[2])
    by_shader = '--by-shader' in sys.argv
    text, cols = {}, None
    time = collections.defaultdict(lambda: [0, 0, set()])  # label -> [vertex ns, fragment/compute ns, encoders]
    for kind, e in ET.iterparse(sys.argv[1], events=('start', 'end')):
        if kind == 'start':
            if e.tag == 'schema':
                cols = []
            continue
        if e.get('id'):
            text[e.get('id')] = (e.text, e.get('fmt'))
        if e.tag == 'mnemonic' and cols is not None:
            cols.append(e.text)
        if e.tag != 'row':
            continue
        values = [text.get(x.get('ref')) if x.get('ref') else (x.text, x.get('fmt')) for x in e]
        d = dict(zip(cols, values))
        e.clear()
        if 'tlggpu' not in str(d['process'][1]) or d['event-depth'][0] != '0':
            continue
        label = (d['event-label'][1] or '').split('  (tlggpu')[0]
        label = label.split(':', 1)[1] if ':' in label else label
        if label.startswith('tlgpass '):
            if not by_shader:
                label = re.sub(r' vs:\w+ ps:\w+', '', label)
        else:
            label = re.sub(r'Command Buffer \d+', 'Command Buffer N', label)
        entry = time[label]
        entry[0 if d['channel-name'][1] == 'Vertex' else 1] += int(d['duration'][0])
        entry[2].add(d['encoder-id'][0])
    total = sum(v[0] + v[1] for v in time.values())
    print(f'{total / 1e6 / frames:7.2f} ms GPU work a frame (sum of intervals; overlapping work counts twice)')
    print('  ms/frame  vertex  frag/compute  encoders/frame  label')
    for label, (vertex, other, encoders) in sorted(time.items(), key=lambda kv: -(kv[1][0] + kv[1][1]))[:40]:
        print(f'  {(vertex + other) / 1e6 / frames:7.2f} {vertex / 1e6 / frames:7.2f} {other / 1e6 / frames:9.2f} '
              f'{len(encoders) / frames:10.1f}      {label[:110]}')


if __name__ == '__main__':
    main()
