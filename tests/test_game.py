#!/usr/bin/env python3
import os
import pty
import re
import select
import shutil
import signal
import subprocess
import tempfile
import termios
import time
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parent.parent
ANSI = re.compile(rb"\x1b\[[0-?]*[ -/]*[@-~]")
CLEAR = b"\x1b[H\x1b[2J"


def wait_for(predicate, timeout=3):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        if predicate():
            return
        time.sleep(0.01)
    raise AssertionError("Timed out waiting for terminal state")


def frames_from(output):
    frames = []
    for part in output.split(CLEAR)[1:]:
        lines = ANSI.sub(b"", part).replace(b"\r\n", b"\n").split(b"\n")
        if len(lines) < 28 or not lines[0].startswith(b"Score: "):
            continue
        rows = lines[2:27]
        if len(rows) == 25 and all(len(row) == 40 for row in rows):
            frames.append(rows)
    return frames


class FlappyBirdTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.directory = tempfile.TemporaryDirectory()
        cls.binary = Path(cls.directory.name) / "flappy"
        cls.core_test = Path(cls.directory.name) / "core-test"
        compiler = os.environ.get("CC") or shutil.which("clang") or "cc"
        flags = ["-std=c11", "-O1", "-g", "-Wall", "-Wextra", "-Wpedantic", "-Werror"]
        if os.environ.get("SANITIZE") == "1":
            flags.extend(["-fsanitize=address,undefined", "-fno-omit-frame-pointer"])
        for source, output in [
            (ROOT / "proj1_20201574.c", cls.binary),
            (ROOT / "tests" / "test_game.c", cls.core_test),
        ]:
            subprocess.run([compiler, *flags, str(source), "-o", str(output)], check=True)
        cls.env = dict(os.environ, ASAN_OPTIONS="detect_leaks=0")

    @classmethod
    def tearDownClass(cls):
        cls.directory.cleanup()

    def start_game(self):
        master, slave = pty.openpty()
        original = termios.tcgetattr(slave)
        process = subprocess.Popen(
            [str(self.binary)],
            stdin=slave,
            stdout=slave,
            stderr=subprocess.PIPE,
            env=self.env,
        )
        self.addCleanup(os.close, master)
        self.addCleanup(os.close, slave)

        def stop():
            if process.poll() is None:
                process.send_signal(signal.SIGINT)
                try:
                    process.wait(timeout=2)
                except subprocess.TimeoutExpired:
                    process.kill()
                    process.wait()
            process.stderr.close()

        self.addCleanup(stop)
        wait_for(lambda: not (termios.tcgetattr(slave)[3] & termios.ICANON))
        return process, master, slave, original

    def read_frames(self, master, count):
        output = bytearray()
        deadline = time.monotonic() + 3
        while time.monotonic() < deadline:
            if len(frames_from(output)) >= count:
                return output, frames_from(output)
            if select.select([master], [], [], 0.1)[0]:
                try:
                    output.extend(os.read(master, 65536))
                except OSError:
                    break
        self.fail(f"Expected {count} complete frames, received {len(frames_from(output))}")

    def test_core_game_rules(self):
        subprocess.run([str(self.core_test)], check=True, env=self.env)

    def test_non_terminal_input_is_rejected(self):
        result = subprocess.run(
            [str(self.binary)],
            stdin=subprocess.DEVNULL,
            stdout=subprocess.DEVNULL,
            stderr=subprocess.PIPE,
            env=self.env,
            timeout=3,
        )
        self.assertNotEqual(result.returncode, 0)
        self.assertIn(b"standard input must be a terminal", result.stderr)
        self.assertNotIn(b"AddressSanitizer", result.stderr)

    def test_initial_board_and_gravity_ignore_unrelated_keys(self):
        process, master, _, _ = self.start_game()
        os.write(master, b"xxxxxxx")
        output, frames = self.read_frames(master, 2)
        self.assertNotIn(b"\x00", output)
        self.assertNotIn(b"Call Do function!", output)
        self.assertEqual(frames[0][12][9:10], b"@")
        self.assertEqual(frames[1][13][9:10], b"@")
        self.assertIsNone(process.poll())

    def test_queued_flaps_are_not_discarded(self):
        process, master, _, _ = self.start_game()
        os.write(master, b"ww")
        _, frames = self.read_frames(master, 2)
        self.assertEqual(frames[1][8][9:10], b"@")
        self.assertIsNone(process.poll())

    def test_rapid_flaps_end_game_safely_with_final_score(self):
        process, master, slave, original = self.start_game()
        os.write(master, b"wwwwwww")
        output = bytearray()
        deadline = time.monotonic() + 3
        while process.poll() is None and time.monotonic() < deadline:
            if select.select([master], [], [], 0.1)[0]:
                output.extend(os.read(master, 65536))
        process.wait(timeout=1)
        while select.select([master], [], [], 0)[0]:
            output.extend(os.read(master, 65536))
        self.assertEqual(process.returncode, 0)
        self.assertIn(b"game over!", output)
        self.assertIn(b"Score: 0", output)
        self.assertEqual(termios.tcgetattr(slave), original)
        self.assertNotIn(b"AddressSanitizer", process.stderr.read())

    def test_interrupt_restores_terminal_settings(self):
        process, _, slave, original = self.start_game()
        self.assertFalse(termios.tcgetattr(slave)[3] & termios.ECHO)
        process.send_signal(signal.SIGINT)
        process.wait(timeout=3)
        self.assertEqual(process.returncode, 128 + signal.SIGINT)
        self.assertEqual(termios.tcgetattr(slave), original)
        self.assertNotIn(b"AddressSanitizer", process.stderr.read())

    def test_suspend_and_resume_restore_terminal_settings(self):
        prompt = b"FLAPPY_TEST_PROMPT> "
        pid, master = pty.fork()
        if pid == 0:
            env = dict(self.env, PS1=prompt.decode())
            os.execve("/bin/bash", ["bash", "--noprofile", "--norc", "-i"], env)

        def stop_shell():
            try:
                os.kill(pid, signal.SIGKILL)
            except ProcessLookupError:
                pass
            os.waitpid(pid, 0)
            os.close(master)

        self.addCleanup(stop_shell)
        output = bytearray()

        def pump_and_check(predicate):
            if select.select([master], [], [], 0.01)[0]:
                output.extend(os.read(master, 65536))
            return predicate()

        wait_for(lambda: pump_and_check(lambda: prompt in output))
        original = termios.tcgetattr(master)
        os.write(master, os.fsencode(str(self.binary)) + b"\n")
        wait_for(lambda: pump_and_check(
            lambda: not (termios.tcgetattr(master)[3] & termios.ICANON)
        ))

        os.write(master, b"\x1a")
        wait_for(lambda: pump_and_check(lambda: output.count(prompt) >= 2))
        self.assertEqual(termios.tcgetattr(master), original)

        os.write(master, b"fg\n")
        wait_for(lambda: pump_and_check(
            lambda: not (termios.tcgetattr(master)[3] & termios.ICANON)
        ))
        os.write(master, b"\x03")
        wait_for(lambda: pump_and_check(lambda: output.count(prompt) >= 3))
        self.assertEqual(termios.tcgetattr(master), original)
        self.assertNotIn(b"AddressSanitizer", output)


if __name__ == "__main__":
    unittest.main()
