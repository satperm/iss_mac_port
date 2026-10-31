"""Data folder resolution, migration copy and the search for saves of other installs
(launcher/userdata.py), and that the game gets the right --user (iss_run.launch)."""
import os
import sys
import tempfile
import unittest
from pathlib import Path
from unittest import mock

from paths import ROOT
sys.path.insert(0, str(ROOT / 'launcher'))
import userdata  # noqa: E402
import iss_run  # noqa: E402


def make_save(data, user='1', slot='infamousssUser0', body=b'x' * 100, mtime=None):
    game = Path(data) / 'savedata' / user / userdata.SERIAL
    (game / slot).mkdir(parents=True, exist_ok=True)
    (game / (slot + '.sce_sys')).mkdir(parents=True, exist_ok=True)
    (game / slot / 'dis.dat').write_bytes(body)
    (game / (slot + '.sce_sys') / 'param.bin').write_bytes(b'\0' * 64)
    if mtime:
        for p in game.rglob('*'):
            os.utime(p, (mtime, mtime))
    return game / slot


class Temp(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.root = Path(self.temp.name)
        self.addCleanup(self.temp.cleanup)


class ResolutionTests(Temp):
    def test_default_is_beside_the_port(self):
        self.assertEqual(self.root / 'user', userdata.user_dir(self.root, env={}, pointer={}))
        self.assertEqual('default', userdata.user_dir_source(env={}, pointer={}))

    def test_pointer_beats_default(self):
        got = userdata.user_dir(self.root, env={}, pointer={'user_dir': str(self.root / 'mine')})
        self.assertEqual(self.root / 'mine', got)
        self.assertEqual('pointer', userdata.user_dir_source(env={}, pointer={'user_dir': 'x'}))

    def test_env_beats_pointer(self):
        env = {'ISS_USER_DIR': str(self.root / 'env')}
        got = userdata.user_dir(self.root, env=env, pointer={'user_dir': str(self.root / 'mine')})
        self.assertEqual(self.root / 'env', got)
        self.assertEqual('env', userdata.user_dir_source(env=env, pointer={}))

    def test_old_env_name_still_works_and_new_one_wins(self):
        self.assertEqual(self.root / 'old', userdata.user_dir(self.root, env={'TLG_USER_DIR': str(self.root / 'old')}, pointer={}))
        env = {'TLG_USER_DIR': 'old', 'ISS_USER_DIR': str(self.root / 'new')}
        self.assertEqual(self.root / 'new', userdata.user_dir(self.root, env=env, pointer={}))

    def test_blank_pointer_value_is_the_default(self):
        self.assertEqual(self.root / 'user', userdata.user_dir(self.root, env={}, pointer={'user_dir': '  '}))

    def test_pointer_locations(self):
        win = userdata.pointer_path(env={'APPDATA': 'C:\\Users\\a\\AppData\\Roaming'}, nt=True)
        self.assertEqual(('ISS-PC', 'launcher.json'), win.parts[-2:])
        lin = userdata.pointer_path(env={'XDG_CONFIG_HOME': '/cfg'}, nt=False)
        self.assertEqual(Path('/cfg/iss-pc/launcher.json'), lin)

    def test_write_pointer_keeps_other_keys_and_reset_removes(self):
        path = self.root / 'cfg' / 'launcher.json'
        userdata.write_pointer({'user_dir': '/a', 'backup_keep': 5}, path)
        userdata.write_pointer({'user_dir': None}, path)
        self.assertEqual({'backup_keep': 5}, userdata.read_pointer(path))
        self.assertEqual({}, userdata.read_pointer(self.root / 'missing.json'))

    def test_real_pointer_through_environment(self):
        env = {'XDG_CONFIG_HOME': str(self.root / 'xdg')}
        with mock.patch.dict(os.environ, env, clear=False), mock.patch.object(userdata.os, 'name', 'posix'):
            os.environ.pop('ISS_USER_DIR', None)
            os.environ.pop('TLG_USER_DIR', None)
            userdata.set_user_dir(self.root / 'saves')
            self.assertEqual(self.root / 'saves', userdata.user_dir(self.root))
            userdata.reset_user_dir()
            self.assertEqual(self.root / 'user', userdata.user_dir(self.root))

    def test_suggestions(self):
        self.assertEqual(('Documents', 'ISS-PC'), userdata.suggested_dir(nt=True).parts[-2:])
        self.assertEqual(Path('/d/iss-pc'), userdata.suggested_dir(nt=False, env={'XDG_DATA_HOME': '/d'}))


class GameGetsTheRightUserTests(Temp):
    """iss_run.launch must hand tlg-probe --user <resolved folder>, and run it from there."""

    def run_launch(self, env, pointer=None):
        port = self.root / 'port'
        settings = dict(iss_run.DEFAULT_SETTINGS, game_dir=str(self.root / 'game'))
        with mock.patch.dict(os.environ, env), \
                mock.patch.object(iss_run, 'OUT_DIR', port / 'out'), \
                mock.patch.object(iss_run, 'LOG_DIR', port / 'out' / 'logs'), \
                mock.patch.object(iss_run, 'PORT_DIR', port), \
                mock.patch.object(iss_run, 'pad_glyphs', return_value=''), \
                mock.patch.object(iss_run, 'probe_env', return_value={}), \
                mock.patch.object(iss_run.subprocess, 'Popen') as popen, \
                mock.patch.object(userdata, 'read_pointer', return_value=pointer or {}):
            iss_run.launch(settings)
        return popen.call_args

    def test_default_pointer_and_env(self):
        with mock.patch.dict(os.environ):
            for name in ('ISS_USER_DIR', 'TLG_USER_DIR', 'ISS_GPU_USER_DIR', 'TLG_GPU_USER_DIR'):
                os.environ.pop(name, None)
            call = self.run_launch({})
            cmd = call.args[0]
            self.assertEqual(str(self.root / 'port' / 'user'), cmd[cmd.index('--user') + 1])
            self.assertEqual(str(self.root / 'port' / 'user'), call.kwargs['cwd'])
            self.assertEqual(str(self.root / 'port' / 'user' / 'tlg.ini'), call.kwargs['env']['ISS_CONFIG'])
            self.assertEqual(str(self.root / 'port' / 'user'), call.kwargs['env']['TLG_GPU_USER_DIR'])

            call = self.run_launch({}, {'user_dir': str(self.root / 'docs')})
            cmd = call.args[0]
            self.assertEqual(str(self.root / 'docs'), cmd[cmd.index('--user') + 1])
            self.assertTrue((self.root / 'docs' / 'tlg.ini').is_file())
            self.assertEqual(str(self.root / 'docs'), call.kwargs['env']['TLG_GPU_USER_DIR'])

            call = self.run_launch({'ISS_USER_DIR': str(self.root / 'envdir')}, {'user_dir': 'ignored'})
            cmd = call.args[0]
            self.assertEqual(str(self.root / 'envdir'), cmd[cmd.index('--user') + 1])


class MigrationTests(Temp):
    def test_copies_saves_and_settings_and_keeps_the_old_ones(self):
        old, new = self.root / 'old', self.root / 'new'
        save = make_save(old, body=b'hello')
        (old / 'tlg.ini').write_text('bp01=0\n')
        (old / 'cache').mkdir()
        (old / 'cache' / 'big').write_bytes(b'z' * 1000)
        result = userdata.migrate(old, new)
        self.assertEqual({'savedata', 'tlg.ini'}, set(result['copied']))
        self.assertEqual(b'hello', (new / 'savedata' / '1' / userdata.SERIAL / 'infamousssUser0' / 'dis.dat').read_bytes())
        self.assertEqual('bp01=0\n', (new / 'tlg.ini').read_text())
        self.assertFalse((new / 'cache').exists())            # the shader cache is rebuilt, not copied
        self.assertTrue(save.is_dir())                         # nothing moved or deleted
        self.assertEqual([], [p.name for p in new.iterdir() if 'copying' in p.name])

    def test_existing_items_in_the_new_folder_are_kept(self):
        old, new = self.root / 'old', self.root / 'new'
        make_save(old, body=b'old')
        make_save(new, body=b'new')
        result = userdata.migrate(old, new)
        self.assertEqual(['savedata'], result['skipped'])
        self.assertEqual(b'new', (new / 'savedata' / '1' / userdata.SERIAL / 'infamousssUser0' / 'dis.dat').read_bytes())

    def test_same_folder_is_refused(self):
        make_save(self.root / 'a')
        with self.assertRaises(userdata.DataError) as caught:
            userdata.migrate(self.root / 'a', self.root / 'a' / '.')
        self.assertEqual('data_same_folder', caught.exception.key)

    def test_no_space_is_refused_before_copying(self):
        make_save(self.root / 'old')
        with mock.patch.object(userdata, 'free_bytes', return_value=10):
            with self.assertRaises(userdata.DataError) as caught:
                userdata.migrate(self.root / 'old', self.root / 'new')
        self.assertEqual('data_no_space', caught.exception.key)
        self.assertFalse((self.root / 'new' / 'savedata').exists())

    def test_copy_error_leaves_no_half_copy_and_the_original(self):
        old, new = self.root / 'old', self.root / 'new'
        make_save(old)
        with mock.patch.object(userdata.shutil, 'copytree', side_effect=OSError(28, 'No space left on device')):
            with self.assertRaises(userdata.DataError) as caught:
                userdata.migrate(old, new)
        self.assertEqual('data_copy_failed', caught.exception.key)
        self.assertTrue(has(old))
        self.assertFalse(has(new))


def has(data):
    return userdata.has_saves(data)


class ScanTests(Temp):
    def test_finds_folder_inside_folder_and_ignores_current(self):
        downloads = self.root / 'Downloads'
        inner = downloads / 'iss-windows-x86_64' / 'iss-windows-x86_64' / 'user'
        make_save(inner, slot='infamousssAuto0', mtime=1_700_000_000)
        make_save(inner, slot='infamousssUser0', mtime=1_700_000_100)
        current = self.root / 'current' / 'user'
        make_save(current)
        other = self.root / 'Desktop' / 'iss-old' / 'user'
        make_save(other, mtime=1_600_000_000)
        found = userdata.find_installs([downloads, self.root / 'Desktop', self.root / 'current'], exclude=[current])
        self.assertEqual([str(inner.resolve()), str(other.resolve())], [f['path'] for f in found])   # newest first
        self.assertEqual(2, found[0]['saves'])
        self.assertAlmostEqual(1_700_000_100, found[0]['newest'], delta=2)

    def test_data_folder_without_user_level_is_found(self):
        docs = self.root / 'Documents' / 'ISS-PC'
        make_save(docs)
        found = userdata.find_installs([self.root / 'Documents'])
        self.assertEqual([str(docs.resolve())], [f['path'] for f in found])

    def test_empty_savedata_and_other_games_are_not_saves(self):
        (self.root / 'a' / 'user' / 'savedata' / '1' / 'CUSA99999' / 'slot').mkdir(parents=True)
        (self.root / 'b' / 'user' / 'savedata').mkdir(parents=True)
        self.assertEqual([], userdata.find_installs([self.root]))

    def test_depth_and_budget_limits(self):
        deep = self.root / 'a' / 'b' / 'c' / 'd' / 'e' / 'user'
        make_save(deep)
        self.assertEqual([], userdata.find_installs([self.root], depth=3))
        shallow = self.root / 'iss' / 'x' / 'y' / 'user'      # 'iss' in the name: deeper search
        make_save(shallow)
        self.assertEqual(1, len(userdata.find_installs([self.root], depth=3)))
        self.assertEqual([], userdata.find_installs([self.root], max_dirs=0))

    def test_candidate_roots_include_sibling_iss_folders(self):
        port = self.root / 'game-v2' / 'port'
        port.mkdir(parents=True)
        (self.root / 'game-v1-iss').mkdir()
        roots = userdata.candidate_roots(port, nt=False, home=self.root / 'home')
        self.assertIn((self.root / 'game-v1-iss').resolve(), [r.resolve() for r in roots])


if __name__ == '__main__':
    unittest.main()
