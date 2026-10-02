"""Exercise the real CLI and controlling-terminal prompts without hardware."""

import errno
import fcntl
import os
from pathlib import Path
import pty
import select
import signal
import subprocess
import termios
import time
import unittest

ROOT = Path(__file__).resolve().parents[1]
BINARY = ROOT / "build/dell-rf-cli-test"


def controlling_terminal():
    os.setsid()
    fcntl.ioctl(0, termios.TIOCSCTTY, 0)


class Terminal:
    def __init__(self, arguments, scenario=""):
        self.master, slave = pty.openpty()
        environment = os.environ.copy()
        environment["DRF_TEST_SCENARIO"] = scenario
        self.process = subprocess.Popen(
            [str(BINARY), *arguments],
            stdin=slave,
            stdout=slave,
            stderr=slave,
            env=environment,
            preexec_fn=controlling_terminal,
        )
        os.close(slave)
        self.output = b""

    def read_until(self, marker, timeout=5):
        deadline = time.monotonic() + timeout
        while marker not in self.output:
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                raise AssertionError(f"Timed out waiting for {marker!r}")
            ready, _, _ = select.select([self.master], [], [], remaining)
            if not ready:
                continue
            try:
                data = os.read(self.master, 4096)
            except OSError as error:
                if error.errno == errno.EIO:
                    break
                raise
            if not data:
                break
            self.output += data
        if marker not in self.output:
            raise AssertionError(self.output.decode(errors="replace"))

    def finish(self, timeout=5):
        self.process.wait(timeout=timeout)
        while select.select([self.master], [], [], 0)[0]:
            try:
                data = os.read(self.master, 4096)
            except OSError as error:
                if error.errno == errno.EIO:
                    break
                raise
            if not data:
                break
            self.output += data
        return self.process.returncode, self.output.decode(errors="replace")

    def close(self):
        if self.process.poll() is None:
            self.process.kill()
            self.process.wait(timeout=5)
        os.close(self.master)


