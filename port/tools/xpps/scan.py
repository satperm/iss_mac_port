"""Everything the shader precompiler needs from one package.

The scan follows the relocation tables (knli) instead of guessing pointers: a field is taken
as a pointer only when the link table says the loader relocates it. All offsets below are
relative to the package data (doff).

Shaders     relocation type 4 at H+8 whose target is code that follows the start rule and ends
            in an OrbShdr whose length matches H's m_shaderSize -> Gnm header at H (gnm.py).
Pass record 0xB8 bytes at B (arrays of records, stride 0xB8). The ONE base used everywhere:
                +0x00 u64 class hash   +0x20 object start (CID entry, when the package has CID)
                +0x28 u64 pass id      +0x30 fetch shader (GPU pointer, relocation type 3)
                +0x38 -> VS header     +0x40 -> PS header (both relocated CPU pointers)
                +0x48 -> IRO of the VS +0x50 -> IRO of the PS (0x138 bytes, u16 words)
                +0x58 (ptr, n) bytes   +0x68, +0x78, +0x88, +0x98 (ptr, n) {stage, slot} pairs
                +0xA8 (ptr, n) u32 ids
            Accepted when +0x38 is relocated to a VS header and +0x40 is relocated to a PS header
            (`vs_only` counts records whose +0x40 is null).
Fetch       code at +0x30 up to and including the s_setpc_b64.
Effect      E with +0x80 relocated to the first record of an array and +0x88 = n records, all
            of them accepted. Fields: +0x2C number of material textures, +0x68 slot map (24 B),
            +0x90/+0xA0 (ptr, n) 8 B, +0xB0 (ptr, n) 16 B, +0xC0 samplers (0x20 B),
            +0xD0 texture descriptors (0x10 B), +0xE0 material index -> texture (1 B),
            +0xF0 material samplers (1 B).
Material    M with +0x28 relocated (block: V# of constant buffer 13 in the first 16 bytes, T#s
instance    from +0x20), +0x30 (relocated, or null when the count is 0) texture list with
            +0x38 = n entries of 0x30 bytes (name hash in the first 8), +0x38 not a pointer,
            n <= 64; +0x18 u64 name hash. Three kinds, by the effect slot +0x20:
              local     relocated to an accepted effect of the package, n == effect +0x2C
              import    null and listed in XLNK (resolved later through the exports)
              unlinked  null, not imported (linked by the game at run time); only taken when a
                        mesh points to it
Texture     X with relocation type 4 at X+0x18 (T# base) that is not a shader header, a T# of an
            image type (dword3 >> 28 in 8..15) and data format != 0; name hash at X+0x38, format
            word at X+0x40.
Mesh        M with +0x50 relocated and pointing to M+0x70, u32 n at +0x60 in 1..16, n V# of
            16 bytes at M+0x70 each with a type-2 relocation; +0x00 = material instance
            (relocated to an accepted one, or an import slot).
"""
from __future__ import annotations

import struct
from collections import Counter
from dataclasses import dataclass, field

from . import gnm, kcap, knli

REC_SIZE = 0xB8
REC_CLASS, REC_OBJECT, REC_PASS_ID, REC_FETCH, REC_VS, REC_PS = 0x00, 0x20, 0x28, 0x30, 0x38, 0x40
REC_IRO_VS, REC_IRO_PS = 0x48, 0x50
REC_LISTS = ((0x58, 1), (0x68, 2), (0x78, 2), (0x88, 2), (0x98, 2), (0xA8, 4))
IRO_SIZE = 0x138
MAX_LIST = 4096

EFF_NMAT_TEX, EFF_SLOT_MAP, EFF_PASSES = 0x2C, 0x68, 0x80
EFF_SLOT_MAP_SIZE = 24
EFF_LISTS = ((0x90, 8), (0xA0, 8), (0xB0, 16), (0xC0, 0x20), (0xD0, 0x10), (0xE0, 1), (0xF0, 1))
EFF_OBJECT = 0x10
MAX_PASSES = 255

