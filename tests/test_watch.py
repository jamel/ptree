#!/usr/bin/env python3
import fcntl
import os
import pty
import select
import signal
import struct
import subprocess
import sys
import termios
import time
import unittest
import uuid


CLEAR = b"\x1b[H\x1b[J"


class Watch:
    def __init__(self, case, args, terminal=False, rows=24, cols=80, env=None):
        self.buffer = b""
        self.terminal = terminal
        if terminal:
            self.fd, slave = pty.openpty()
            fcntl.ioctl(self.fd, termios.TIOCSWINSZ, struct.pack("HHHH", rows, cols, 0, 0))
            self.proc = subprocess.Popen(["./ptree", *args], stdout=slave,
                                         stderr=subprocess.PIPE, env=env)
            os.close(slave)
        else:
            self.proc = subprocess.Popen(["./ptree", *args], stdout=subprocess.PIPE,
                                         stderr=subprocess.PIPE, env=env)
            self.fd = self.proc.stdout.fileno()
        case.addCleanup(self.close)

    def close(self):
        if self.proc.poll() is None:
            self.proc.terminate()
        self.proc.wait(timeout=2)
        if self.terminal:
            os.close(self.fd)
        else:
            self.proc.stdout.close()
        self.proc.stderr.close()

    def read_until(self, marker, timeout=3):
        deadline = time.monotonic() + timeout
        while marker not in self.buffer:
            remaining = deadline - time.monotonic()
            if remaining <= 0 or not select.select([self.fd], [], [], remaining)[0]:
                raise AssertionError(f"timed out waiting for {marker!r}: {self.buffer!r}")
            try:
                chunk = os.read(self.fd, 65536)
            except OSError:
                chunk = b""
            if not chunk:
                raise AssertionError(f"watch exited: {self.proc.stderr.read()!r}")
            self.buffer += chunk
        end = self.buffer.index(marker) + len(marker)
        result, self.buffer = self.buffer[:end], self.buffer[end:]
        return result

    def frame(self):
        self.read_until(CLEAR)
        result = self.read_until(CLEAR)
        self.buffer = CLEAR + self.buffer
        return result[:-len(CLEAR)]

    def expect_frame(self, expected):
        deadline = time.monotonic() + 3
        while time.monotonic() < deadline:
            actual = self.frame()
            if actual == expected:
                return
        raise AssertionError(f"expected frame {expected!r}, got {actual!r}")


class WatchTests(unittest.TestCase):
    def process(self, args, **kwargs):
        proc = subprocess.Popen(args, **kwargs)

        def cleanup():
            if proc.poll() is None:
                proc.terminate()
            proc.wait(timeout=2)
            for stream in (proc.stdin, proc.stdout, proc.stderr):
                if stream:
                    stream.close()

        self.addCleanup(cleanup)
        return proc

    def test_default_interval_and_ctrl_c(self):
        """Refresh at the default interval and exit on Ctrl+C"""
        root = self.process(["sleep", "60"])
        watch = Watch(self, ["--watch", "-p", str(root.pid)])
        self.assertEqual(watch.read_until(b"\n", timeout=1), f"{root.pid}\n".encode())
        start = time.monotonic()
        self.assertEqual(watch.read_until(b"\n", timeout=7), f"{root.pid}\n".encode())
        self.assertGreaterEqual(time.monotonic() - start, 4.5)
        watch.proc.send_signal(signal.SIGINT)
        self.assertEqual(watch.proc.wait(timeout=1), -signal.SIGINT)

    def test_explicit_intervals_and_plain_redirected_output(self):
        """Use custom refresh intervals with plain redirected output"""
        root = self.process(["sleep", "60"])
        for args in (["--watch", "0.05"], ["--watch=0.05"]):
            with self.subTest(args=args):
                watch = Watch(self, [*args, "-c", "-p", str(root.pid)])
                for _ in range(2):
                    self.assertEqual(watch.read_until(b"\n"), f"{root.pid}\n".encode())

    def test_terminal_refreshes_children_and_clears_shorter_frames(self):
        """Update child PIDs and clear stale terminal rows"""
        root = self.process([
            sys.executable, "-u", "-c",
            "import subprocess, sys\n"
            "sys.stdin.readline()\n"
            "child = subprocess.Popen(['sleep', '60'])\n"
            "print(child.pid)\n"
            "sys.stdin.readline()\n"
            "child.terminate(); child.wait()\n"
            "print('stopped')\n"
            "sys.stdin.readline()\n",
        ], stdin=subprocess.PIPE, stdout=subprocess.PIPE, text=True)
        watch = Watch(self, ["--watch=0.05", "-p", str(root.pid)], terminal=True)
        root_frame = f"{root.pid}\r\n".encode()
        watch.expect_frame(root_frame)
        root.stdin.write("start\n")
        root.stdin.flush()
        child_pid = int(root.stdout.readline())
        child_active = True

        def cleanup_child():
            if child_active:
                self.stop_child(child_pid)

        self.addCleanup(cleanup_child)
        watch.expect_frame(root_frame + f"{child_pid}\r\n".encode())
        root.stdin.write("stop\n")
        root.stdin.flush()
        self.assertEqual(root.stdout.readline(), "stopped\n")
        child_active = False
        watch.expect_frame(root_frame)
        root.terminate()
        root.wait(timeout=2)
        watch.expect_frame(b"")
        self.assertIsNone(watch.proc.poll())

    @staticmethod
    def stop_child(pid):
        try:
            os.kill(pid, signal.SIGTERM)
        except ProcessLookupError:
            pass

    def test_filter_keeps_watching_when_matches_appear_and_disappear(self):
        """Keep watching as filter matches come and go"""
        token = "ptree-watch-" + uuid.uuid4().hex
        watch = Watch(self, ["--watch=0.05", "-f", f"^{token} 60$"], terminal=True)
        watch.expect_frame(b"")
        root = self.process(["bash", "-c", 'exec -a "$1" sleep 60', "bash", token])
        watch.expect_frame(f"{root.pid}\r\n".encode())
        root.terminate()
        root.wait(timeout=2)
        watch.expect_frame(b"")
        self.assertIsNone(watch.proc.poll())

    def test_terminal_wraps_and_clips_to_screen_size(self):
        """Wrap and clip output to fit the terminal"""
        root = self.process(["sleep", "60"])
        watch = Watch(self, ["--watch=0.05", "-c", "-p", str(root.pid)],
                      terminal=True, rows=2, cols=1)
        watch.expect_frame("\r\n".join(str(root.pid)[:2]).encode())
        fcntl.ioctl(watch.fd, termios.TIOCSWINSZ, struct.pack("HHHH", 3, 1, 0, 0))
        watch.expect_frame("\r\n".join(str(root.pid)[:3]).encode())

    def test_invalid_intervals(self):
        """Reject invalid refresh intervals"""
        for value in ("", "0", "-1", "abc", "1s", "nan", "inf", "1e100", "1e-100"):
            with self.subTest(interval=value):
                result = subprocess.run(["./ptree", f"--watch={value}", "-p", "1"],
                                        capture_output=True, timeout=2)
                self.assertEqual(result.returncode, 2)
                self.assertEqual(result.stdout, b"")
                self.assertIn(b"invalid watch interval", result.stderr)


if __name__ == "__main__":
    unittest.main()
