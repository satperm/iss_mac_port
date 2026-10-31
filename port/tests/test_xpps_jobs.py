"""tools/xpps: precompile job files, the signature, the engine rules and the job builder helpers.

SYNTHETIC data only (made-up hashes, headers and sharps): nothing of the game is needed.
"""
import struct
import sys
import tempfile
import unittest
from pathlib import Path

from paths import ROOT
sys.path.insert(0, str(ROOT / 'tools'))
from xpps import engine_tables as ET  # noqa: E402
from xpps import jobfile, jobs as J, signature as S  # noqa: E402

VS_HASH, PS_HASH, CS_HASH = 0x1111222233334444, 0xF000000000000001, 0x5555666677778888


def vs_header(slots, n_exp=2):
    regs = struct.pack('<5I', 0x00AC0041, 0x18, 0x6, 0x4, 0) + bytes([1, n_exp, 0, 0])
    sem = bytes([0x0F, 4, 3, 0]) + struct.pack('<%dH' % n_exp, *[(i << 8) | (0x0F + i) for i in range(n_exp)])
    return S.Header('VS', regs, bytes(b for s in slots for b in s), sem)


def ps_header(slots, inputs=(0x0F, 0x10), colfmt=0x4, dsc=0x10):
    regs = struct.pack('<10I', 0x002C0003, 0x1C, 0, colfmt, 0x2, 0x2, len(inputs), 0, dsc, 0xF)
    return S.Header('PS', regs, bytes(b for s in slots for b in s), struct.pack('<%dH' % len(inputs), *inputs))


def cs_header(slots):
    regs = struct.pack('<5I', 0x002C0041, 0x8C, 64, 1, 1)
    return S.Header('CS', regs, bytes(b for s in slots for b in s), b'')


def tsharp(base=0x1000, dfmt=10, nfmt=0, typ=9):
    return [base, dfmt << 20 | nfmt << 26, 0, 4 | 5 << 3 | 6 << 6 | 7 << 9 | typ << 28, 0, 0, 0, 0]


def vsharp(base=0x2000, stride=16, num=4, dfmt=4, nfmt=7, mtype=4):
    return [base, stride << 16, num, 4 | nfmt << 12 | dfmt << 15 | mtype << 27]


class Base(unittest.TestCase):
    def setUp(self):
        # PS: T# at 4, sampler at 12, CB at 16 (extended user data); VS: CB at 0, buffer at 4
        self.ps = ps_header([(0, 0, 4, 3), (1, 0, 12, 0), (2, 13, 16, 0), (0x1B, 0, 2, 0)])
        self.vs = vs_header([(2, 0, 0, 0), (0, 35, 4, 0), (0x12, 0, 8, 0), (0x17, 0, 10, 0)])
        self.cs = cs_header([(4, 1, 4, 0), (2, 1, 8, 0), (28, 0, 0, 0)])
        self.headers = {VS_HASH: self.vs, PS_HASH: self.ps, CS_HASH: self.cs}

    def stage(self, sw, hw, h, flat, fetch=b'', vsharps=()):
        return {'sw': sw, 'hw': hw, 'hash': h, 'code': b'\x00' * 8, 'ud': flat[:16], 'flat': flat,
                'fetch': fetch, 'fetch_sgpr': 8 if fetch else 0, 'vsharps': list(vsharps)}

    def graphics(self, tex_base=0x1000, cb_base=0x3000, stride=64):
        ps_flat = [0] * 20
        ps_flat[4:12] = tsharp(tex_base)
        ps_flat[16:20] = [cb_base, 16 << 16, 8, ET.CB_DWORD3]
        vs_flat = [0] * 16
        vs_flat[0:4] = [cb_base, 16 << 16, 8, ET.CB_DWORD3]
        vs_flat[4:8] = vsharp(stride=stride)
        fetch = struct.pack('<I', 0xC0800000 | 1 << 8 | 0x5 << 9 | 0x00 | J.S_LOAD_DWORDX4 << 22) + struct.pack('<I', 0xBE802000)
        regs = J.header_regs(self.vs, self.ps)
        st = {'off': False, 'cb': {0: [10, 0, 1, 0, 15, 0]}, 'depth': [3, 0, 0, 1],
              'raster': [4, 2, 0, 1, 1, 0xCC], 'vs': [0, 1, 0, 0, 0, 0]}
        regs.update(J.state_regs(st))
        return {'kind': 0, 'regs': regs, 'stages': [
            self.stage(0, 0, PS_HASH, ps_flat),
            self.stage(3, 1, VS_HASH, vs_flat, fetch, [(10, 0, tuple(vsharp(dfmt=13)))])]}


