"""tools/shader_index.py finds OrbShdr blocks in a synthetic KCAP-like container (no game data)."""
import io
import struct
import sys
import tempfile
import unittest
from pathlib import Path

from paths import ROOT
sys.path.insert(0, str(ROOT / 'tools'))
import shader_index as si  # noqa: E402


def orb(length, shader_hash, type_id=1):
    flags = (length << si.LENGTH_SHIFT) | (type_id << si.TYPE_SHIFT)
    head = si.SIGNATURE + bytes([7]) + struct.pack('<I', flags) + bytes(4)
    return head + struct.pack('<QI', shader_hash, 0)


def code(size):
    """Code that follows the start rule: s_mov_b32 vcc_hi, (size/8 - 1), then zeros."""
    return struct.pack('<II', si.CODE_MARKER, size // 8 - 1) + bytes(size - 8)


def container(*shaders):
    """KCAP-ish blob: magic, then code + OrbShdr per shader, with padding between. The footer's
    length field is 4 bytes short of the code, as on the game, so the old rule would be wrong."""
    out = bytearray(b'KCAP' + bytes(12))
    for size, h, t in shaders:
        out += code(size) + orb(size - 4, h, t) + bytes(8)
    return bytes(out)


class ShaderIndexTest(unittest.TestCase):
    def test_header_is_28_bytes(self):
        self.assertEqual(len(orb(16, 1)), si.HEADER_SIZE)

    def test_scan_finds_code_span_hash_and_stage(self):
        blob = container((64, 0xDEADBEEF12345678, 0), (32, 0x1, 3))
        counts = si.ScanCounts()
        refs = list(si.scan_stream(io.BytesIO(blob), len(blob), counts))
        self.assertEqual([(r.hash, r.size, r.stage) for r in refs],
                         [(0xDEADBEEF12345678, 64, 'PS'), (0x1, 32, 'T3')])
        self.assertEqual(refs[0].offset, 16)
        self.assertEqual(counts.headers, 2)

    def test_start_is_found_by_the_marker_not_by_length(self):
        blob = b'KCAP' + bytes(12) + code(64) + orb(40, 0x77, 0)
        refs = list(si.scan_stream(io.BytesIO(blob), len(blob), si.ScanCounts()))
        self.assertEqual([(r.offset, r.size) for r in refs], [(16, 64)])

    def test_inconsistent_and_truncated_headers(self):
        blob = bytes(8) + orb(4096, 5)             # no code before it
        blob += orb(0, 6)                          # no code start
        blob += struct.pack('<II', si.CODE_MARKER, 5) + bytes(8) + orb(16, 7)  # literal points elsewhere
        blob += si.SIGNATURE + bytes(5)            # cut by EOF
        counts = si.ScanCounts()
        self.assertEqual(list(si.scan_stream(io.BytesIO(blob), len(blob), counts)), [])
        self.assertEqual((counts.rejected, counts.truncated), (3, 1))

    def test_header_across_block_edge(self):
        old = si.BLOCK_SIZE
        si.BLOCK_SIZE = 40
        try:
            blob = container((16, 0xABC, 1), (16, 0xDEF, 1))
            refs = list(si.scan_stream(io.BytesIO(blob), len(blob), si.ScanCounts()))
        finally:
            si.BLOCK_SIZE = old
        self.assertEqual([r.hash for r in refs], [0xABC, 0xDEF])

    def test_build_lookup_and_signed_hash(self):
        big = 0xFFFFFFFFFFFFFFF0                   # above int64: must survive SQLite
        with tempfile.TemporaryDirectory() as d:
            root, db = Path(d) / 'cache', Path(d) / 'idx.sqlite'
            root.mkdir()
            (root / 'a.xpps').write_bytes(container((16, big, 1)))
            (root / 'b.xpps').write_bytes(container((16, big, 1), (8, 0x22, 0)))
            si.build_index(root, db, 0, False)
            con = si.open_index(db)
            path, off, size, stage, n_occ, _ = si.find_shader(con, big)
            self.assertEqual((path, size, stage, n_occ), ('a.xpps', 16, 'VS', 2))
            data = si.read_shader(root, path, off, size)
            self.assertEqual(len(data), 16)
            self.assertEqual(struct.unpack_from('<I', data)[0], si.CODE_MARKER)
            self.assertIsNone(si.find_shader(con, 0x99))
            known = {si.from_signed(r[0]) for r in con.execute('SELECT hash FROM shaders')}
            self.assertEqual(known, {big, 0x22})

    def test_parse_hash_errors(self):
        self.assertEqual(si.parse_hash('0x10'), 16)
        self.assertEqual(si.parse_hash('16'), 16)
        with self.assertRaises(si.ShaderIndexError):
            si.parse_hash('zz')
        with self.assertRaises(si.ShaderIndexError):
            si.parse_hash('0x1ffffffffffffffff')


if __name__ == '__main__':
    unittest.main()
