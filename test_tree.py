#!/usr/bin/env python3
import json
import os
import re
import signal
import subprocess
import sys
import time
import unittest
import uuid

from test_watch import Watch


def stop_fixture(signum, frame):
    raise SystemExit(0)


def fixture():
    # Keep every process's argv identical to exercise overlapping filter roots.
    read_fd, write_fd = os.pipe()
    child = os.fork()
    if child == 0:
        os.close(read_fd)
        grandchild = os.fork()
        if grandchild == 0:
            os.close(write_fd)
            signal.pause()
            os._exit(0)
        try:
            signal.signal(signal.SIGTERM, stop_fixture)
            os.write(write_fd, str(grandchild).encode())
            os.close(write_fd)
            signal.pause()
        finally:
            os.kill(grandchild, signal.SIGTERM)
            os.waitpid(grandchild, 0)
        os._exit(0)
    os.close(write_fd)
    grandchild = int(os.read(read_fd, 100))
    os.close(read_fd)
    sibling = os.fork()
    if sibling == 0:
        signal.pause()
        os._exit(0)
    try:
        signal.signal(signal.SIGTERM, stop_fixture)
        print(json.dumps([os.getpid(), child, grandchild, sibling]), flush=True)
        signal.pause()
    finally:
        for pid in (child, sibling):
            os.kill(pid, signal.SIGTERM)
        for pid in (child, sibling):
            os.waitpid(pid, 0)


