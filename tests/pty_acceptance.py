#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Exercise the real input loop and inspect terminal cells, not ANSI snapshots."""
import codecs
import fcntl
import os
import pathlib
import pty
import select
import struct
import subprocess
import sys
import tempfile
import termios
import time
import unittest

import pyte

BINARY = str(pathlib.Path(sys.argv.pop(1)).resolve())


class Terminal:
    def __init__(self, config, width=100, height=30, args=()):
        self.directory = tempfile.TemporaryDirectory()
        self.config = pathlib.Path(self.directory.name) / "btop.conf"
        self.config.write_text('update_ms = 100\nclock_format = ""\n' + config)
        self.start(width, height, args)

    def start(self, width=100, height=30, args=()):
        self.master, slave = pty.openpty()
        fcntl.ioctl(slave, termios.TIOCSWINSZ, struct.pack("HHHH", height, width, 0, 0))
        self.original = termios.tcgetattr(slave)
        self.slave = slave
        self.screen = pyte.Screen(width, height)
        self.stream = pyte.Stream(self.screen)
        self.decoder = codecs.getincrementaldecoder("utf-8")("replace")
        self.output = ""
        self.pending = ""
        self.process = subprocess.Popen(
            [BINARY, "--config", str(self.config), *args], stdin=slave,
            stdout=slave, stderr=subprocess.PIPE,
            env={**os.environ, "TERM": "xterm-256color", "LANG": "C.UTF-8", "XDG_STATE_HOME": self.directory.name},
        )

    def read(self, duration=0.15):
        until = time.monotonic() + duration
        while time.monotonic() < until:
            if select.select([self.master], [], [], 0.03)[0]:
                chunk = os.read(self.master, 65536)
                text = self.decoder.decode(chunk)
                self.output += text
                self.pending += text
                # Observe only completed synchronized terminal frames.
                while "\x1b[?2026l" in self.pending:
                    frame, self.pending = self.pending.split("\x1b[?2026l", 1)
                    self.stream.feed(frame + "\x1b[?2026l")
        return "\n".join(self.screen.display)

    def wait_for(self, text):
        until = time.monotonic() + 8
        while time.monotonic() < until:
            display = self.read()
            if text in display:
                return display
            if self.process.poll() is not None:
                break
        raise AssertionError(f"Missing {text!r}:\n{self.read()}\nstderr: " +
                             (self.process.stderr.read().decode() if self.process.poll() is not None else "running"))

    def key(self, key):
        os.write(self.master, key.encode())
        return self.read()

    def relaunch(self):
        self.key("q")
        self.process.wait(5)
        if self.process.returncode != 0:
            raise AssertionError(self.process.stderr.read().decode())
        os.close(self.master)
        os.close(self.slave)
        self.process.stderr.close()
        self.start()

    def close(self):
        if self.process.poll() is None:
            self.key("\x1b")
            self.key("q")
            try:
                self.process.wait(2)
            except subprocess.TimeoutExpired:
                self.process.terminate()
                self.process.wait(3)
        os.close(self.master)
        os.close(self.slave)
        self.process.stderr.close()
        self.directory.cleanup()


