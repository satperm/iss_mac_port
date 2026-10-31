"""tools/xpps: KCAP reader, link tables, Gnm headers, scan and SQLite index.

Everything here runs on SYNTHETIC packages built by the test itself (no game data). The last
class checks invariants on the real packages and is skipped when the game folder is missing.
"""
import contextlib
import io
import os
import sqlite3
import struct
import sys
import tempfile
import unittest
from pathlib import Path

from paths import ROOT
sys.path.insert(0, str(ROOT / 'tools'))
from xpps import gnm, index, kcap, knli, scan  # noqa: E402

DOFF = 0x200
VS_HASH, PS_HASH, CS_HASH = 0x1111222233334444, 0xF000000000000001, 0x5555666677778888
EXPORT_HASH = bytes(range(0x10, 0x20))
SETPC = 0xBE80201E          # s_setpc_b64 s[30:31]


def code(size):
    """GCN code that follows the start rule: s_mov_b32 vcc_hi, size/8 - 1."""
    return struct.pack('<II', gnm.CODE_MARKER, size // 8 - 1) + bytes(size - 8)


def footer(length_field, shader_hash, stage):
    flags = (length_field << gnm.LENGTH_SHIFT) | (stage << gnm.TYPE_SHIFT)
    return gnm.SIGNATURE + bytes([7]) + struct.pack('<II', flags, 0) + struct.pack('<QI', shader_hash, 0)


class PackBuilder:
    """Writes a KCAP package: data with structures + one KNLI link table item."""

    def __init__(self, size=0x22000):
        self.data = bytearray(size)
        self.relocs = {0: [], 2: [], 3: [], 4: []}
        self.cid = []
        self.imports = []
        self.items = []            # (off, size, flags)

    def put(self, off, raw):
        self.data[off:off + len(raw)] = raw

    def u32(self, off, v):
        struct.pack_into('<I', self.data, off, v)

    def u64(self, off, v):
        struct.pack_into('<Q', self.data, off, v)

    def ptr(self, at, target, rtype=0):
        self.u64(at, target)
        self.relocs[rtype].append(at)

    def item(self, off, size, itype, align=0x10):
        self.items.append((off, size, (itype << 16) | align))

    # --- link table -------------------------------------------------------------------
    @staticmethod
    def encode_positions(positions):
        out, base = [], None
        for p in sorted(positions):
            b = p >> 17
            if b != base:
                out.append(-0x8000 + 0x4000 + b)     # negative with 0x4000: new base
                base = b
            out.append((p - (b << 17)) // 4)
        return out

    def link_table(self, split=False):
        """One chunk chain (KNLI, KNLX, CID, END) or, with split, two: KNLI + END and
        KNLX + CID + END (the layout of the world-block packages)."""
        streams = [(t, self.encode_positions(v)) for t, v in sorted(self.relocs.items()) if v]
        ilnk = struct.pack('<i', len(streams))
        ilnk += b''.join(struct.pack('<ii', t, len(s)) for t, s in streams)
        ilnk += b''.join(struct.pack('<%dh' % len(s), *s) for _, s in streams)
        ilnk += bytes(-len(ilnk) % 4)
        out = b'KNLI' + struct.pack('<I', len(ilnk)) + ilnk
        if split:
            first = out + b' DNE' + struct.pack('<I', 0)
            out = b''
        if self.imports:
            x = struct.pack('<3Q', 0, 16, len(self.imports))
            x += b''.join(h + struct.pack('<Q', local) for local, h in self.imports)
            out += b'KNLX' + struct.pack('<I', len(x)) + x
        if self.cid:
            c = struct.pack('<Q', len(self.cid)) + b''.join(struct.pack('<QQ', l, k) for l, k in self.cid)
            out += b' DIC' + struct.pack('<I', len(c)) + c
        out += b' DNE' + struct.pack('<I', 0)
        return [first, out] if split else [out]

    def build(self, bss_item=True, split=False):
        tables = self.link_table(split)
        link_off = len(self.data)
        data = bytes(self.data)
        items = list(self.items)
        for t in tables:
            items.append((len(data), len(t), (2 << 16) | 8))
            data += t
        table = tables[0]
        if bss_item:   # runs past the data: BSS
            items.insert(0, (link_off, len(table) + 0x1000, (9 << 16) | 0x10))
        head = bytearray(0x98)
        head[0:4] = b'KCAP'
        struct.pack_into('<HH', head, 4, 13, 0)
        tbl = struct.pack('<6Q', 0, 1, 0, len(items), 0, 0)
        tbl += struct.pack('<IIIIQQQ', 0x1, len(data), 0, 0, 0, 0, len(items))
        tbl += b''.join(struct.pack('<III', sz, off, fl) for off, sz, fl in items)
        struct.pack_into('<II', head, 0x18, 0x98, len(tbl))
        doff = DOFF
        assert 0x98 + len(tbl) <= doff
        struct.pack_into('<II', head, 0x28, doff, len(data))
        out = bytes(head) + tbl
        return out + bytes(doff - len(out)) + data


# Layout of the synthetic package (offsets relative to the data).
G = 0x1000                 # GPU item: shader code and fetch shader
C = 0x2000                 # CPU item: Gnm headers, records, effect, materials, meshes
VS_H, PS_H, CS_H = C + 0x000, C + 0x100, C + 0x200
IRO_VS, IRO_PS = C + 0x300, C + 0x438
EFF = C + 0x600
RECS = C + 0x800
MI_LOCAL, MI_IMPORT, MI_UNLINKED = C + 0xA00, C + 0xC00, C + 0xD00
MESH1, MESH2, MESH3 = C + 0xE00, C + 0xF00, C + 0x1000
TEX = 0x20010              # past 128 KiB: its relocation needs a second ILNK base


def shader(b, at, size, stage, h):
    b.put(at, code(size))
    b.put(at + size, footer(size - 4, h, stage))   # the length field is not the code size


def gnm_header(b, at, stage, code_at, size, nslots, extra):
    b.u32(at, (size + gnm.FOOTER_SIZE) | (nslots << 24))
    b.ptr(at + 8, code_at, 4)
    so = gnm.SLOTS_OFFSET[stage]
    for i in range(nslots):
        b.put(at + so + 4 * i, bytes([i, i + 1, 2 * i, 0]))
    extra(at, at + so + 4 * nslots)


def make_pack(with_import=True, with_export=False, name_seed=0, split=False):
    b = PackBuilder()
    b.item(G, 0x1000, 0x11, 0x100)
    b.item(C, 0x1200, 0x1)
    b.item(EFF - 0x10, 0x200, 0xB)
    shader(b, G + 0x000, 0x40, gnm.STAGE_VS, VS_HASH)
    shader(b, G + 0x100, 0x30, gnm.STAGE_PS, PS_HASH)
    shader(b, G + 0x200, 0x20, gnm.STAGE_CS, CS_HASH)
    b.put(G + 0x300, struct.pack('<4I', 0xE00C2000, 0x80000100, 0xBF8C0F70, SETPC))   # fetch: 16 bytes

    def vs_extra(at, sem):
        b.put(at + 0x24, bytes([2, 1, 0, 0]))                        # 2 inputs, 1 export
        b.put(sem, bytes([0, 4, 3, 0, 1, 8, 2, 0]) + bytes([0x0F, 0x10]))

    def ps_extra(at, sem):
        b.u32(at + 0x1C, 0x4)                                        # SPI_SHADER_COL_FORMAT
        b.put(at + 0x38, bytes([1]))
        b.put(sem, bytes([0x0F, 0x00]))

    gnm_header(b, VS_H, gnm.STAGE_VS, G + 0x000, 0x40, 2, vs_extra)
    gnm_header(b, PS_H, gnm.STAGE_PS, G + 0x100, 0x30, 1, ps_extra)
    gnm_header(b, CS_H, gnm.STAGE_CS, G + 0x200, 0x20, 1, lambda at, sem: None)
    b.put(IRO_VS, bytes([0xFF]) * scan.IRO_SIZE)
    b.put(IRO_PS, bytes([0xEE]) * scan.IRO_SIZE)
    # Effect with two pass records.
    b.u32(EFF + scan.EFF_NMAT_TEX, 2)
    b.put(EFF + scan.EFF_SLOT_MAP, bytes([0xFF, 0, 1]) + bytes([0xFF]) * 21)
    b.ptr(EFF + scan.EFF_PASSES, RECS)
    b.u64(EFF + scan.EFF_PASSES + 8, 2)
    lists = C + 0x700
    for k, (at, elem) in enumerate(scan.EFF_LISTS):
        b.ptr(EFF + at, lists + 0x10 * k)
        b.u64(EFF + at + 8, 1)
        b.put(lists + 0x10 * k, bytes([0x40 + k]) * elem)
    b.cid.append((EFF + scan.EFF_OBJECT, 0xE0E0))
    small = C + 0x980
    for i in range(2):
        r = RECS + scan.REC_SIZE * i
        b.u64(r + scan.REC_CLASS, 0xC1A55 + i)
        b.u64(r + scan.REC_PASS_ID, 16 * i)
        b.ptr(r + scan.REC_FETCH, G + 0x300, 3)
        b.ptr(r + scan.REC_VS, VS_H)
        b.ptr(r + scan.REC_PS, PS_H)
        b.ptr(r + scan.REC_IRO_VS, IRO_VS)
        b.ptr(r + scan.REC_IRO_PS, IRO_PS)
        for k, (at, elem) in enumerate(scan.REC_LISTS):
            b.ptr(r + at, small + 0x10 * k)
            b.u64(r + at + 8, 2)
            b.put(small + 0x10 * k, bytes([k, i]) * elem)
        b.cid.append((r + scan.REC_OBJECT, 0xBA55))
    # A VS pointer without a PS: not a record.
    b.ptr(C + 0x9F0, VS_H)

    def matinst(m, effect_at=None):
        b.u64(m + scan.MI_NAME, 0xAB00 + m)
        if effect_at is not None:
            b.ptr(m + scan.MI_EFFECT, effect_at)
        b.ptr(m + scan.MI_BLOCK, m + 0xB0)
        b.put(m + 0xB0, struct.pack('<4I', 0x1000, 0x00100000, 0x40, 0x20077FAC))   # CB V#
        b.ptr(m + scan.MI_TEX, m + 0x40)
        b.u64(m + scan.MI_NTEX, 2)
        for j in range(2):
            b.u64(m + 0x40 + scan.MI_TEX_ENTRY * j, 0x7E0 + j + name_seed)
        b.cid.append((m + scan.MI_OBJECT, 0x3A7))

    matinst(MI_LOCAL, EFF)
    if with_import:
        matinst(MI_IMPORT)
        b.imports.append((MI_IMPORT + scan.MI_EFFECT, EXPORT_HASH))
    matinst(MI_UNLINKED)

    def mesh(m, mi, n=2):
        if mi is not None:
            b.ptr(m + scan.MESH_MATINST, mi)
        b.ptr(m + scan.MESH_VS_PTR, m + scan.MESH_VSHARPS)
        b.u32(m + scan.MESH_NVS, n)
        for j in range(n):
            v = m + scan.MESH_VSHARPS + scan.VSHARP_SIZE * j
            b.put(v, struct.pack('<4I', 0x100 * j, 0x000C0000, 0x10, 0x00067FAC))
            b.relocs[2].append(v)

    mesh(MESH1, MI_LOCAL)
    mesh(MESH2, MI_UNLINKED, 1)
    mesh(MESH3, None)                            # no material: rejected
    # Texture object (T# base relocated with type 4) past 128 KiB, plus a non-image decoy.
    b.item(TEX - 0x10, 0x100, 0x1)
    b.ptr(TEX + scan.TEX_TSHARP, 0x30000, 4)
    b.put(TEX + scan.TEX_TSHARP + 4, struct.pack('<I', 10 << 20))      # data format 8_8_8_8
    b.put(TEX + scan.TEX_TSHARP + 12, struct.pack('<I', 9 << 28))      # 2D
    b.u64(TEX + scan.TEX_HASH, 0x7E0 + name_seed)
    b.u32(TEX + scan.TEX_FMT, 0x00100A00)
    b.cid.append((TEX + scan.TEX_OBJECT, 0x7E7))
    b.ptr(TEX + 0x80, 0x30000, 4)                # relocated but not a T#
    if with_export:
        ex = C + 0x1100
        b.item(ex, 0x60, 0xD)
        b.u64(ex, ex + 0x10)
        b.u64(ex + 8, 2)                          # two buckets, one empty
        b.u64(ex + 0x10, 0)
        b.u64(ex + 0x18, 0)
        b.u64(ex + 0x20, ex + 0x30)
        b.u64(ex + 0x28, 1)
        b.put(ex + 0x30, struct.pack('<I', 0x5EED) + EXPORT_HASH + struct.pack('<III', 0, EFF, 0))
    return b.build(split=split)


class KcapTest(unittest.TestCase):
    def test_header_items_and_bss(self):
        raw = make_pack()
        k = kcap.parse(raw)
        self.assertEqual((k.version, k.doff, k.dsize), (13, DOFF, len(raw) - DOFF))
        self.assertEqual(len(k.sections), 1)
        self.assertEqual(k.items[1].type, 0x11)
        self.assertEqual(k.items[1].align, 0x100)
        self.assertEqual(len(k.bss_items()), 1)
        self.assertEqual(k.bss_bytes(), 0x1000)
        self.assertEqual(k.item_at(G + 4).index, 1)

    def test_rejects_other_files(self):
        with self.assertRaises(kcap.KcapError):
            kcap.parse(b'NOPE' + bytes(0x200))
        with self.assertRaises(kcap.KcapError):
            kcap.parse(b'KCAP' + bytes(8))


class KnliTest(unittest.TestCase):
    def test_decode_base_and_run(self):
        self.assertEqual(knli.decode_positions([3, -0x8000 + 0x4000 + 2, 1, -0x8000 + 2]),
                         [12, (2 << 17) + 4, (2 << 17) + 8, (2 << 17) + 12])

    def test_tables(self):
        with tempfile.TemporaryDirectory() as d:
            p = Path(d) / 'a.xpps'
            p.write_bytes(make_pack(with_export=True))
            with kcap.Pack(str(p)) as pack:
                lt = knli.read_links(pack)
                self.assertEqual(lt.tables, 1)                     # the BSS item at the same offset is not a second table
                self.assertIn(TEX + scan.TEX_TSHARP, lt.positions(knli.RELOC_GPU_ADDR))
                self.assertIn(VS_H + 8, lt.positions(knli.RELOC_GPU_ADDR))
                self.assertIn(RECS + scan.REC_FETCH, lt.positions(knli.RELOC_GPU_PTR))
                self.assertIn(MESH1 + scan.MESH_VSHARPS, lt.positions(knli.RELOC_VSHARP))
                self.assertEqual(lt.imports, {MI_IMPORT + scan.MI_EFFECT: EXPORT_HASH})
                self.assertEqual(lt.cid[EFF + scan.EFF_OBJECT], 0xE0E0)
                self.assertEqual(knli.read_exports(pack), [(EXPORT_HASH, EFF)])

    def test_split_tables(self):
        """World-block layout: the KNLX and CID chunks live in a second link item."""
        with tempfile.TemporaryDirectory() as d:
            p = Path(d) / 'b.xpps'
            p.write_bytes(make_pack(split=True))
            with kcap.Pack(str(p)) as pack:
                lt = knli.read_links(pack)
                self.assertEqual(lt.tables, 2)
                self.assertEqual(lt.imports, {MI_IMPORT + scan.MI_EFFECT: EXPORT_HASH})
                self.assertEqual(lt.cid[EFF + scan.EFF_OBJECT], 0xE0E0)
                self.assertIn(VS_H + 8, lt.positions(knli.RELOC_GPU_ADDR))


class GnmTest(unittest.TestCase):
    def test_start_rule_and_header(self):
        raw = make_pack()
        buf = raw[DOFF:]
        fpos = gnm.footer_from_start(buf, G)
        self.assertEqual(fpos, G + 0x40)
        f = gnm.parse_footer(buf, fpos)
        self.assertEqual((f.stage, f.hash, f.length), ('VS', VS_HASH, 0x3C))
        self.assertEqual(gnm.code_starts(buf, fpos), [G])
        self.assertIsNone(gnm.footer_from_start(buf, G + 4))
        h = gnm.parse_header(buf, VS_H, gnm.STAGE_VS)
        self.assertEqual((h.shader_size, h.num_slots, h.code_ptr), (0x40 + 28, 2, G))
        self.assertEqual(h.input_usage_slots(), [(0, 1, 0, 0), (1, 2, 2, 0)])
        self.assertEqual(len(h.semantics), 2 * 4 + 1 * 2)
        p = gnm.parse_header(buf, PS_H, gnm.STAGE_PS)
        self.assertEqual((p.num_slots, len(p.semantics), p.size), (1, 2, 0x3C + 4 + 2))
        self.assertEqual(struct.unpack_from('<I', p.regs, 0x1C - gnm.REGS_OFFSET)[0], 4)
        c = gnm.parse_header(buf, CS_H, gnm.STAGE_CS)
        self.assertEqual(c.size, 0x2C + 4)
        self.assertEqual(gnm.fetch_size(buf, G + 0x300), 16)
        self.assertIsNone(gnm.fetch_size(buf, G + 0x400, 0x40))


class ScanTest(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.path = Path(self.tmp.name) / 'a.xpps'
        self.path.write_bytes(make_pack())
        self.r = scan.scan_pack(str(self.path), 'a.xpps')

    def tearDown(self):
        self.tmp.cleanup()

    def test_shaders(self):
        got = sorted((s[3], s[4], s[1], s[2]) for s in self.r.shaders)
        self.assertEqual(got, [('CS', CS_HASH, G + 0x200, 0x20 + 28), ('PS', PS_HASH, G + 0x100, 0x30 + 28),
                               ('VS', VS_HASH, G, 0x40 + 28)])

    def test_records(self):
        self.assertEqual(len(self.r.passes), 2)
        b, cls, cid, pid, vs, ps, vh, ph, fetch, fsize, iv, ip, *lists = self.r.passes[1]
        self.assertEqual((b, cls, cid, pid, vs, ps, vh, ph), (RECS + scan.REC_SIZE, 0xC1A56, 0xBA55, 16,
                                                              VS_HASH, PS_HASH, VS_H, PS_H))
        self.assertEqual((fetch, fsize), (G + 0x300, 16))
        self.assertEqual((iv[:1], ip[:1], len(iv)), (b'\xff', b'\xee', scan.IRO_SIZE))
        self.assertEqual([len(x) for x in lists], [2 * e for _, e in scan.REC_LISTS])
        self.assertEqual(lists[4][:2], bytes([4, 1]))                  # +0x98 pairs of record 1
        self.assertEqual(self.r.stats['rec_vs_only'], 1)

    def test_effect(self):
        (e,) = self.r.effects
        off, cid, first, n, nmat, smap, *lists = e
        self.assertEqual((off, cid, first, n, nmat), (EFF, 0xE0E0, RECS, 2, 2))
        self.assertEqual(smap[:3], bytes([0xFF, 0, 1]))
        self.assertEqual([len(x) for x in lists], [e for _, e in scan.EFF_LISTS])
        self.assertEqual(self.r.stats['eff_at_type_b_item'], 1)

    def test_materials_meshes_textures(self):
        kinds = {m[0]: m[1] for m in self.r.matinst}
        self.assertEqual(kinds, {MI_LOCAL: 'local', MI_IMPORT: 'import', MI_UNLINKED: 'unlinked'})
        local = next(m for m in self.r.matinst if m[0] == MI_LOCAL)
        _, _, cid, name, eff, imp, block, cb, n, hashes, raw = local
        self.assertEqual((cid, name, eff, imp, block, n), (0x3A7, 0xAB00 + MI_LOCAL, EFF, None, MI_LOCAL + 0xB0, 2))
        self.assertEqual(cb[12:16], struct.pack('<I', 0x20077FAC))
        self.assertEqual(hashes, struct.pack('<QQ', 0x7E0, 0x7E1))
        imp = next(m for m in self.r.matinst if m[0] == MI_IMPORT)
        self.assertEqual((imp[4], imp[5]), (None, EXPORT_HASH))
        meshes = {m[0]: (m[1], m[3]) for m in self.r.meshes}
        self.assertEqual(meshes, {MESH1: (MI_LOCAL, 2), MESH2: (MI_UNLINKED, 1)})
        self.assertEqual(self.r.stats['mesh_no_matinst'], 1)
        (t,) = self.r.textures
        self.assertEqual((t[0], t[1], t[2], t[4]), (TEX, 0x7E7, 0x7E0, 0x00100A00))
        self.assertEqual(self.r.stats['tex_not_image'], 1)

    def test_footer_check(self):
        c = scan.scan_footers(str(self.path))
        self.assertEqual((c['footers'], c['start_one']), (3, 3))

    def test_not_a_package(self):
        bad = Path(self.tmp.name) / 'bad.xpps'
        bad.write_bytes(b'junk')
        self.assertEqual(scan.scan_pack(str(bad)).stats['not_kcap'], 1)


class IndexTest(unittest.TestCase):
    def build(self, root, out, **kw):
        buf = io.StringIO()
        with contextlib.redirect_stdout(buf), contextlib.redirect_stderr(io.StringIO()):
            meta = index.build(str(root), str(out), jobs=kw.pop('jobs', 1), **kw)
        return meta, buf.getvalue().splitlines()

    def test_build_resolve_incremental_and_sha(self):
        with tempfile.TemporaryDirectory() as d:
            root = Path(d) / 'cache'
            (root / 'sub').mkdir(parents=True)
            (root / 'a.xpps').write_bytes(make_pack())
            (root / 'sub' / 'core.xpps').write_bytes(make_pack(with_import=False, with_export=True, name_seed=5))
            (root / 'junk.xpps').write_bytes(b'not a package')
            out = Path(d) / 'idx.sqlite'
            meta, lines = self.build(root, out, jobs=2)
            self.assertEqual(lines[0], 'PROGRESS index 0 3')
            self.assertEqual(lines[-1], 'PROGRESS index 3 3')
            self.assertEqual(meta['scanned'], 3)
            db = index.open_index(str(out))
            s = index.summary(db)
            self.assertEqual((s['shaders_unique'], s['shader_occurrences'], s['code_starts']), (3, 6, 6))
            self.assertEqual((s['records'], s['pairs_vs_ps'], s['effects']), (4, 1, 2))
            self.assertEqual((s['matinst_local'], s['matinst_import'], s['matinst_unlinked']), (2, 1, 2))
            self.assertEqual((s['textures'], s['texture_hashes'], s['meshes']), (2, 2, 4))
            self.assertEqual(s['shaders_hdr_variants'], 0)
            row = db.execute("SELECT stage, n_occ, size FROM shaders WHERE hash = ?", (index.s64(PS_HASH),)).fetchone()
            self.assertEqual(row, ('PS', 2, 0x30 + 28))
            core_id = db.execute("SELECT id FROM files WHERE path = 'sub/core.xpps'").fetchone()[0]
            res = db.execute("SELECT effect_file, effect_off FROM matinst WHERE kind = 'import'").fetchone()
            self.assertEqual(res, (core_id, EFF))
            self.assertEqual(s['meshes_with_effect'], 2)
            sha = index.content_sha(db)
            self.assertEqual(sha, meta['content_sha'])
            db.close()
            # Same files again: everything reused, same content.
            meta2, _ = self.build(root, out)
            self.assertEqual((meta2['reused'], meta2['scanned'], meta2['content_sha']), (3, 0, sha))
            # Fresh build in another place: same content hash.
            meta3, _ = self.build(root, Path(d) / 'other.sqlite', full=True)
            self.assertEqual(meta3['content_sha'], sha)
            # A changed and a removed package are noticed.
            (root / 'junk.xpps').unlink()
            (root / 'a.xpps').write_bytes(make_pack(with_import=False))
            meta4, lines4 = self.build(root, out)
            self.assertEqual((meta4['scanned'], meta4['reused'], meta4['removed']), (1, 1, 1))
            self.assertEqual(lines4[-1], 'PROGRESS index 2 2')
            self.assertNotEqual(meta4['content_sha'], sha)
            db = index.open_index(str(out))
            self.assertNotIn('matinst_import', index.summary(db))
            db.close()

    def test_cli_and_errors(self):
        with tempfile.TemporaryDirectory() as d:
            err = io.StringIO()
            with contextlib.redirect_stderr(err):
                self.assertEqual(index.main(['build', str(Path(d) / 'missing'), str(Path(d) / 'x.sqlite')]), 1)
                self.assertEqual(index.main(['stats', str(Path(d) / 'x.sqlite')]), 1)
            self.assertIn('error:', err.getvalue())
            bad = Path(d) / 'bad.sqlite'
            sqlite3.connect(bad).execute('CREATE TABLE t(x)').connection.close()
            with self.assertRaises(index.IndexError_):
                index.open_index(str(bad))


GAME = Path(os.environ.get('ISS_GAME') or ROOT.parent / 'game' / 'CUSA00223') / 'art' / 'cache'


@unittest.skipUnless(GAME.is_dir(), 'game folder not found (set ISS_GAME)')
class RealGameTest(unittest.TestCase):
    """Invariants on the first packages that hold shaders (no game value is written here)."""

    def test_first_packages_with_shaders(self):
        checked = 0
        for rel in kcap.iter_packages(str(GAME)):
            r = scan.scan_pack(str(GAME / rel), rel)
            if not r.passes:
                continue
            footers = scan.scan_footers(str(GAME / rel))
            starts = {s[1] for s in r.shaders}
            self.assertEqual(footers['footers'], footers['start_one'], rel)
            self.assertEqual(footers['footers'], len(starts), rel)
            ps_headers = {s[0] for s in r.shaders if s[3] == 'PS'}
            self.assertTrue(all(p[7] in ps_headers for p in r.passes), rel)
            self.assertTrue(all(p[9] is not None for p in r.passes), rel)
            self.assertEqual(r.stats['mi_local_count_mismatch'], 0, rel)
            checked += 1
            if checked == 3:
                break
        self.assertEqual(checked, 3)


if __name__ == '__main__':
    unittest.main()
