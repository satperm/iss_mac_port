"""Save management (launcher/savemanager.py): slots, backups, delete/duplicate, .zip export and
import (invalid zips, ../ traversal), restore, and the guard that backs up before any change."""
import os
import struct
import sys
import tempfile
import unittest
import zipfile
from pathlib import Path
from unittest import mock

from paths import ROOT
sys.path.insert(0, str(ROOT / 'launcher'))
import savemanager as sm  # noqa: E402
import userdata  # noqa: E402
import save_backup  # noqa: E402

NOT_RUNNING = lambda: False   # noqa: E731
RUNNING = lambda: True        # noqa: E731


def raw_param(subtitle, when=1_792_008_146, title='ISS'):
    data = bytearray(1328)
    data[0:len(title.encode())] = title.encode()
    data[128:128 + len(subtitle.encode())] = subtitle.encode()
    struct.pack_into('<Q', data, 0x508, when)
    return bytes(data)


def put_slot(data, slot, subtitle='Welcome to Seattle (Good Karma) - 8%', body=b'save', uid='1', icon=True):
    game = Path(data) / 'savedata' / uid / userdata.SERIAL
    (game / slot).mkdir(parents=True, exist_ok=True)
    (game / (slot + '.sce_sys')).mkdir(parents=True, exist_ok=True)
    (game / slot / 'dis.dat').write_bytes(body)
    (game / (slot + '.sce_sys') / 'param.bin').write_bytes(raw_param(subtitle))
    if icon:
        (game / (slot + '.sce_sys') / 'icon0.png').write_bytes(b'\x89PNG fake')


def body_of(data, slot, uid='1'):
    """dis.dat of a slot in a data folder, or in a backup folder (which is a copy of savedata/)."""
    data = Path(data)
    if data.parent.name == 'savedata_backups':
        return (data / uid / userdata.SERIAL / slot / 'dis.dat').read_bytes()
    return (Path(data) / 'savedata' / uid / userdata.SERIAL / slot / 'dis.dat').read_bytes()


