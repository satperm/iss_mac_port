# SPDX-License-Identifier: GPL-2.0-or-later
"""Add-on labels found in the player's dlc folder (launcher/addons.py)."""
import struct
import sys
import tempfile
import unittest
from pathlib import Path

from paths import ROOT

sys.path.insert(0, str(ROOT / 'launcher'))
import addons  # noqa: E402

ID_A = 'UP9000-CUSA00223_00-ISSCOSTUMEPACK01'
ID_B = 'EP9000-CUSA00004_00-COLESLEGACY00000'


def make_pkg(path, content_id):
    header = bytearray(0x100)
    header[0:4] = b'\x7fCNT'
    header[0x40:0x40 + 36] = content_id.encode('ascii')
    path.write_bytes(bytes(header))


def make_sfo(path, entries):
    keys = b''
    values = b''
    table = b''
    for name, text in entries.items():
        data = text.encode('ascii') + b'\0'
        table += struct.pack('<HHIII', len(keys), 0x0204, len(data), len(data), len(values))
        keys += name.encode('ascii') + b'\0'
        values += data
    keys_at = 20 + len(table)
    values_at = keys_at + len(keys)
    header = struct.pack('<4sIIII', b'\0PSF', 0x101, keys_at, values_at, len(entries))
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(header + table + keys + values)


class AddonsTest(unittest.TestCase):
    def test_label_of(self):
        self.assertEqual(addons.label_of(ID_A), 'ISSCOSTUMEPACK01')
        self.assertIsNone(addons.label_of('UP9000-CUSA00223'))
        self.assertIsNone(addons.label_of('X' * 36))

    def test_packages_and_dumped_folders(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            make_pkg(root / 'costumes.PKG', ID_A)
            make_sfo(root / 'dump' / 'CUSA00004-legacy' / 'sce_sys' / 'param.sfo',
                     {'CATEGORY': 'ac', 'CONTENT_ID': ID_B})
            make_pkg(root / 'copy-of-costumes.pkg', ID_A)  # the same label once
            (root / 'not-a-package.pkg').write_bytes(b'garbage')
            (root / 'readme.txt').write_text('x')
            self.assertEqual(addons.labels(root), ['COLESLEGACY00000', 'ISSCOSTUMEPACK01'])

    def test_missing_folder(self):
        self.assertEqual(addons.labels(Path(tempfile.gettempdir()) / 'no-such-dlc-folder-iss'), [])


if __name__ == '__main__':
    unittest.main()
