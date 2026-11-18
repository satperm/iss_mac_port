#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Source-line profile of one function from a Time Profiler export (time_profile.py's x.xml).

    python3 line_profile.py x.xml <function name part> [--thread <name part>] [--dsym <DWARF file>]

Every sample whose backtrace passes through the function is charged to the address in that function
(its own instruction, or the call it was in), and the addresses are turned into source lines with
atos (inlined frames included). The library must have debug info that matches the profiled build:
the GPU library keeps its LTO objects on macOS, so `dsymutil out/gpu-arm64/libtlggpu.dylib` right
after the build gives the dSYM (default: out/gpu-arm64/libtlggpu.dylib.dSYM)."""
import collections
import os
import subprocess
import sys
import xml.etree.ElementTree as ET


def main():
    args = sys.argv[1:]
    if len(args) < 2:
        print(__doc__)
        sys.exit(2)
    path, target = args[0], args[1]
    only = args[args.index('--thread') + 1] if '--thread' in args else None
    here = os.path.dirname(os.path.abspath(__file__))
    dsym = args[args.index('--dsym') + 1] if '--dsym' in args else os.path.join(
        here, '../../out/gpu-arm64/libtlggpu.dylib.dSYM/Contents/Resources/DWARF/libtlggpu.dylib')
    fmt, frames, binaries, stacks = {}, {}, {}, {}
    total = 0
    by_addr = collections.Counter()      # (offset, self?) -> samples
    callee = collections.defaultdict(collections.Counter)
    for _, e in ET.iterparse(path, events=('end',)):
        i = e.get('id')
        if i:
            if e.tag == 'binary':
                binaries[i] = (e.get('name'), int(e.get('load-addr', '0'), 16))
            elif e.tag == 'frame':
                b = e.find('binary')
                bin_ = None
                if b is not None:
                    bin_ = binaries.get(b.get('ref') or b.get('id'))
                frames[i] = (e.get('name') or '?', int(e.get('addr', '0'), 16), bin_)
            elif e.tag == 'tagged-backtrace':
                stacks[i] = [frames.get(f.get('ref') or f.get('id')) for f in e.findall('frame')]
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
        e.clear()
        if not stack or not proc or 'tlggpu' not in proc:
            continue
        if only and only not in (thread or ''):
            continue
        for k, f in enumerate(stack):
            if f and target in f[0] and f[2]:
                total += 1
                off = f[1] - f[2][1] - (0 if k == 0 else 4)
                by_addr[(off, k == 0)] += 1
                if k > 0 and stack[k - 1]:
                    callee[off][stack[k - 1][0][:70]] += 1
                break
    if not by_addr:
        print('no samples in', target)
        return
    offs = sorted({o for o, _ in by_addr})
    out = subprocess.run(['atos', '-o', dsym, '-arch', 'arm64', '-l', '0x0', '-i'] + [hex(o) for o in offs],
                         capture_output=True, text=True).stdout
    # atos -i prints the inline chain (innermost first) per address, a blank line between addresses.
    chains, cur = [], []
    for line in out.splitlines():
        if not line.strip():
            chains.append(cur)
            cur = []
        else:
            cur.append(line)
    if cur:
        chains.append(cur)
    where = {}
    for o, chain in zip(offs, chains):
        def loc(s):
            return s[s.rfind('(') + 1:-1] if s.endswith(')') else '?'
        inner = chain[0] if chain else '?'
        outer = loc(chain[-1]) if chain else '?'
        name = inner.split(' (in ')[0]
        where[o] = (outer, f'{loc(inner)} {name[:60]}' if len(chain) > 1 else '')
    per_line = collections.Counter()
    per_line_self = collections.Counter()
    detail = collections.defaultdict(collections.Counter)
    for (o, is_self), n in by_addr.items():
        outer, inner = where.get(o, ('?', ''))
        per_line[outer] += n
        if is_self:
            per_line_self[outer] += n
        if inner:
            detail[outer][inner] += n
        for c, m in callee[o].items():
            detail[outer]['-> ' + c] += m
    print(f'{total} samples through {target}')
    for line, n in per_line.most_common(30):
        print(f'{n:6d} {100.0 * n / total:5.1f}%  self {per_line_self[line]:5d}  {line}')
        for d, m in detail[line].most_common(3):
            print(f'              {m:6d}  {d}')


if __name__ == '__main__':
    main()