MI_NAME, MI_EFFECT, MI_BLOCK, MI_TEX, MI_NTEX = 0x18, 0x20, 0x28, 0x30, 0x38
MI_TEX_ENTRY = 0x30
MI_OBJECT = 0x10
MAX_MAT_TEX = 64
CB_VSHARP = 16

TEX_TSHARP, TEX_HASH, TEX_FMT = 0x18, 0x38, 0x40
TSHARP_SIZE = 32
TEX_OBJECT = 0x10

MESH_MATINST, MESH_VS_PTR, MESH_NVS, MESH_VSHARPS = 0x00, 0x50, 0x60, 0x70
VSHARP_SIZE = 16
MAX_VSHARPS = 16


@dataclass
class PackScan:
    path: str
    shaders: list = field(default_factory=list)    # (hdr_off, code_off, size, stage, hash, footer_off, hdr_size, common, regs, slots, sem)
    passes: list = field(default_factory=list)     # see scan_records
    effects: list = field(default_factory=list)
    matinst: list = field(default_factory=list)
    textures: list = field(default_factory=list)
    meshes: list = field(default_factory=list)
    imports: list = field(default_factory=list)    # (local, hash16)
    exports: list = field(default_factory=list)    # (hash16, off)
    stats: Counter = field(default_factory=Counter)
    kcap: dict = field(default_factory=dict)


def _list(p: kcap.Pack, relocs: set, at: int, elem: int):
    """(offset, count, bytes) of a (ptr, n) pair at `at`; ptr must be relocated (or null with n = 0)."""
    ptr, n = p.u64(at), p.u64(at + 8)
    if at in relocs and n <= MAX_LIST and p.valid(ptr, elem * n):
        return ptr, n, p.read(ptr, elem * n)
    if ptr == 0 and at not in relocs:
        return None, n if n <= MAX_LIST else 0, b""
    return None, None, b""


def scan_shaders(p: kcap.Pack, lt: knli.LinkTable, out: PackScan) -> dict:
    """Gnm headers reached through type-4 relocations; returns {hdr_off: (stage, hash, code_off)}."""
    hdrs = {}
    mm, d = p.mm, p.doff
    for r in sorted(lt.positions(knli.RELOC_GPU_ADDR)):
        h = r - 8
        if h < 0 or not p.valid(r, 8):
            continue
        code = p.u64(r)
        if not p.valid(code, 8):
            continue
        fpos = gnm.footer_from_start(mm, d + code)
        if fpos is None or fpos + gnm.FOOTER_SIZE > d + p.size:
            continue
        ft = gnm.parse_footer(mm, fpos)
        g = gnm.parse_header(mm, d + h, ft.type)
        if g is None:
            out.stats["hdr_unknown_stage"] += 1
            continue
        size = fpos + gnm.FOOTER_SIZE - (d + code)
        if g.shader_size != size:
            out.stats["hdr_size_mismatch"] += 1
            continue
        hdrs[h] = (ft.type, ft.hash, code)
        out.shaders.append((h, code, size, ft.stage, ft.hash, fpos - d, g.size, g.common,
                            g.regs, g.slots, g.semantics))
        out.stats["shader_" + ft.stage] += 1
    return hdrs