class Acceptance(unittest.TestCase):
    def terminal(self, config, **kwargs):
        terminal = Terminal(config, **kwargs)
        self.addCleanup(terminal.close)
        return terminal

    def test_absent_selectors_restore_terminal_and_fail_even_with_net_hidden(self):
        for args, config in [(('--iface', 'missing-btop-interface'), ''),
                             ((), 'net_iface = "missing-btop-interface"\n')]:
            with self.subTest(args=args):
                terminal = self.terminal('shown_boxes = "cpu"\n' + config, args=args)
                terminal.process.wait(8)
                self.assertNotEqual(terminal.process.returncode, 0)
                self.assertIn('Interface "missing-btop-interface" not found', terminal.process.stderr.read().decode())
                self.assertEqual(termios.tcgetattr(terminal.slave), terminal.original)

    def test_editor_and_compact_at_minimum_narrow_and_wide_sizes(self):
        for width, height in [(36, 6), (80, 24), (160, 40)]:
            with self.subTest(width=width):
                terminal = self.terminal('shown_boxes = "net"\niface_view = "compact"\n', width=width, height=height)
                terminal.wait_for('page 1/')
                terminal.key('I')
                terminal.wait_for('Include:')
                terminal.wait_for('Exclude:')
                terminal.key('[')
                terminal.key('\t')
                terminal.key('(')
                terminal.key('\r')
                terminal.wait_for('Invalid: include exclude')
                terminal.key('\x1b')
                terminal.wait_for('page 1/')
                terminal.key('I')
                terminal.wait_for('Include:')
                for char in '^missing$':
                    terminal.key(char)
                terminal.key('\r')
                terminal.wait_for('No interfaces')
                self.assertIn('del', terminal.read())
                terminal.key('\x1b[3~')
                terminal.wait_for('page 1/')
                terminal.key('\r')
                self.assertNotIn('page 1/', terminal.read())
                terminal.key('q')
                terminal.process.wait(5)
                self.assertEqual(terminal.process.returncode, 0)

    def assert_view(self, terminal, view):
        if view == "compact":
            terminal.wait_for('page 1/')
        else:
            terminal.wait_for('compact')
            self.assertNotIn('page 1/', terminal.read())

    def test_last_view_is_restored(self):
        for initial, final in [('detail', 'compact'), ('compact', 'detail')]:
            with self.subTest(initial=initial):
                terminal = self.terminal(f'shown_boxes = "net"\niface_view = "{initial}"\n')
                self.assert_view(terminal, initial)
                # Seed a complete config so a view-only change must request a write.
                terminal.relaunch()
                self.assert_view(terminal, initial)
                terminal.key('v')
                self.assert_view(terminal, final)
                terminal.relaunch()
                self.assert_view(terminal, final)
                self.assertIn(f'iface_view = "{final}"', terminal.config.read_text())

    def test_filter_confirmation_and_cancelled_drafts_are_remembered(self):
        for draft in ['confirm', 'cancel', 'invalid']:
            with self.subTest(draft=draft):
                terminal = self.terminal('shown_boxes = "net"\niface_view = "detail"\n')
                self.assert_view(terminal, 'detail')
                terminal.key('I')
                terminal.wait_for('Include:')
                if draft == 'invalid':
                    terminal.key('[')
                    terminal.key('\r')
                    terminal.wait_for('Invalid: include')
                terminal.key('\r' if draft == 'confirm' else '\x1b')
                expected = 'compact' if draft == 'confirm' else 'detail'
                self.assert_view(terminal, expected)
                terminal.relaunch()
                self.assert_view(terminal, expected)

    def test_reload_saves_the_active_view(self):
        terminal = self.terminal('shown_boxes = "net"\niface_view = "compact"\n')
        self.assert_view(terminal, 'compact')
        terminal.config.write_text('shown_boxes = "net"\niface_view = "detail"\niface_sorting = "alnum"\n')
        terminal.process.send_signal(__import__('signal').SIGUSR2)
        terminal.wait_for('alnum')
        self.assert_view(terminal, 'compact')
        terminal.relaunch()
        self.assert_view(terminal, 'compact')

    def test_disabled_saving_preserves_configured_view(self):
        terminal = self.terminal('shown_boxes = "net"\niface_view = "detail"\nsave_config_on_exit = false\n')
        original = terminal.config.read_text()
        self.assert_view(terminal, 'detail')
        terminal.key('v')
        self.assert_view(terminal, 'compact')
        terminal.relaunch()
        self.assertEqual(terminal.config.read_text(), original)
        self.assert_view(terminal, 'detail')

    def test_hidden_net_preserves_last_active_or_uninitialized_view(self):
        for shown in [False, True]:
            with self.subTest(shown=shown):
                boxes = 'net proc' if shown else 'proc'
                terminal = self.terminal(f'shown_boxes = "{boxes}"\niface_view = "compact"\n')
                terminal.wait_for('Pid:')
                if shown:
                    self.assert_view(terminal, 'compact')
                    terminal.key('v')
                    self.assert_view(terminal, 'detail')
                    terminal.key('3')
                terminal.relaunch()
                expected = 'detail' if shown else 'compact'
                self.assertIn(f'iface_view = "{expected}"', terminal.config.read_text())
                terminal.wait_for('Pid:')
                terminal.key('3')
                self.assert_view(terminal, expected)

    def test_process_confirmation_owns_delete_and_outside_click_cancels(self):
        terminal = self.terminal('shown_boxes = "net proc"\niface_include = "lo|eth"\n', width=120, height=30)
        terminal.wait_for('net')
        terminal.key('I')
        terminal.wait_for('Include:')
        terminal.key('\x1b[<0;67;1M')
        self.assertNotIn('Include:', terminal.read())
        terminal.key('f')
        for char in 'btop':
            terminal.key(char)
        terminal.key('\r')
        terminal.wait_for('btop del')
        terminal.key('\x1b[3~')
        self.assertNotIn('btop del', terminal.read())

    def test_mouse_hints_clear_their_own_filter(self):
        terminal = self.terminal('shown_boxes = "net proc"\niface_include = "lo|eth"\nproc_filter = "btop"\n', width=120, height=30, args=("--filter", "btop"))
        terminal.wait_for('btop del')

        def click_hint(text, offset):
            terminal.wait_for(text)
            for row, line in enumerate(terminal.screen.display):
                if text in line:
                    column = line.index(text) + offset + 1
                    terminal.key(f'\x1b[<0;{column};{row + 1}M')
                    return
            self.fail('Missing mouse hint')

        click_hint('btop del', 5)
        terminal.wait_for('I del')
        self.assertNotIn('btop del', terminal.read())
        click_hint('I del', 2)
        self.assertNotIn('I del', terminal.read())
        terminal.key('/')
        for char in 'btop':
            terminal.key(char)
        terminal.key('\r')
        terminal.wait_for('btop del')
        terminal.key('I')
        terminal.wait_for('Include:')
        terminal.key('l')
        terminal.key('o')
        terminal.key('\r')
        terminal.wait_for('I del')
        self.assertNotIn('btop del', terminal.read())
        click_hint('I del', 2)
        terminal.wait_for('btop del')

    def test_fixed_ceilings_cli_precedence_and_reload(self):
        terminal = self.terminal('shown_boxes = "net"\niface_view = "compact"\nnet_auto = false\nnet_download = 8\nnet_upload = 16\nnet_iface = "missing"\n',
                                 width=160, height=30, args=("--iface", "lo"))
        display = terminal.wait_for('D 1.00 MiB')
        self.assertIn('U 2.00 MiB', display)
        self.assertIn('lo', display)
        terminal.config.write_text('shown_boxes = "net"\niface_view = "detail"\niface_include = "^missing$"\nnet_iface = "missing"\n')
        terminal.process.send_signal(__import__('signal').SIGUSR2)
        display = terminal.wait_for('page 1/')
        self.assertIn('lo', display)
        terminal.key('I')
        terminal.wait_for('Include:')
        # Click the exclude field, then make only that field invalid.
        terminal.key('\x1b[<0;60;15M')
        terminal.key('[')
        terminal.key('\r')
        terminal.wait_for('Invalid: exclude')

    def test_default_config_documents_patterns_and_omits_runtime_state(self):
        result = subprocess.run([BINARY, '--default-config'], capture_output=True, text=True, check=True)
        self.assertIn('POSIX extended regular expression', result.stdout)
        self.assertIn('Empty or .* includes all', result.stdout)
        self.assertIn('Empty excludes none', result.stdout)
        for name in ['iface_compact_view_active', 'confirmed_interfaces', 'iface_index', 'iface_page', 'clear_owner']:
            self.assertNotIn(name + ' =', result.stdout)


if __name__ == '__main__':
    unittest.main()
