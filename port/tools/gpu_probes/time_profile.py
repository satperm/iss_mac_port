#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""CPU samples of the GPU process per thread and function, from a Time Profiler trace:

    xcrun xctrace record --template 'Time Profiler' --all-processes --time-limit 6s --output x.trace
    xcrun xctrace export --input x.trace \
        --xpath '/trace-toc/run[@number="1"]/data/table[@schema="time-profile"]' > x.xml
    python3 time_profile.py x.xml [--callers SYMBOL] [--process tlggpu]

(scripts/macos/host_profile.sh does all three during a run.) Prints samples per thread, the hottest
leaf functions, and inclusive counts for the busiest threads; --callers lists who calls a leaf.
In the export a row's process nests inside its thread, frames sit directly in tagged-backtrace, and
repeated elements are id/ref pairs. Never attach to tlg-probe (Rosetta); --all-processes is safe."""
import collections
import sys
import xml.etree.ElementTree as ET


def main():
    args = sys.argv[1:]
    callers = process = None
    if '--callers' in args:
        callers = args[args.index('--callers') + 1]
    process = args[args.index('--process') + 1] if '--process' in args else 'tlggpu'
    fmt, frames, stacks = {}, {}, {}
    per_thread = collections.Counter()
    leaf = collections.Counter()
    inclusive = collections.defaultdict(collections.Counter)
    who = collections.Counter()
    for _, e in ET.iterparse(args[0], events=('end',)):
        i = e.get('id')
        if i:
            if e.tag == 'frame':
                frames[i] = e.get('name') or e.get('addr') or '?'
            elif e.tag == 'tagged-backtrace':
                stacks[i] = [frames.get(f.get('ref')) if f.get('ref') else (f.get('name') or f.get('addr') or '?')
                             for f in e.findall('frame')]
            else:
                fmt[i] = e.get('fmt')
        if e.tag != 'row':
            continue
        thread = proc = stack = None
        for x in e:
            value = fmt.get(x.get('ref')) if x.get('ref') else x.get('fmt')
            if x.tag == 'thread':
                thread = value
            elif x.tag == 'process':
                proc = value
            elif x.tag == 'tagged-backtrace':
                stack = stacks.get(x.get('ref') or x.get('id'))
        if proc and process in proc and stack:
            t = (thread or '?').split(' (0x')[0][:22]
            per_thread[t] += 1
            leaf[(t, stack[0])] += 1
            for name in set(stack):
                inclusive[t][name] += 1
            if callers and callers in stack[0]:
                who[(t, ' < '.join(f[:60] for f in stack[1:4]))] += 1
        e.clear()
    if callers:
        for (t, chain), n in who.most_common(15):
            print(n, t, chain)
        return
    print('samples per thread:')
    for t, n in per_thread.most_common(12):
        print(f'  {n:6d} {t}')
    print('hottest leaves:')
    for (t, name), n in leaf.most_common(30):
        print(f'  {n:6d} {t}  {name[:120]}')
    skip = ('thread_start', '_pthread_start', '__thread_proxy', 'start_wqthread', '_pthread_wqthread', '_dispatch_')
    for t, _ in per_thread.most_common(4):
        print(f'inclusive, {t} ({per_thread[t]} samples):')
        shown = 0
        for name, n in inclusive[t].most_common(80):
            if any(s in name for s in skip):
                continue
            print(f'  {n:6d} {name[:130]}')
            shown += 1
            if shown >= 22:
                break


if __name__ == '__main__':
    main()