class JobFile(Base):
    def test_round_trip(self):
        recs = [self.graphics(), {'kind': 1, 'regs': {7: 64, 18: 1}, 'stages': [
            self.stage(5, 6, CS_HASH, [0] * 16)]}]
        with tempfile.TemporaryDirectory() as d:
            p = Path(d) / 'x.jobs'
            self.assertEqual(jobfile.write(p, recs), 2)
            back = jobfile.read(p)
        self.assertEqual(back[0]['regs'], {k: v for k, v in recs[0]['regs'].items() if v})
        self.assertEqual(back[0]['stages'][1]['vsharps'], recs[0]['stages'][1]['vsharps'])
        self.assertEqual(back[1]['stages'][0]['hash'], CS_HASH)

    def test_bad_magic(self):
        with self.assertRaises(ValueError):
            jobfile.parse(b'NOTJOBS!' + bytes(4))


class Signature(Base):
    def test_addresses_do_not_matter(self):
        a = S.signature(self.graphics(tex_base=0x1000, cb_base=0x3000), self.headers)
        b = S.signature(self.graphics(tex_base=0x9000, cb_base=0x7000), self.headers)
        self.assertEqual(S.freeze(a), S.freeze(b))

    def test_stride_matters(self):
        a = S.signature(self.graphics(stride=64), self.headers)
        b = S.signature(self.graphics(stride=48), self.headers)
        self.assertNotEqual(a['vs_res'], b['vs_res'])
        self.assertEqual(a['ps_res'], b['ps_res'])

    def test_unbound_texture(self):
        sig = S.signature(self.graphics(tex_base=0), self.headers)
        self.assertEqual(sig['ps_res'][0][2], ('I', 0))

    def test_implicit_compute_quad(self):
        flat = [0] * 16
        flat[0:4] = vsharp(stride=96)
        rec = {'kind': 1, 'regs': {}, 'stages': [self.stage(5, 6, CS_HASH, flat)]}
        res = S.signature(rec, self.headers)['cs_res']
        self.assertIn(('implicit', 0, S.proj_buffer(vsharp(stride=96), False)), res)

    def test_srgb_and_integer_projection(self):
        self.assertEqual(S.proj_image(tsharp(nfmt=9), False)[-1], 1)
        self.assertEqual(S.proj_image(tsharp(dfmt=38, nfmt=9), False)[-1], 0)   # BC4 has no sRGB
        self.assertEqual(S.proj_image(tsharp(nfmt=4), False)[4], 1)


class Rules(Base):
    def test_db_shader_control(self):
        self.assertEqual(ET.db_shader_control(0x50), 0x60)
        self.assertEqual(ET.db_shader_control(0x11), 0x01)
        self.assertEqual(ET.db_shader_control(0x910), 0x920)
        self.assertEqual(ET.db_shader_control(0x10), 0x10)

    def test_srgb_swap(self):
        w = tsharp(dfmt=35, nfmt=0)
        out = ET.srgb_swap(w, 1 << 20 | 6 << 8, 0)
        f = S.image_fields(out)
        self.assertEqual((f['dfmt'], f['nfmt']), (35, 9))
        self.assertEqual(ET.srgb_swap(w, 1 << 20 | 6 << 8, -1), w)     # descriptor says no
        self.assertEqual(ET.srgb_swap(w, 2 << 20 | 6 << 8, 0), w)      # not an sRGB texture
        self.assertEqual(ET.srgb_swap(w, 1 << 20 | 3 << 8, 0), w)      # code without a swap

    def test_ps_input_cntl(self):
        ps = ps_header([], inputs=(0x10, 0x0F, 0x1410, 0x22))
        self.assertEqual(J.ps_input_cntl(self.vs, ps), [1, 0, 0x421, 0x20])

    def test_state_round_trip(self):
        rec = self.graphics()
        sig = S.signature(rec, self.headers)
        st = J.state_of_signature(sig, rec['regs'])
        regs = J.header_regs(self.vs, self.ps)
        regs.update(J.state_regs(st))
        rec2 = dict(rec, regs=regs)
        self.assertEqual(S.freeze(S.signature(rec2, self.headers)), S.freeze(sig))

    def test_witness_round_trip(self):
        for usage, words in ((0, tsharp(nfmt=9, typ=11)), (0, vsharp(stride=12, dfmt=13)),
                             (1, [1 << 20, 0, 0, 0]), (2, vsharp())):
            w = J.witness_of(usage, words)
            self.assertEqual(J.proj_witness(usage, w),
                             S.proj_slot((usage, 0, 0, 3 if len(words) == 8 else 0), list(words)))

    def test_fetch_loads(self):
        code = struct.pack('<III', 0xC0800000 | 1 << 8 | 5 << 9 | 4 | J.S_LOAD_DWORDX4 << 22,
                           0xC0800000 | 1 << 8 | 5 << 9 | 8 | J.S_LOAD_DWORDX4 << 22, 0xBE802000)
        self.assertEqual(J.fetch_loads(code), [(10, 4), (10, 8)])

    def test_material_bindings(self):
        # two descriptors: 0 -> api 3 (material texture 0), 1 -> api 7 (engine)
        td = bytes(8) + bytes([2, 0, 0, 3]) + struct.pack('<i', 0) + bytes(8) + bytes([6, 0, 0, 7]) + struct.pack('<i', -1)
        effect = (1, td, bytes([0]), b'')
        mat, desc = J.material_bindings(effect, bytes([3, 0, 3, 1, 0, 0]))
        self.assertEqual(mat, {('PS', 3): 0, ('VS', 3): 0})
        self.assertEqual(desc[('PS', 7)], (6, -1))


