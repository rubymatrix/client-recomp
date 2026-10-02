"""tools/setup.py's settings: the questions, what follows from the answers, and the saved files they
update. Nothing is built; the saved files are in a temporary folder.

  python3 tests/setup_test.py [-v]
"""
import builtins
import os
import plistlib
import sys
import tempfile
import types
import unittest
from unittest import mock

sys.path.insert(0, os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), 'tools'))
import setup  # noqa: E402

SETTINGS_REG = ('REGEDIT4\r\n\r\n[HKEY_LOCAL_MACHINE\\SOFTWARE\\Test]\r\n'
                '"0001"=dword:00000780\r\n"0002"=dword:00000438\r\n"0034"=dword:00000001\r\n'
                '"0037"=dword:000003c0\r\n"0038"=dword:0000021c\r\n')


def options(**kw):
    o = dict(install_dir=None, server=None, resolution=None, window_mode=None, dats=None)
    o.update(kw)
    return types.SimpleNamespace(**o)


class Sandbox(unittest.TestCase):
    """A data folder, an install folder and a DAT overlay of their own; the terminal is scripted."""

    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.root = self.tmp.name
        self.data = os.path.join(self.root, 'data')
        self.apps = os.path.join(self.root, 'Applications')
        self.dats = os.path.join(self.root, 'dats')
        os.makedirs(os.path.join(self.dats, 'era', 'ROM2'))
        os.makedirs(os.path.join(self.root, 'empty'))
        patch = mock.patch.object(setup, 'DATA_DIR', self.data)
        patch.start()
        self.addCleanup(patch.stop)
        self.addCleanup(self.tmp.cleanup)
        self.asked = []

    def saved(self, reg=SETTINGS_REG, cfg='server=old.example\nuser=bob\n', modern=None):
        os.makedirs(self.data, exist_ok=True)
        for name, text in (('settings.reg', reg), ('signin.cfg', cfg), ('modern.cfg', modern)):
            if text is not None:
                with open(os.path.join(self.data, name), 'w', newline='') as f:
                    f.write(text)

    def installed(self, **keys):
        contents = os.path.join(self.apps, setup.APP_NAME + '.app', 'Contents')
        os.makedirs(contents)
        with open(os.path.join(contents, 'Info.plist'), 'wb') as f:
            plistlib.dump(keys, f)

    def choose(self, answers, **kw):
        """choose_settings with answers typed on a terminal (None: not a terminal)."""
        it = iter(answers or [])

        def typed(prompt):
            self.asked.append(prompt)
            return next(it)
        with mock.patch.object(sys.stdin, 'isatty', lambda: answers is not None), \
                mock.patch.object(builtins, 'input', typed), mock.patch('builtins.print'):
            s = setup.choose_settings(options(install_dir=self.apps, **kw))
        self.assertIsNone(next(it, None), 'answers left over')
        return s

    def read(self, name):
        with open(os.path.join(self.data, name), newline='') as f:
            return f.read()


class Derived(unittest.TestCase):
    def test_resolution(self):
        self.assertEqual(setup.parse_resolution('2560x1440'), (2560, 1440))
        self.assertEqual(setup.parse_resolution(' 1920 * 1080 '), (1920, 1080))
        self.assertEqual(setup.parse_resolution('1920X1080'), (1920, 1080))
        for bad in ('', 'big', '1920', '639x480', '640x479', '1920x1080x2', None):
            self.assertIsNone(setup.parse_resolution(bad), bad)

    def test_menus_and_interface(self):
        self.assertEqual(setup.derive(1920, 1080), ((960, 540), 'off'))
        self.assertEqual(setup.derive(2560, 1600), ((1280, 800), 'off'))
        self.assertEqual(setup.derive(1024, 768), ((512, 384), 'off'))
        self.assertEqual(setup.derive(1280, 720), ((682, 384), 'off'))  # 384 high at least
        self.assertEqual(setup.derive(3440, 1440), ((1280, 720), '16:9'))  # ultrawide: 16:9 in the middle
        self.assertEqual(setup.derive(5120, 1440), ((1280, 720), '16:9'))
        self.assertEqual(setup.derive(640, 480), ((512, 384), 'off'))

    def test_aspect_name(self):
        self.assertEqual(setup.aspect_name(1920, 1080), '16:9')
        self.assertEqual(setup.aspect_name(3440, 1440), '43:18')

    def test_app_options(self):
        s = {'server': 'a.b', 'resolution': (3440, 1440), 'menu_resolution': (1280, 720), 'window_mode': 3,
             'ui_aspect': '16:9', 'dats': ''}
        self.assertEqual(setup.app_options(s), ['--server', 'a.b', '--resolution', '3440x1440', '--menu-resolution',
                                                '1280x720', '--window-mode', '3', '--ui-aspect', '16:9'])
        self.assertEqual(setup.app_options(dict(s, dats='/d'))[-2:], ['--dats', '/d'])


class Dats(Sandbox):
    def test_layouts(self):
        self.assertTrue(setup.has_dats(self.dats))  # a folder of overlays
        self.assertTrue(setup.has_dats(os.path.join(self.dats, 'era')))  # one overlay
        self.assertFalse(setup.has_dats(os.path.join(self.root, 'empty')))
        self.assertFalse(setup.has_dats(os.path.join(self.root, 'missing')))
        os.makedirs(os.path.join(self.root, 'snd', 'Sound3'))
        self.assertTrue(setup.has_dats(os.path.join(self.root, 'snd')))  # any case