class TreeTests(unittest.TestCase):
    def start_tree(self, token=None):
        token = token or "pidtree-tree-" + uuid.uuid4().hex
        args = [sys.executable, __file__, "--fixture", token]
        proc = subprocess.Popen(args, stdout=subprocess.PIPE, text=True,
                                start_new_session=True)

        def cleanup():
            proc.terminate()
            proc.wait(timeout=2)
            proc.stdout.close()

        self.addCleanup(cleanup)
        pids = json.loads(proc.stdout.readline())
        return pids, " ".join(args), token

    def rows(self, output):
        lines = output.splitlines()
        self.assertEqual(lines[0].split(), ["PID", "%CPU", "%MEM", "COMMAND"])
        rows = []
        for line in lines[1:]:
            match = re.fullmatch(r"\s*(\d+)\s+(\d+\.\d)\s+(\d+\.\d) (.*)", line)
            self.assertIsNotNone(match, line)
            rows.append((int(match[1]), float(match[2]), float(match[3]), match[4]))
        return rows

    def run_tree(self, *args):
        result = subprocess.run(["./pidtree", *args], capture_output=True, text=True,
                                timeout=3)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(result.stderr, "")
        return self.rows(result.stdout)

    def test_pid_tree_has_depth_first_branches_and_full_commands(self):
        pids, command, _ = self.start_tree()
        for flag in ("-t", "--tree"):
            with self.subTest(flag=flag):
                rows = self.run_tree(flag, "-p", str(pids[0]))
                self.assertEqual([row[0] for row in rows], pids)
                self.assertEqual([row[3] for row in rows], [
                    command, " \\_ " + command, " |   \\_ " + command, " \\_ " + command,
                ])

    def test_filter_forest_keeps_overlapping_matches_under_their_parents(self):
        token = "pidtree-tree-" + uuid.uuid4().hex
        first, _, _ = self.start_tree(token)
        second, _, _ = self.start_tree(token)
        rows = self.run_tree("--tree", "-f", token)
        self.assertEqual([row[0] for row in rows], first + second)
        self.assertFalse(rows[0][3].startswith(" "))
        self.assertFalse(rows[4][3].startswith(" "))

    def test_long_commands_and_control_characters_stay_on_one_line(self):
        pids, command, _ = self.start_tree("x" * 5000 + "\n\t\x1btail")
        rows = self.run_tree("-t", "-p", str(pids[0]))
        self.assertEqual(len(rows), 4)
        self.assertEqual(rows[0][3], command.replace("\n", "?").replace("\t", "?").replace("\x1b", "?"))

    def test_cpu_and_memory_percentages_are_similar_to_ps(self):
        proc = subprocess.Popen([sys.executable, "-c",
                                 "import time; data = bytearray(16000000); "
                                 "exec('while True: pass')"])
        self.addCleanup(proc.wait, timeout=2)
        self.addCleanup(proc.terminate)
        time.sleep(1.1)
        row = self.run_tree("-t", "-p", str(proc.pid))[0]
        expected = subprocess.check_output(
            ["ps", "-o", "pcpu=,pmem=", "-p", str(proc.pid)], text=True).split()
        self.assertGreater(row[1], 0)
        self.assertAlmostEqual(row[1], float(expected[0]), delta=20)
        self.assertAlmostEqual(row[2], float(expected[1]), delta=0.1)

    def test_empty_command_line_uses_name_with_parentheses(self):
        proc = subprocess.Popen([
            sys.executable, "-u", "-c",
            "import os, sys\n"
            "child = os.fork()\n"
            "if child == 0:\n"
            "    with open('/proc/self/comm', 'w') as f: f.write('tree ) (name')\n"
            "    os._exit(0)\n"
            "os.waitid(os.P_PID, child, os.WEXITED | os.WNOWAIT)\n"
            "print(child)\n"
            "sys.stdin.readline()\n"
            "os.waitpid(child, 0)\n",
        ], stdin=subprocess.PIPE, stdout=subprocess.PIPE, text=True)

        def cleanup():
            proc.stdin.close()
            proc.wait(timeout=2)
            proc.stdout.close()

        self.addCleanup(cleanup)
        child = int(proc.stdout.readline())
        rows = self.run_tree("-t", "-p", str(child))
        self.assertEqual(len(rows), 1)
        self.assertEqual(rows[0][0], child)
        self.assertEqual(rows[0][3], "[tree ) (name]")

    def test_watch_renders_tree_frames(self):
        pids, command, _ = self.start_tree()
        watch = Watch(self, ["--watch=0.05", "-t", "-p", str(pids[0])],
                      terminal=True, rows=20, cols=1000)
        rows = self.rows(watch.frame().decode())
        self.assertEqual([row[0] for row in rows], pids)
        self.assertEqual(rows[2][3], " |   \\_ " + command)

    def test_watch_cpu_tracks_each_interval_when_process_stops_and_resumes(self):
        proc = subprocess.Popen([
            sys.executable, "-u", "-c",
            "import time\n"
            "end = time.process_time() + 0.2\n"
            "while time.process_time() < end: pass\n"
            "print('ready')\n"
            "while True: pass\n",
        ], stdout=subprocess.PIPE, text=True)
        self.addCleanup(proc.wait, timeout=2)
        self.addCleanup(proc.terminate)
        self.addCleanup(proc.send_signal, signal.SIGCONT)
        self.addCleanup(proc.stdout.close)
        self.assertEqual(proc.stdout.readline(), "ready\n")

        for terminal in (False, True):
            with self.subTest(terminal=terminal):
                proc.send_signal(signal.SIGCONT)
                watch = Watch(self, ["--watch=0.05", "-t", "-p", str(proc.pid)],
                              terminal=terminal, cols=1000)

                def cpu():
                    if terminal:
                        frame = watch.frame()
                    else:
                        frame = watch.read_until(b"\n") + watch.read_until(b"\n")
                    return self.rows(frame.decode())[0][1]

                self.assertGreater(cpu(), 0)
                proc.send_signal(signal.SIGSTOP)
                os.waitpid(proc.pid, os.WUNTRACED)
                # Allow a frame already in the pipe and one partial interval.
                stopped = [cpu() for _ in range(4)]
                self.assertEqual(stopped[-1], 0.0, stopped)
                proc.send_signal(signal.SIGCONT)
                resumed = [cpu() for _ in range(4)]
                self.assertGreater(resumed[-1], 0.0, resumed)

    def test_watch_tree_refreshes_matches_after_empty_frames(self):
        token = "pidtree-tree-" + uuid.uuid4().hex
        watch = Watch(self, ["--watch=0.05", "-t", "-f", f"^{token} 60$"],
                      terminal=True, cols=1000)
        watch.expect_frame(b"")
        for _ in range(2):
            proc = subprocess.Popen([token, "60"], executable="/usr/bin/sleep")
            self.addCleanup(proc.wait, timeout=2)
            self.addCleanup(proc.terminate)
            frame = b""
            for _ in range(5):
                frame = watch.frame()
                if frame:
                    break
            rows = self.rows(frame.decode())
            self.assertEqual([row[0] for row in rows], [proc.pid])
            self.assertEqual(rows[0][3], token + " 60")
            proc.terminate()
            proc.wait(timeout=2)
            watch.expect_frame(b"")

    def test_watch_wraps_complete_unicode_characters_by_display_width(self):
        proc = subprocess.Popen(["éé界e\u0301", "60"], executable="/usr/bin/sleep")
        self.addCleanup(proc.wait, timeout=2)
        self.addCleanup(proc.terminate)
        for cols, suffix in ((19, "é\r\né界e\u0301 60\r\n"),
                             (2, "éé\r\n界\r\ne\u0301 \r\n60\r\n")):
            with self.subTest(cols=cols):
                watch = Watch(self, ["--watch=0.05", "-t", "-p", str(proc.pid)],
                              terminal=True, rows=50, cols=cols,
                              env={**os.environ, "LC_ALL": "C.UTF-8"})
                frame = watch.frame().decode("utf-8")
                self.assertTrue(frame.endswith(suffix), frame)

    def test_empty_selection_and_conflicting_output_modes(self):
        for args, status in ((["-t", "-f", "^pidtree-tree-missing$"], 1),
                             (["-ct", "-p", str(os.getpid())], 2),
                             (["--tree", "--comma", "-p", str(os.getpid())], 2)):
            with self.subTest(args=args):
                result = subprocess.run(["./pidtree", *args], capture_output=True,
                                        timeout=3)
                self.assertEqual(result.returncode, status)
                self.assertEqual(result.stdout, b"")


if __name__ == "__main__":
    if "--fixture" in sys.argv:
        fixture()
    else:
        unittest.main()