def scan_records(p: kcap.Pack, lt: knli.LinkTable, hdrs: dict, out: PackScan) -> dict:
    """Pass records; returns {B: (vs_hash, ps_hash)}."""
    r0, r3 = lt.positions(knli.RELOC_CPU), lt.positions(knli.RELOC_GPU_PTR)
    recs = {}
    for s in sorted(r0):
        v = p.u64(s)
        if hdrs.get(v, (None,))[0] != gnm.STAGE_VS:
            continue
        b = s - REC_VS
        if b < 0 or not p.valid(b, REC_SIZE):
            out.stats["rec_cut"] += 1
            continue
        ps = p.u64(b + REC_PS)
        if (b + REC_PS) not in r0:
            out.stats["rec_vs_only" if ps == 0 else "rec_ps_not_relocated"] += 1
            continue
        if hdrs.get(ps, (None,))[0] != gnm.STAGE_PS:
            out.stats["rec_ps_not_header"] += 1
            continue
        fetch = p.u64(b + REC_FETCH) if (b + REC_FETCH) in r3 else None
        fsize = gnm.fetch_size(p.mm, p.doff + fetch) if fetch is not None and p.valid(fetch, 4) else None
        if fetch is None:
            out.stats["rec_no_fetch"] += 1
        elif fsize is None:
            out.stats["rec_fetch_no_setpc"] += 1
        iro = []
        for at in (REC_IRO_VS, REC_IRO_PS):
            ptr = p.u64(b + at)
            iro.append(p.read(ptr, IRO_SIZE) if (b + at) in r0 and p.valid(ptr, IRO_SIZE) else None)
        lists = []
        for at, elem in REC_LISTS:
            _, n, raw = _list(p, r0, b + at, elem)
            if n is None:
                out.stats["rec_bad_list_%02x" % at] += 1
            lists.append(raw)
        cls = p.u64(b + REC_CLASS)
        cid = lt.cid.get(b + REC_OBJECT)
        out.stats["rec_cid" if cid is not None else "rec_no_cid"] += 1
        recs[b] = (hdrs[v][1], hdrs[ps][1])
        out.passes.append((b, cls, cid, p.u64(b + REC_PASS_ID), hdrs[v][1], hdrs[ps][1], v, ps,
                           fetch, fsize, iro[0], iro[1], *lists))
    out.stats["records"] += len(recs)
    return recs


def scan_effects(p: kcap.Pack, lt: knli.LinkTable, recs: dict, out: PackScan) -> dict:
    """Effects; returns {E: number of material textures}."""
    r0 = lt.positions(knli.RELOC_CPU)
    effects = {}
    for s in sorted(r0):
        first = p.u64(s)
        if first not in recs:
            continue
        e = s - EFF_PASSES
        if e < 0 or not p.valid(e, 0x100):
            continue
        n = p.u64(e + EFF_PASSES + 8)
        if not 0 < n <= MAX_PASSES:
            out.stats["eff_bad_count"] += 1
            continue
        if any(first + REC_SIZE * i not in recs for i in range(n)):
            out.stats["eff_bad_array"] += 1
            continue
        lists = []
        bad = False
        for at, elem in EFF_LISTS:
            _, cnt, raw = _list(p, r0, e + at, elem)
            if cnt is None:
                bad = True
            lists.append(raw)
        if bad:
            out.stats["eff_bad_list"] += 1
        nmat = p.u32(e + EFF_NMAT_TEX)
        effects[e] = nmat
        out.stats["eff_cid" if (e + EFF_OBJECT) in lt.cid else "eff_no_cid"] += 1
        out.effects.append((e, lt.cid.get(e + EFF_OBJECT), first, n, nmat,
                            p.read(e + EFF_SLOT_MAP, EFF_SLOT_MAP_SIZE), *lists))
    out.stats["effects"] += len(effects)
    return effects


def _matinst_body(p: kcap.Pack, r0: set, m: int):
    """(n, texture list offset) when the block/texture-list part of a material instance is valid."""
    if m < 0 or not p.valid(m, MI_NTEX + 8):
        return None
    if (m + MI_BLOCK) not in r0 or (m + MI_NTEX) in r0:
        return None
    n, tex = p.u64(m + MI_NTEX), p.u64(m + MI_TEX)
    if n > MAX_MAT_TEX:
        return None
    if (m + MI_TEX) in r0:
        if not p.valid(tex, MI_TEX_ENTRY * n):
            return None
    elif not (tex == 0 and n == 0):
        return None
    return n, tex