class FirstRun(Sandbox):
    def test_blank_answers(self):
        s = self.choose(['', '', '', ''])
        self.assertEqual((s['server'], s['resolution'], s['window_mode'], s['dats']),
                         ('127.0.0.1', (1920, 1080), 1, ''))
        self.assertEqual((s['menu_resolution'], s['ui_aspect']), ((960, 540), 'off'))

    def test_answers(self):
        s = self.choose(['play.example.net', 'bogus', '3440x1440', '9', '3', self.root + '/empty', self.dats])
        self.assertEqual((s['server'], s['resolution'], s['window_mode'], s['dats']),
                         ('play.example.net', (3440, 1440), 3, self.dats))
        self.assertEqual((s['menu_resolution'], s['ui_aspect']), ((1280, 720), '16:9'))

    def test_dats_path_forms(self):
        with mock.patch.dict(os.environ, {'HOME': self.root}):
            s = self.choose(['', '', '', "'~/dats'"])
        self.assertEqual(s['dats'], self.dats)
        spaced = os.path.join(self.root, 'my dats')
        os.rename(self.dats, spaced)
        s = self.choose(['', '', '', spaced.replace(' ', '\\ ') + ' '])  # a folder dragged into Terminal
        self.assertEqual(s['dats'], spaced)

    def test_relative_to_caller(self):
        with mock.patch.object(setup, 'CALLER_DIR', self.root):
            self.assertEqual(self.choose(['', '', '', 'dats'])['dats'], self.dats)
            self.assertEqual(self.choose(None, dats='./dats')['dats'], self.dats)

    def test_bad_server_asked_again(self):
        s = self.choose(['not a server', 'ok.example', '', '', ''])
        self.assertEqual(s['server'], 'ok.example')

    def test_options_answer_instead(self):
        s = self.choose([], server='x.example', resolution='2560x1440', window_mode=0, dats='none')
        self.assertEqual(self.asked, [])
        self.assertEqual((s['server'], s['resolution'], s['window_mode'], s['dats']),
                         ('x.example', (2560, 1440), 0, ''))

    def test_not_a_terminal(self):
        s = self.choose(None)
        self.assertEqual((s['server'], s['resolution'], s['window_mode'], s['dats']),
                         ('127.0.0.1', (1920, 1080), 1, ''))

    def test_bad_options(self):
        with self.assertRaises(setup.Failure):
            self.choose(None, resolution='huge')
        with self.assertRaises(setup.Failure):
            self.choose(None, dats=os.path.join(self.root, 'empty'))

    def test_nothing_saved_yet(self):
        s = self.choose(['', '3440x1440', '', ''])
        setup.save_settings(s)
        self.assertFalse(os.path.exists(self.data))  # the app writes its files on its first run


class Update(Sandbox):
    """A second run: questions start from what the player has now."""

    def test_enter_keeps_everything(self):
        self.saved()
        self.installed(FFXIServer='plist.example', FFXIDats=self.dats)
        s = self.choose(['', '', '', ''])
        self.assertEqual((s['server'], s['resolution'], s['window_mode'], s['dats'], s['resized']),
                         ('old.example', (1920, 1080), 1, self.dats, False))
        self.assertIn('[old.example]', self.asked[0])
        before = self.read('settings.reg')
        setup.save_settings(s)
        self.assertEqual(self.read('settings.reg'), before)

    def test_none_drops_dats(self):
        self.installed(FFXIDats=self.dats)
        self.assertEqual(self.choose(['', '', '', 'none'])['dats'], '')

    def test_plist_when_nothing_saved(self):
        self.installed(FFXIServer='plist.example', FFXIResolution='2560x1440', FFXIWindowMode=2)
        s = self.choose(None)
        self.assertEqual((s['server'], s['resolution'], s['window_mode']), ('plist.example', (2560, 1440), 2))

    def test_new_answers_saved(self):
        self.saved(modern='fps_divisor=1\nui_aspect=0\n')
        s = self.choose(['new.example', '3440x1440', '2', ''])
        setup.save_settings(s)
        self.assertIn('server=new.example\nuser=bob\n', self.read('signin.cfg'))
        reg = self.read('settings.reg')
        for line in ('"0001"=dword:00000d70', '"0002"=dword:000005a0', '"0034"=dword:00000002',
                     '"0037"=dword:00000500', '"0038"=dword:000002d0'):
            self.assertIn(line + '\r\n', reg)
        self.assertEqual(reg.count('\n'), reg.count('\r\n'))  # still CRLF
        self.assertEqual(self.read('modern.cfg'), 'fps_divisor=1\nui_aspect=1.77778\n')

    def test_same_size_keeps_players_menus(self):
        self.saved(reg=SETTINGS_REG.replace('"0037"=dword:000003c0', '"0037"=dword:00000400'))
        setup.save_settings(self.choose(['', '', '4', '']))
        reg = self.read('settings.reg')
        self.assertIn('"0037"=dword:00000400', reg)
        self.assertIn('"0034"=dword:00000000', reg)

    def test_missing_key_added(self):
        self.saved(cfg='user=bob')  # no server, no newline at the end
        setup.save_settings(self.choose(['a.example', '', '', '']))
        self.assertEqual(self.read('signin.cfg'), 'user=bob\nserver=a.example\n')


if __name__ == '__main__':
    unittest.main()