if __name__ == '__main__':
    unittest.main()


class GcnMem(unittest.TestCase):
    def test_code_stride_and_write(self):
        from xpps import gcn_mem
        # buffer_load_dword idxen offset 0x2C, srsrc s[0:3]; buffer_store_dword idxen, srsrc s[4:7]
        load = struct.pack('<II', 0xE0000000 | 1 << 13 | 0x2C | 12 << 18, 0)
        store = struct.pack('<II', 0xE0000000 | 1 << 13 | 0x0 | 28 << 18, 1 << 16)
        end = struct.pack('<I', 0xBF810000)
        acc = gcn_mem.accesses(load + store + end)
        self.assertEqual(gcn_mem.code_stride(acc, 0), 48)
        self.assertFalse(gcn_mem.written(acc, 0))
        self.assertTrue(gcn_mem.written(acc, 4))


def synthetic_eboot(path, class_hash, tid, impl, size=0x128):
    """A tiny ELF with one text segment (the class table) and one relro segment (the factory)."""
    from xpps import eboot as EB
    text = struct.pack('<QQ', 0x0123, 7) + struct.pack('<QQ', class_hash, tid) + bytes(16)
    fva = EB.FACTORY_VA + (tid - EB.FACTORY_ID) * EB.FACTORY_STRIDE
    entry = struct.pack('<II', tid, impl) + bytes(8) + struct.pack('<QQ', 0x2DA280, 0x2DA2B0)
    entry += bytes(0x74 - len(entry)) + struct.pack('<I', size) + bytes(8)
    relro_va = fva - 0x40
    relro = bytes(0x40) + entry
    phoff, n = 0x40, 2
    text_off = phoff + 56 * n
    relro_off = text_off + len(text)
    hdr = b'\x7fELF' + bytes(0x1C) + struct.pack('<Q', phoff) + bytes(0x10) + struct.pack('<H', n)
    hdr += bytes(0x40 - len(hdr))
    ph = struct.pack('<IIQQQQQQ', 1, 5, text_off, 0, 0, len(text), len(text), 0x4000)
    ph += struct.pack('<IIQQQQQQ', 0x61000010, 4, relro_off, relro_va, relro_va, len(relro), len(relro), 0x4000)
    with open(path, 'wb') as f:
        f.write(hdr + ph + text + relro)


class Eboot(unittest.TestCase):
    def test_class_type_lookup(self):
        from xpps import eboot as EB
        with tempfile.TemporaryDirectory() as d:
            game = Path(d) / 'CUSA00000'
            cache = game / 'art' / 'cache'
            cache.mkdir(parents=True)
            synthetic_eboot(game / 'eboot.bin', 0x0BADC0FFEE123457, 0x3A0, 0x3A6)
            p = EB.find_eboot(str(cache))
            self.assertEqual(p, str(game / 'eboot.bin'))
            eb = EB.Eboot(p)
            self.assertEqual(eb.type_of_class(0x0BADC0FFEE123457), 0x3A0)
            self.assertEqual(eb.factory(0x3A0)['impl'], 0x3A6)
            self.assertEqual(eb.factory(0x3A0)['size'], 0x128)
            self.assertIsNone(eb.type_of_class(0x0123))      # value has no factory entry
            self.assertIsNone(eb.factory(0x3A1))