class Base(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.user = self.root / 'user'
        env = mock.patch.dict(os.environ, {'XDG_CONFIG_HOME': str(self.root / 'cfg'), 'APPDATA': str(self.root / 'cfg')})
        env.start()
        self.addCleanup(env.stop)
        for name in ('ISS_USER_DIR', 'TLG_USER_DIR'):
            os.environ.pop(name, None)

    def zip_with(self, names, name='x.zip', data=b'd'):
        path = self.root / name
        with zipfile.ZipFile(path, 'w') as zf:
            for n in names:
                zf.writestr(n, data)
        return path


class ParamTests(Base):
    def test_raw_param_bin_layout(self):
        got = sm.read_param(raw_param('Bienvenido a Seattle (Protector) - 8%'))
        self.assertEqual('Bienvenido a Seattle (Protector) - 8%', got['subtitle'])
        self.assertEqual('ISS', got['title'])
        self.assertEqual(1_792_008_146, got['time'])

    def test_garbage_gives_empty_fields(self):
        self.assertEqual('', sm.display_title(sm.read_param(b'')))
        self.assertEqual(0, sm.read_param(b'\xff' * 2000)['time'])


class SlotTests(Base):
    def test_slot_cards(self):
        put_slot(self.user, 'infamousssUser1', 'B', b'x' * 10)
        put_slot(self.user, 'infamousssAuto0', 'A')
        put_slot(self.user, 'infamousssUser0', 'C', icon=False)
        slots = sm.list_slots(self.user)
        self.assertEqual(['infamousssAuto0', 'infamousssUser0', 'infamousssUser1'], [s['name'] for s in slots])
        self.assertEqual(['auto', 'user', 'user'], [s['kind'] for s in slots])
        self.assertEqual('A', slots[0]['title'])
        self.assertTrue(slots[0]['icon'].endswith('icon0.png'))
        self.assertEqual('', slots[1]['icon'])
        self.assertEqual(1_792_008_146, slots[0]['time'])
        self.assertGreater(slots[0]['size'], 1000)

    def test_duplicate_picks_next_free_user_slot_and_keeps_the_original(self):
        put_slot(self.user, 'infamousssAuto0', body=b'auto')
        put_slot(self.user, 'infamousssUser0', body=b'u0')
        new = sm.duplicate_slot(self.user, '1/infamousssAuto0', running=NOT_RUNNING)
        self.assertEqual('1/infamousssUser1', new)
        self.assertEqual(b'auto', body_of(self.user, 'infamousssUser1'))
        self.assertEqual(b'auto', body_of(self.user, 'infamousssAuto0'))
        self.assertEqual(b'u0', body_of(self.user, 'infamousssUser0'))
        self.assertTrue((self.user / 'savedata/1' / userdata.SERIAL / 'infamousssUser1.sce_sys' / 'icon0.png').is_file())

    def test_duplicate_refuses_while_the_game_runs(self):
        put_slot(self.user, 'infamousssUser0')
        with self.assertRaises(sm.SaveError) as caught:
            sm.duplicate_slot(self.user, '1/infamousssUser0', running=RUNNING)
        self.assertEqual('save_game_running', caught.exception.key)
        self.assertEqual(1, len(sm.list_slots(self.user)))

    def test_bad_slot_ids_are_refused(self):
        put_slot(self.user, 'infamousssUser0')
        for bad in ('../x/infamousssUser0', '1/../../etc', '1/other', 'nope'):
            with self.assertRaises(sm.SaveError, msg=bad):
                sm.delete_slot(self.user, bad, running=NOT_RUNNING)


class BackupAndDeleteTests(Base):
    def test_delete_backs_up_first_then_removes_only_that_slot(self):
        put_slot(self.user, 'infamousssUser0', body=b'keep')
        put_slot(self.user, 'infamousssUser1', body=b'gone')
        backup = sm.delete_slot(self.user, '1/infamousssUser1', running=NOT_RUNNING)
        self.assertIsNotNone(backup['path'])
        self.assertEqual(['1/infamousssUser0'], [s['id'] for s in sm.list_slots(self.user)])
        self.assertFalse((self.user / 'savedata/1' / userdata.SERIAL / 'infamousssUser1.sce_sys').exists())
        # the deleted save is in the backup
        self.assertEqual(b'gone', body_of(backup['path'], 'infamousssUser1'))
        self.assertEqual([], [p.name for p in self.user.iterdir() if p.name.startswith('.deleting')])

    def test_no_backup_means_no_delete(self):
        put_slot(self.user, 'infamousssUser0')
        with mock.patch.object(sm.save_backup, 'backup', side_effect=OSError(28, 'No space left on device')):
            with self.assertRaises(sm.SaveError) as caught:
                sm.delete_slot(self.user, '1/infamousssUser0', running=NOT_RUNNING)
        self.assertEqual('save_backup_failed', caught.exception.key)
        self.assertEqual(1, len(sm.list_slots(self.user)))     # still there

    def test_delete_refuses_while_the_game_runs_and_makes_no_backup(self):
        put_slot(self.user, 'infamousssUser0')
        with self.assertRaises(sm.SaveError):
            sm.delete_slot(self.user, '1/infamousssUser0', running=RUNNING)
        self.assertEqual([], save_backup.list_backups(self.user))

    def test_backup_is_reused_when_nothing_changed(self):
        put_slot(self.user, 'infamousssUser0')
        first = sm.backup_now(self.user)
        second = sm.backup_now(self.user)
        self.assertFalse(first['reused'])
        self.assertTrue(second['reused'])
        self.assertEqual(first['path'], second['path'])
        put_slot(self.user, 'infamousssUser0', body=b'changed!')
        os.utime(self.user / 'savedata/1' / userdata.SERIAL / 'infamousssUser0' / 'dis.dat', (1, 1_900_000_000))
        self.assertFalse(sm.backup_now(self.user)['reused'])
        self.assertEqual(2, len(sm.list_backups(self.user)))

    def test_extra_folder_gets_a_copy_and_keep_is_honored(self):
        extra = self.root / 'disk2'
        put_slot(self.user, 'infamousssUser0')
        sm.set_prefs(keep=3, extra=str(extra))
        self.assertEqual({'keep': 3, 'extra': str(extra)}, sm.prefs())
        for i in range(5):
            put_slot(self.user, 'infamousssUser0', body=b'v%d' % i)
            os.utime(self.user / 'savedata/1' / userdata.SERIAL / 'infamousssUser0' / 'dis.dat', (1_700_000_000 + i * 100,) * 2)
            sm.backup_now(self.user)
        self.assertEqual(3, len(sm.list_backups(self.user)))
        self.assertEqual(3, len(list(extra.iterdir())))
        self.assertEqual(sorted(p.name for p in extra.iterdir()), sorted(b['name'] for b in sm.list_backups(self.user)))

    def test_extra_folder_failure_is_reported_not_fatal(self):
        put_slot(self.user, 'infamousssUser0')
        blocker = self.root / 'file'
        blocker.write_text('x')
        result = sm.backup_now(self.user, extra=str(blocker / 'sub'))
        self.assertIsNotNone(result['path'])
        self.assertTrue(result['extra_error'])

    def test_keep_is_clamped(self):
        sm.set_prefs(keep=1)
        self.assertEqual(sm.KEEP_MIN, sm.prefs()['keep'])
        sm.set_prefs(keep=10_000)
        self.assertEqual(sm.KEEP_MAX, sm.prefs()['keep'])

    def test_backup_before_play_never_raises(self):
        put_slot(self.user, 'infamousssUser0')
        self.assertIn('backed up', sm.backup_before_play(self.user))
        self.assertEqual('', sm.backup_before_play(self.user))            # unchanged: nothing new
        with mock.patch.object(sm.save_backup, 'backup', side_effect=OSError('disk full')):
            put_slot(self.user, 'infamousssUser0', body=b'zzzzzz')
            os.utime(self.user / 'savedata/1' / userdata.SERIAL / 'infamousssUser0' / 'dis.dat', (1, 1_900_000_000))
            self.assertIn('failed', sm.backup_before_play(self.user))
        self.assertEqual('', sm.backup_before_play(self.root / 'empty'))   # no saves, no backup

    def test_game_running_detection(self):
        self.assertTrue(sm.game_running(lambda: ['bash', 'tlg-probe']))
        self.assertTrue(sm.game_running(lambda: ['TLG-PROBE.EXE']))
        self.assertFalse(sm.game_running(lambda: ['bash', 'firefox']))

    def test_process_names_windows_no_stdout(self):
        """tasklist blocked or intercepted (antivirus, sandboxing): stdout comes back None rather
        than raising, and the caller must not crash on it."""
        with mock.patch.object(sm.os, 'name', 'nt'), \
             mock.patch.object(sm.subprocess, 'run', return_value=mock.Mock(stdout=None)):
            self.assertEqual([], sm._process_names())


class RestoreTests(Base):
    def test_restore_brings_back_the_backup_and_backs_up_what_it_replaces(self):
        put_slot(self.user, 'infamousssUser0', body=b'old')
        old = sm.backup_now(self.user)['path']
        put_slot(self.user, 'infamousssUser0', body=b'new!!')
        result = sm.restore_backup(self.user, old.name, running=NOT_RUNNING)
        self.assertEqual(b'old', body_of(self.user, 'infamousssUser0'))
        self.assertEqual(b'new!!', body_of(result['path'], 'infamousssUser0'))      # what was replaced is safe
        self.assertFalse((self.user / 'savedata.replaced').exists())

    def test_restore_unknown_backup_and_running_game(self):
        put_slot(self.user, 'infamousssUser0')
        with self.assertRaises(sm.SaveError) as caught:
            sm.restore_backup(self.user, '19990101-000000', running=NOT_RUNNING)
        self.assertEqual('save_backup_missing', caught.exception.key)
        with self.assertRaises(sm.SaveError):
            sm.restore_backup(self.user, 'x', running=RUNNING)

    def test_list_backups_has_date_and_count(self):
        put_slot(self.user, 'infamousssUser0')
        put_slot(self.user, 'infamousssAuto0')
        sm.backup_now(self.user)
        (info,) = sm.list_backups(self.user)
        self.assertEqual(2, info['saves'])
        self.assertGreater(info['time'], 1_700_000_000)


class ZipTests(Base):
    def test_export_then_import_round_trip(self):
        put_slot(self.user, 'infamousssAuto0', 'A', b'auto')
        put_slot(self.user, 'infamousssUser0', 'U', b'user')
        out = self.root / 'out' / 'saves.zip'
        self.assertEqual(2, sm.export_zip(self.user, out))
        with zipfile.ZipFile(out) as zf:
            self.assertIn('savedata/1/CUSA00223/infamousssUser0/dis.dat', zf.namelist())
            self.assertIn('savedata/1/CUSA00223/infamousssUser0.sce_sys/param.bin', zf.namelist())
        self.assertFalse(out.with_name('saves.zip.tmp').exists())
        other = self.root / 'other'
        plan = sm.plan_from_zip(out)
        result = sm.import_plan(other, plan, running=NOT_RUNNING)
        self.assertEqual(['1/infamousssAuto0', '1/infamousssUser0'], sorted(result['imported']))
        self.assertIsNone(result['backup'])                    # nothing to replace: no backup needed
        self.assertEqual(b'user', body_of(other, 'infamousssUser0'))
        self.assertEqual('U', sm.list_slots(other)[1]['title'])

    def test_export_one_slot(self):
        put_slot(self.user, 'infamousssAuto0')
        put_slot(self.user, 'infamousssUser0')
        out = self.root / 'one.zip'
        self.assertEqual(1, sm.export_zip(self.user, out, ['1/infamousssUser0']))
        self.assertEqual({('1', 'infamousssUser0')}, set(sm.plan_from_zip(out).slots))

    def test_existing_save_is_never_overwritten_without_asking(self):
        put_slot(self.user, 'infamousssUser0', 'mine', b'mine')
        src = self.root / 'src'
        put_slot(src, 'infamousssUser0', 'theirs', b'theirs')
        put_slot(src, 'infamousssUser1', 'new', b'new')
        sm.export_zip(src, self.root / 'in.zip')
        plan = sm.plan_from_zip(self.root / 'in.zip')
        preview = {p['id']: p for p in sm.import_preview(self.user, plan)}
        self.assertTrue(preview['1/infamousssUser0']['exists'])
        self.assertEqual('mine', preview['1/infamousssUser0']['old_title'])
        self.assertEqual('theirs', preview['1/infamousssUser0']['title'])
        self.assertFalse(preview['1/infamousssUser1']['exists'])
        result = sm.import_plan(self.user, plan, running=NOT_RUNNING)
        self.assertEqual(['1/infamousssUser0'], result['skipped'])
        self.assertEqual(['1/infamousssUser1'], result['imported'])
        self.assertEqual(b'mine', body_of(self.user, 'infamousssUser0'))
        self.assertEqual([], save_backup.list_backups(self.user))        # nothing replaced, no backup

    def test_replace_backs_up_first(self):
        put_slot(self.user, 'infamousssUser0', 'mine', b'mine')
        src = self.root / 'src'
        put_slot(src, 'infamousssUser0', 'theirs', b'theirs')
        sm.export_zip(src, self.root / 'in.zip')
        result = sm.import_plan(self.user, sm.plan_from_zip(self.root / 'in.zip'), replace=['1/infamousssUser0'],
                                running=NOT_RUNNING)
        self.assertEqual(['1/infamousssUser0'], result['replaced'])
        self.assertEqual(b'theirs', body_of(self.user, 'infamousssUser0'))
        self.assertEqual(b'mine', body_of(result['backup']['path'], 'infamousssUser0'))
        self.assertEqual([], [p.name for p in (self.user / 'savedata/1' / userdata.SERIAL).iterdir() if p.name.startswith('.')])

    def test_replace_failure_puts_the_old_save_back(self):
        put_slot(self.user, 'infamousssUser0', 'mine', b'mine')
        src = self.root / 'src'
        put_slot(src, 'infamousssUser0', 'theirs', b'theirs')
        sm.export_zip(src, self.root / 'in.zip')
        real = os.replace
        calls = []

        def flaky(a, b):
            calls.append(b)
            if len(calls) == 3:        # the move of the new data into place
                raise OSError(13, 'Permission denied')
            return real(a, b)
        with mock.patch.object(sm.os, 'replace', flaky):
            with self.assertRaises(sm.SaveError) as caught:
                sm.import_plan(self.user, sm.plan_from_zip(self.root / 'in.zip'), replace=['1/infamousssUser0'],
                               running=NOT_RUNNING)
        self.assertEqual('save_import_failed', caught.exception.key)
        self.assertEqual(b'mine', body_of(self.user, 'infamousssUser0'))

    def test_import_refuses_while_the_game_runs(self):
        src = self.root / 'src'
        put_slot(src, 'infamousssUser0')
        sm.export_zip(src, self.root / 'in.zip')
        with self.assertRaises(sm.SaveError):
            sm.import_plan(self.user, sm.plan_from_zip(self.root / 'in.zip'), running=RUNNING)
        self.assertFalse(self.user.exists())

    def test_zip_with_other_user_id_goes_to_the_only_local_user(self):
        put_slot(self.user, 'infamousssUser0', uid='7')
        src = self.root / 'src'
        put_slot(src, 'infamousssUser1', uid='1')
        sm.export_zip(src, self.root / 'in.zip')
        plan = sm.plan_from_zip(self.root / 'in.zip')
        target = sm.default_target_uid(self.user, plan)
        self.assertEqual('7', target)
        sm.import_plan(self.user, plan, target_uid=target, running=NOT_RUNNING)
        self.assertEqual(b'save', body_of(self.user, 'infamousssUser1', uid='7'))

    def test_invalid_zips(self):
        junk = self.root / 'junk.zip'
        junk.write_bytes(b'this is not a zip')
        with self.assertRaises(sm.SaveError) as caught:
            sm.plan_from_zip(junk)
        self.assertEqual('zip_invalid', caught.exception.key)
        empty = self.zip_with(['readme.txt', 'photos/a.png'], 'empty.zip')
        with self.assertRaises(sm.SaveError) as caught:
            sm.plan_from_zip(empty)
        self.assertEqual('zip_empty', caught.exception.key)
        wrong = self.zip_with(['savedata/1/CUSA99999/infamousssUser0/dis.dat', 'savedata/1/CUSA00223/other/dis.dat'], 'wrong.zip')
        with self.assertRaises(sm.SaveError) as caught:
            sm.plan_from_zip(wrong)
        self.assertEqual('zip_empty', caught.exception.key)

    def test_path_traversal_and_absolute_paths_are_refused_whole(self):
        for bad in ('savedata/1/CUSA00223/infamousssUser0/../../../../evil.txt', '../evil.txt', '/etc/evil',
                    'savedata\\..\\..\\evil', 'C:/evil.txt', 'savedata/1/CUSA00223/infamousssUser0/..\\x'):
            path = self.zip_with(['savedata/1/CUSA00223/infamousssUser0/dis.dat', bad], 'bad.zip')
            with self.assertRaises(sm.SaveError, msg=bad) as caught:
                sm.plan_from_zip(path)
            self.assertEqual('zip_unsafe', caught.exception.key, bad)
            self.assertFalse((self.root.parent / 'evil.txt').exists())

    def test_symlink_member_is_refused(self):
        path = self.root / 'link.zip'
        with zipfile.ZipFile(path, 'w') as zf:
            info = zipfile.ZipInfo('savedata/1/CUSA00223/infamousssUser0/dis.dat')
            info.external_attr = (0o120777 << 16)
            zf.writestr(info, '/etc/passwd')
        with self.assertRaises(sm.SaveError) as caught:
            sm.plan_from_zip(path)
        self.assertEqual('zip_unsafe', caught.exception.key)

    def test_limits(self):
        path = self.zip_with(['savedata/1/CUSA00223/infamousssUser0/f%d' % i for i in range(20)], 'many.zip')
        with mock.patch.object(sm, 'ZIP_MAX_FILES', 10):
            with self.assertRaises(sm.SaveError) as caught:
                sm.plan_from_zip(path)
        self.assertEqual('zip_too_big', caught.exception.key)
        with mock.patch.object(sm, 'ZIP_MAX_BYTES', 5):
            with self.assertRaises(sm.SaveError):
                sm.plan_from_zip(self.zip_with(['savedata/1/CUSA00223/infamousssUser0/f'], 'big.zip', data=b'x' * 100))

    def test_wrapper_folders_are_tolerated(self):
        path = self.zip_with(['user/savedata/1/CUSA00223/infamousssUser0/dis.dat',
                              'user/savedata/1/CUSA00223/infamousssUser0.sce_sys/param.bin', 'user/tlg.ini'], 'wrap.zip')
        plan = sm.plan_from_zip(path)
        self.assertEqual({('1', 'infamousssUser0')}, set(plan.slots))
        self.assertEqual(1, plan.ignored)

    def test_import_from_another_install_folder(self):
        old = self.root / 'Downloads/iss/iss/user'
        put_slot(old, 'infamousssUser0', 'old', b'old')
        plan = sm.plan_from_folder(old)
        sm.import_plan(self.user, plan, running=NOT_RUNNING)
        self.assertEqual(b'old', body_of(self.user, 'infamousssUser0'))
        self.assertEqual(b'old', body_of(old, 'infamousssUser0'))        # the source is untouched
        with self.assertRaises(sm.SaveError):
            sm.plan_from_folder(self.root / 'empty')


if __name__ == '__main__':
    unittest.main()