def _add_matinst(p: kcap.Pack, lt: knli.LinkTable, out: PackScan, m: int, kind: str, eff, imp, n, tex):
    raw = p.read(tex, MI_TEX_ENTRY * n) if n else b""
    hashes = b"".join(raw[i:i + 8] for i in range(0, len(raw), MI_TEX_ENTRY))
    block = p.u64(m + MI_BLOCK)
    out.stats["mi_" + kind] += 1
    out.stats["mi_cid" if (m + MI_OBJECT) in lt.cid else "mi_no_cid"] += 1
    out.matinst.append((m, kind, lt.cid.get(m + MI_OBJECT), p.u64(m + MI_NAME), eff, imp, block,
                        p.read(block, CB_VSHARP), n, hashes, raw))


def scan_matinst(p: kcap.Pack, lt: knli.LinkTable, effects: dict, out: PackScan) -> dict:
    """Material instances with a local or imported effect; returns {M: kind}."""
    r0 = lt.positions(knli.RELOC_CPU)
    cand = [(s - MI_EFFECT, p.u64(s), None) for s in r0 if p.u64(s) in effects]
    cand += [(local - MI_EFFECT, None, h) for local, h in lt.imports.items()]
    mats = {}
    for m, eff, imp in sorted(cand, key=lambda c: (c[0], c[2] is not None)):
        if m in mats:
            continue
        body = _matinst_body(p, r0, m)
        if body is None:
            continue
        n, tex = body
        if eff is not None and effects[eff] != n:
            out.stats["mi_local_count_mismatch"] += 1
            continue
        if imp is not None and p.u64(m + MI_EFFECT) != 0:
            continue
        kind = "local" if imp is None else "import"
        mats[m] = kind
        _add_matinst(p, lt, out, m, kind, eff, imp, n, tex)
    return mats


def add_unlinked_matinst(p: kcap.Pack, lt: knli.LinkTable, mats: dict, targets: set, out: PackScan) -> None:
    """Material instances whose effect slot is null and not imported (the game links them at run
    time). Only taken when a mesh points to them."""
    r0 = lt.positions(knli.RELOC_CPU)
    for m in sorted(targets - set(mats)):
        body = _matinst_body(p, r0, m)
        if body is None or (m + MI_EFFECT) in r0 or (m + MI_EFFECT) in lt.imports or p.u64(m + MI_EFFECT):
            continue
        mats[m] = "unlinked"
        _add_matinst(p, lt, out, m, "unlinked", None, None, *body)


def scan_textures(p: kcap.Pack, lt: knli.LinkTable, hdrs: dict, out: PackScan) -> None:
    for r in sorted(lt.positions(knli.RELOC_GPU_ADDR)):
        if (r - 8) in hdrs:
            continue
        x = r - TEX_TSHARP
        if x < 0 or not p.valid(x, TEX_FMT + 4):
            out.stats["tex_cut"] += 1
            continue
        t = p.read(x + TEX_TSHARP, TSHARP_SIZE)
        w1, w3 = struct.unpack_from("<I", t, 4)[0], struct.unpack_from("<I", t, 12)[0]
        if not 8 <= w3 >> 28 <= 15 or (w1 >> 20) & 0x3F == 0:
            out.stats["tex_not_image"] += 1
            continue
        out.stats["tex_cid" if (x + TEX_OBJECT) in lt.cid else "tex_no_cid"] += 1
        out.textures.append((x, lt.cid.get(x + TEX_OBJECT), p.u64(x + TEX_HASH), t, p.u32(x + TEX_FMT)))
    out.stats["textures"] += len(out.textures)


def mesh_candidates(p: kcap.Pack, lt: knli.LinkTable, out: PackScan) -> list:
    """Meshes (before the material check): [(M, n)]."""
    r0, r2 = lt.positions(knli.RELOC_CPU), lt.positions(knli.RELOC_VSHARP)
    found = []
    for s in sorted(r0):
        m = s - MESH_VS_PTR
        if m < 0 or p.u64(s) != m + MESH_VSHARPS or not p.valid(m, MESH_VSHARPS):
            continue
        n = p.u32(m + MESH_NVS)
        if not 1 <= n <= MAX_VSHARPS or not p.valid(m + MESH_VSHARPS, VSHARP_SIZE * n):
            out.stats["mesh_bad_count"] += 1
            continue
        if any(m + MESH_VSHARPS + VSHARP_SIZE * j not in r2 for j in range(n)):
            out.stats["mesh_vsharp_not_relocated"] += 1
            continue
        found.append((m, n))
    return found