class Model(Base):
    """v2: the eboot model of the state and of the engine resources (no capture)."""

    def test_flags_of(self):
        f = ET.flags_of(struct.pack('<4I', 5, 0x4E, 0xA0, 0xA1))
        self.assertEqual(f[0], 1 << 5)
        self.assertEqual(f[9], 1 << 6)
        self.assertEqual(f[20], 1)                    # id 160 still counts, 161 does not
        self.assertEqual(len(ET.flags_of(b'')), 21)

    def test_decal_mask_rule(self):
        # FUN_011a3f60: f[9] & 0x40 keeps RT1 and blend off; nothing set -> blend on, RT0 only
        keep = bytearray(21)
        keep[9] = 0x40
        keep[1] = 0x02
        self.assertEqual(ET.class_blends(0x3A1, 0, bytes(keep)), [(0, 0, 0xFC3F & ~0xF000)])
        self.assertEqual(ET.class_blends(0x3A1, 0, bytes(21)), [(0, ET.BL_ALPHA, 0x000F)])

    def test_class_sets(self):
        self.assertEqual(ET.class_sets(0x3C3, 16, 1), ['SHADOW'])
        self.assertEqual(ET.class_sets(0x39B, 0, 5), ['G7'])
        self.assertEqual(ET.class_sets(0x396, 0, 2), ['HDR_ZS'])     # dual source: 2 exports, 1 target
        self.assertEqual(ET.class_sets(None, 255, 1), ET.ENGINE_SETS)
        for name in ET.ENGINE_SETS + ['G7', 'G6', 'G6_T0']:
            targets, mask, depth = ET.RT_SETS[name]
            self.assertLessEqual(mask, (1 << 4 * len(targets)) - 1)

    def test_vertex_by_size(self):
        self.assertEqual(ET.vertex_witness_by_size(3)[8], [4, 5, 6, 1])
        self.assertEqual(ET.vertex_witness_by_size(1)[8], [4, 0, 0, 1])
        self.assertEqual(ET.INSTANCE_STREAM_WITNESS[2], 2)

    def test_engine_tables_from_eboot(self):
        """The hashes are read at the code addresses of the tables (movabs / movabs + lea)."""
        from xpps import eboot as EB
        with tempfile.TemporaryDirectory() as d:
            path = Path(d) / 'eboot.bin'
            synthetic_eboot(path, 0x0BADC0FFEE123457, 0x3A0, 0x3A6)
            eb = EB.Eboot(str(path))
        # patch two code addresses of the real tables into a fake text segment
        ref, spec = ET.ENGINE_BUFFERS[0]
        (base, lea), spec2 = next((r, s) for r, s in ET.ENGINE_BUFFERS if isinstance(r, tuple))
        text = bytearray(max(ref, base, lea) + 16)
        text[ref:ref + 10] = b'\x48\xba' + struct.pack('<Q', 0x1234)
        text[base:base + 10] = b'\x49\xbc' + struct.pack('<Q', 0x5000)
        text[lea:lea + 8] = b'\x49\x8d\x94\x24' + struct.pack('<i', 0x10)
        eb.data, eb.segs, eb.text_end = bytes(text), [(0, 0, len(text))], len(text)
        table = ET.engine_buffer_table(eb)
        self.assertEqual(table[0x1234], spec)
        self.assertEqual(table[0x5010], spec2)
        self.assertEqual(ET.engine_buffer_table(None), {})


class BuildCli(unittest.TestCase):
    """`python -m xpps.jobs build`: index + jobs on synthetic packages, the launcher interface."""

    def test_build(self):
        import contextlib
        import io
        from test_xpps_index import make_pack
        with tempfile.TemporaryDirectory() as d:
            cache = Path(d) / 'CUSA00000' / 'art' / 'cache'
            (cache / 'sub').mkdir(parents=True)
            (cache / 'a.xpps').write_bytes(make_pack())
            (cache / 'sub' / 'core.xpps').write_bytes(make_pack(with_import=False, with_export=True,
                                                               name_seed=5, split=True))
            out = Path(d) / 'out' / 'all.jobs'
            buf, err = io.StringIO(), io.StringIO()
            with contextlib.redirect_stdout(buf), contextlib.redirect_stderr(err):
                rc = J.main(['build', str(cache), str(out)])
            self.assertEqual(rc, 0, err.getvalue())
            lines = buf.getvalue().splitlines()
            self.assertTrue(all(ln.startswith('PROGRESS ') for ln in lines))
            phases = [ln.split()[1] for ln in lines]
            self.assertEqual(phases[0], 'index')
            self.assertEqual(phases[-1], 'jobs')
            self.assertEqual(phases, sorted(phases, key=lambda p: p != 'index'))   # index, then jobs
            self.assertEqual(lines[-1].split()[2], lines[-1].split()[3])          # n == total at the end
            self.assertTrue((Path(d) / 'out' / 'all.index.sqlite').is_file())
            recs = jobfile.read(out)
            self.assertTrue(recs)
            self.assertFalse(Path(str(out) + '.tmp').exists())
            # second run: index reused, same records
            with contextlib.redirect_stdout(io.StringIO()), contextlib.redirect_stderr(io.StringIO()):
                self.assertEqual(J.main(['build', str(cache), str(out)]), 0)
            self.assertEqual(len(jobfile.read(out)), len(recs))
            # errors: exit code 1, the previous output stays
            with contextlib.redirect_stdout(io.StringIO()), contextlib.redirect_stderr(io.StringIO()):
                self.assertEqual(J.main(['build', str(Path(d) / 'missing'), str(out)]), 1)
            self.assertEqual(len(jobfile.read(out)), len(recs))
