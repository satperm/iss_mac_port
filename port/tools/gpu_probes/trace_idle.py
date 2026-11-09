#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Why the GPU idles, from a Metal System Trace (table metal-gpu-intervals, exported as for
trace_busy.py): each gap with no GPU work of the GPU process is classed by the command buffer that
starts at its end. Its commit time is start - start-latency: committed during the gap = the GPU was
starved (the CPU side had nothing ready); committed before the gap = it had work but did not start
(a semaphore or fence held it).

    python3 trace_idle.py x.xml [process substring, default tlggpu]"""
import bisect
import collections
import sys
import xml.etree.ElementTree as ET


def main():
    process = sys.argv[2] if len(sys.argv) > 2 else 'tlggpu'
    text, cols, events = {}, None, []
    for kind, e in ET.iterparse(sys.argv[1], events=('start', 'end')):
        if kind == 'start':
            if e.tag == 'schema':
                cols = []
            continue
        if e.get('id'):
            text[e.get('id')] = (e.text, e.get('fmt'))
        if e.tag == 'mnemonic' and cols is not None:
            cols.append(e.text)
        if e.tag == 'row':
            values = [text.get(x.get('ref')) if x.get('ref') else (x.text, x.get('fmt')) for x in e]
            d = dict(zip(cols, values))
            e.clear()
            if process not in str(d['process'][1]) or d['event-depth'][0] != '0':
                continue
            start = int(d['start'][0])
            events.append((start, start + int(d['duration'][0]), start - int(d['start-latency'][0] or 0),
                           d['cmdbuffer-id'][0]))
    events.sort()
    first = {}
    for start, _, commit, cmdbuffer in events:
        first.setdefault(cmdbuffer, (start, commit))
    starts = sorted(first.values())
    first_starts = [s for s, _ in starts]
    gaps, end = [], events[0][1]
    for start, stop, _, _ in events[1:]:
        if start > end:
            gaps.append((end, start))
        end = max(end, stop)
    total, count = collections.Counter(), collections.Counter()
    for g0, g1 in gaps:
        j = bisect.bisect_left(first_starts, g1 - 1000)
        if j >= len(starts):
            continue
        start, commit = starts[j]
        if abs(start - g1) > 50000:
            key = 'the gap does not end at a command buffer start'
        elif commit > g0:
            key = 'starved: the next command buffer was committed during the gap'
        else:
            key = 'had work: committed before the gap, started late'
        total[key] += g1 - g0
        count[key] += 1
    span = (max(e[1] for e in events) - events[0][0]) / 1e6
    for key, ns in total.most_common():
        print(f'{ns / 1e6:8.1f} ms {count[key]:6d} gaps  {key}')
    print(f'GPU idle {sum(total.values()) / 1e6:.1f} ms of {span:.1f} ms')


if __name__ == '__main__':
    main()