def scan_meshes(p: kcap.Pack, lt: knli.LinkTable, cands: list, mats: dict, out: PackScan) -> None:
    r0 = lt.positions(knli.RELOC_CPU)
    for m, n in cands:
        mi = p.u64(m + MESH_MATINST)
        if (m + MESH_MATINST) in r0:
            if mi not in mats:
                out.stats["mesh_matinst_unknown"] += 1
                continue
            ref = (mi, None)
        elif m + MESH_MATINST in lt.imports:
            ref = (None, lt.imports[m + MESH_MATINST])
        else:
            out.stats["mesh_no_matinst"] += 1
            continue
        out.stats["mesh_matinst_" + ("import" if ref[1] is not None else mats[ref[0]])] += 1
        out.meshes.append((m, ref[0], ref[1], n, p.read(m + MESH_VSHARPS, VSHARP_SIZE * n)))
    out.stats["meshes"] += len(out.meshes)


def scan_pack(path: str, rel: str = None) -> PackScan:
    out = PackScan(rel or path)
    try:
        p = kcap.Pack(path)
    except (kcap.KcapError, OSError) as exc:
        out.stats["not_kcap"] += 1
        out.kcap = {"error": str(exc)}
        return out
    with p:
        k = p.k
        out.kcap = {"version": k.version, "doff": k.doff, "dsize": k.dsize, "items": len(k.items),
                    "sections": len(k.sections), "bss_items": len(k.bss_items()),
                    "bss_bytes": k.bss_bytes(), "truncated": p.file_size < k.doff + k.dsize}
        lt = knli.read_links(p)
        out.stats.update({"link_" + key: v for key, v in lt.stats.items()})
        out.stats["link_tables"] += lt.tables
        for t, s in lt.relocs.items():
            out.stats["reloc_%d" % t] += len(s)
        out.stats["cid"] += len(lt.cid)
        out.imports = sorted(lt.imports.items())
        out.exports = knli.read_exports(p)
        hdrs = scan_shaders(p, lt, out)
        recs = scan_records(p, lt, hdrs, out)
        effects = scan_effects(p, lt, recs, out)
        out.stats["eff_at_type_b_item"] += sum(1 for it in k.items if it.type == 0xB and (it.off + 0x10) in effects)
        out.stats["type_b_items"] += sum(1 for it in k.items if it.type == 0xB)
        mats = scan_matinst(p, lt, effects, out)
        cands = mesh_candidates(p, lt, out)
        r0 = lt.positions(knli.RELOC_CPU)
        targets = {p.u64(m + MESH_MATINST) for m, _ in cands if (m + MESH_MATINST) in r0}
        add_unlinked_matinst(p, lt, mats, targets, out)
        out.matinst.sort(key=lambda r: r[0])
        scan_textures(p, lt, hdrs, out)
        scan_meshes(p, lt, cands, mats, out)
    return out


def scan_footers(path: str) -> Counter:
    """Independent check: every OrbShdr of the file by signature and the start rule."""
    c = Counter()
    with kcap.Pack(path) as p:
        mm, d, end = p.mm, p.doff, p.doff + p.size
        i = mm.find(gnm.SIGNATURE, d, end)
        while i >= 0:
            c["footers"] += 1
            if i + gnm.FOOTER_SIZE > end:
                c["footer_cut"] += 1
            else:
                n = len(gnm.code_starts(mm, i, d))
                c["start_%s" % ("one" if n == 1 else "none" if n == 0 else "many")] += 1
            i = mm.find(gnm.SIGNATURE, i + 1, end)
    return c
