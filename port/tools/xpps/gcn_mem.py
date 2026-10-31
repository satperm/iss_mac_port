"""Minimal GCN (gfx7) walker for the job builder: buffer memory instructions (MUBUF/MTBUF) with
their resource SGPRs, offsets, idxen and access size, and the SMRD loads that bring V# in from
the extended user data. Enough to tell, per user data V#, whether a shader writes it and the
stride its indexed accesses imply (re_compute report §2: roundup16(max offset + size) of the
idxen accesses equals the stride in 574/616 buffers with stride >= 32)."""
from __future__ import annotations

import struct
from collections import defaultdict

MUBUF_SIZE = {0: 4, 1: 8, 2: 12, 3: 16, 4: 4, 5: 8, 6: 12, 7: 16, 8: 1, 9: 1, 10: 2, 11: 2, 12: 4,
              13: 8, 14: 16, 15: 12, 24: 1, 26: 2, 28: 4, 29: 8, 30: 16, 31: 12}
MUBUF_STORE = {4, 5, 6, 7, 24, 26, 28, 29, 30, 31}
SMRD_N = {0: 1, 1: 2, 2: 4, 3: 8, 4: 16, 8: 1, 9: 2, 10: 4, 11: 8, 12: 16}


def walk(code: bytes) -> list:
    n = len(code) // 4
    w = struct.unpack_from(f"<{n}I", code, 0)
    i, out = 0, []
    while i < n:
        x = w[i]
        top = x >> 26
        if (x >> 23) == 0x17F:                      # SOPP
            if ((x >> 16) & 0x7F) == 1:             # s_endpgm
                break
            i += 1
            continue
        if (x >> 27) == 0x18:                       # SMRD
            op, sdst, sbase = (x >> 22) & 0x1F, (x >> 15) & 0x7F, ((x >> 9) & 0x3F) * 2
            imm, off = (x >> 8) & 1, x & 0xFF
            lit = None
            if not imm and off == 0xFF and i + 1 < n:
                lit = w[i + 1]
                i += 1
            out.append(("smrd", op, sdst, sbase, imm, off if imm else lit if lit is not None else ("sgpr", off)))
            i += 1
            continue
        if (x >> 30) == 2 or (x >> 28) == 0xB:      # SOP2 / SOPK / SOP1 / SOPC
            if (x >> 28) == 0xB and (x >> 23) not in (0x17D, 0x17E, 0x17F):
                i += 1
                continue
            s0, s1 = x & 0xFF, (x >> 8) & 0xFF
            if (x >> 23) == 0x17D:                  # SOP1
                out.append(("sop1", (x >> 8) & 0xFF, (x >> 16) & 0x7F, s0,
                            w[i + 1] if s0 == 0xFF and i + 1 < n else None))
                i += 2 if s0 == 0xFF else 1
                continue
            lit = s0 == 0xFF or s1 == 0xFF
            if (x >> 30) == 2:
                out.append(("sop2", (x >> 23) & 0x7F, (x >> 16) & 0x7F, s0, s1, None))
            i += 2 if lit else 1
            continue
        if (x >> 31) == 0:                          # VOP2
            i += 2 if (x & 0x1FF) == 0xFF else 1
            continue
        if (x >> 25) in (0x3F, 0x3E):               # VOP1 / VOPC
            i += 2 if (x & 0x1FF) == 0xFF else 1
            continue
        if top == 0x32:                             # VINTRP
            i += 1
            continue
        if top in (0x34, 0x36, 0x37, 0x3C, 0x3E):   # VOP3, DS, FLAT, MIMG, EXP
            i += 2
            continue
        if top in (0x38, 0x3A) and i + 1 < n:       # MUBUF / MTBUF
            y = w[i + 1]
            off, idxen = x & 0xFFF, (x >> 13) & 1
            srsrc = ((y >> 16) & 0x1F) * 4
            if top == 0x38:
                op = (x >> 18) & 0x7F
                size = MUBUF_SIZE.get(op, 8 if op in (49, 81) else 4)
                store = op in MUBUF_STORE or op >= 48
            else:
                op = (x >> 16) & 7
                size, store = 4 * ((op & 3) + 1), op >= 4
            out.append(("mem", op, srsrc, off, idxen, size, store))
            i += 2
            continue
        i += 1
    return out


def accesses(code: bytes) -> dict:
    """flat dword index of a V# -> the buffer instructions that use it. User SGPRs 0..15 are
    flat 0..15; an SMRD from the extended user data at dword k brings in flat 16 + k."""
    src = {i: i for i in range(16)}
    out = defaultdict(list)
    for x in walk(code):
        if x[0] == "smrd":
            for k in range(SMRD_N.get(x[1], 1)):
                src[x[2] + k] = (16 + x[5] + k) if (x[4] == 1 and x[1] < 8) else None
        elif x[0] in ("sop1", "sop2"):
            n = 2 if x[0] == "sop1" and x[1] in (4, 5) else 1
            for k in range(n):
                src[x[2] + k] = src.get(x[3] + k) if (x[0] == "sop1" and x[1] in (3, 4) and x[3] < 104) else None
        elif x[0] == "mem":
            f = src.get(x[2])
            if f is not None:
                out[f].append(x)
    return out


def code_stride(acc: dict, off: int):
    a = [x for x in acc.get(off, []) if x[4]]
    if not a:
        return None
    return (max(x[3] + x[5] for x in a) + 15) // 16 * 16


def written(acc: dict, off: int) -> bool:
    return any(x[6] for x in acc.get(off, []))