@unittest.skipUnless(BINARY.exists(), "build the CLI mock with make check")
class CLITests(unittest.TestCase):
    def terminal(self, arguments, scenario=""):
        terminal = Terminal(arguments, scenario)
        self.addCleanup(terminal.close)
        return terminal

    def test_confirmed_pairing_for_both_device_types(self):
        for kind, model in [("mouse", "MS3121W"), ("keyboard", "KB3121W")]:
            with self.subTest(kind=kind):
                terminal = self.terminal(["pair", kind])
                terminal.read_until(b"anything else cancels: ")
                self.assertIn(model.encode(), terminal.output)
                self.assertIn(b"for 30 seconds", terminal.output)
                self.assertNotIn(b"MOCK_COMMIT", terminal.output)
                os.write(terminal.master, b"pair\n")
                status, output = terminal.finish()
                self.assertEqual(status, 0, output)
                self.assertIn(f"Paired {model}", output)
                self.assertEqual(output.count("MOCK_COMMIT"), 1)

    def test_slot_confirmation_and_decline(self):
        for answer, expected in [
            (b"unpair 1\n", 0),
            (b"unpair 2\n", 1),
            (b"no\n", 1),
            (b"unpair 1" + b" " * 100 + b"\n", 1),
        ]:
            with self.subTest(answer=answer):
                terminal = self.terminal(["unpair", "1"])
                terminal.read_until(b"anything else cancels: ")
                self.assertIn(b"MS3121W", terminal.output)
                os.write(terminal.master, answer)
                status, output = terminal.finish()
                self.assertEqual(status, expected, output)
                self.assertEqual("MOCK_COMMIT" in output, expected == 0)

    def test_confirmation_deadline(self):
        terminal = self.terminal(["pair", "mouse"], "deadline")
        terminal.read_until(b"anything else cancels: ")
        status, output = terminal.finish(timeout=3)
        self.assertEqual(status, 1, output)
        self.assertIn("window expired", output)
        self.assertNotIn("MOCK_COMMIT", output)

    def test_signals_during_confirmation(self):
        for sig in [signal.SIGINT, signal.SIGTERM]:
            with self.subTest(signal=sig):
                terminal = self.terminal(["unpair", "1"])
                terminal.read_until(b"anything else cancels: ")
                os.kill(terminal.process.pid, sig)
                status, output = terminal.finish()
                self.assertEqual(status, 128 + sig, output)
                self.assertNotIn("MOCK_COMMIT", output)

    def test_input_queued_before_prompt_cannot_approve(self):
        terminal = self.terminal(["pair", "mouse"], "stale")
        terminal.read_until(b"Ctrl+C ends the local search")
        os.write(terminal.master, b"pair\n")
        terminal.read_until(b"anything else cancels: ")
        os.write(terminal.master, b"no\n")
        status, output = terminal.finish()
        self.assertEqual(status, 1, output)
        self.assertNotIn("MOCK_COMMIT", output)

    def test_multiple_receivers_list_paths_before_any_action(self):
        for args in [
            ["slots"],
            ["battery"],
            ["pair", "mouse"],
            ["pair", "keyboard"],
            ["unpair", "1"],
        ]:
            with self.subTest(arguments=args):
                terminal = self.terminal(args, "multiple")
                status, output = terminal.finish()
                self.assertEqual(status, 1, output)
                self.assertIn("Multiple validated receivers", output)
                self.assertIn("Compatible receiver paths:", output)
                for path in ["/dev/hidraw2", "/dev/hidraw5", "/dev/hidraw8"]:
                    self.assertIn(f"  {path}\r\n", output)
                self.assertNotIn("MOCK_ARGS", output)
                self.assertNotIn("MOCK_COMMIT", output)

    def test_battery_is_read_only_and_reports_available_values(self):
        result = subprocess.run(
            [str(BINARY), "battery"],
            capture_output=True,
            text=True,
            start_new_session=True,
            timeout=3,
        )
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("MOCK_ARGS battery /dev/hidraw-test", result.stdout)
        self.assertIn("KB3121W", result.stdout)
        self.assertIn("73%", result.stdout)
        self.assertIn("MS3121W", result.stdout)
        self.assertIn("unavailable", result.stdout)
        self.assertNotIn("MOCK_COMMIT", result.stdout)

    def test_explicit_path_bypasses_ambiguous_selection(self):
        for args in [["pair", "mouse"], ["unpair", "1"]]:
            with self.subTest(arguments=args):
                terminal = self.terminal([*args, "/dev/hidraw5"], "multiple")
                terminal.read_until(b"anything else cancels: ")
                os.write(terminal.master, b"no\n")
                status, output = terminal.finish()
                self.assertEqual(status, 1, output)
                self.assertIn("MOCK_ARGS", output)
                self.assertIn("/dev/hidraw5", output)
                self.assertNotIn("Compatible receiver paths:", output)
                self.assertNotIn("MOCK_COMMIT", output)

    def test_receiver_listing_failure_is_reported(self):
        terminal = self.terminal(["slots"], "multiple-list-error")
        status, output = terminal.finish()
        self.assertEqual(status, 1, output)
        self.assertIn("Could not list compatible receiver paths:", output)
        self.assertNotIn("MOCK_ARGS", output)
        self.assertNotIn("MOCK_COMMIT", output)

    def test_piped_approval_is_refused_without_a_controlling_terminal(self):
        for args in [["pair", "mouse"], ["unpair", "1"]]:
            result = subprocess.run(
                [str(BINARY), *args],
                input="pair\nunpair 1\n",
                capture_output=True,
                text=True,
                start_new_session=True,
                timeout=3,
            )
            self.assertEqual(result.returncode, 1, result.stderr)
            self.assertIn("interactive controlling terminal", result.stderr)
            self.assertNotIn("MOCK_ARGS", result.stdout)

    def test_invalid_arguments_are_rejected_before_receiver_access(self):
        cases = [
            ["pair"],
            ["unpair"],
            ["pair", "trackpad"],
            ["pair", "mouse", "--yes"],
            ["unpair", "1", "--yes"],
            ["unpair", "1", "--timeout", "1"],
            ["unpair", "1", "a", "b"],
            ["pair", "mouse", "--timeout", "30"],
            ["battery", "--yes"],
            ["battery", "/dev/hidraw2", "extra"],
        ]
        cases += [["unpair", value] for value in ["0", "7", "-1", "", "a"]]
        for args in cases:
            with self.subTest(arguments=args):
                result = subprocess.run(
                    [str(BINARY), *args],
                    capture_output=True,
                    text=True,
                    start_new_session=True,
                    timeout=3,
                )
                self.assertEqual(result.returncode, 2, result.stderr)
                self.assertNotIn("MOCK_ARGS", result.stdout)

    def test_uncertain_outcome_and_already_paired_messages(self):
        terminal = self.terminal(["pair", "mouse"], "uncertain")
        terminal.read_until(b"anything else cancels: ")
        os.write(terminal.master, b"pair\n")
        status, output = terminal.finish()
        self.assertEqual(status, 1, output)
        self.assertIn("outcome is unverified", output)
        self.assertIn("may still take effect", output.replace("\r\n", " "))
        self.assertNotIn("Paired MS3121W", output)
        terminal = self.terminal(["pair", "mouse"], "already")
        status, output = terminal.finish()
        self.assertEqual(status, 1, output)
        self.assertIn("already paired in slot 1", output)
        self.assertNotIn("MOCK_COMMIT", output)


if __name__ == "__main__":
    unittest.main()
