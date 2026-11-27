#!/usr/bin/env python3
import json
import os
import re
import shlex
import signal
import subprocess
import sys
import tempfile
import time
import unittest
import uuid
from pathlib import Path

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
    def setUp(self):
        self.env = {**os.environ, "TERM": "xterm", "NO_COLOR": ""}
        with open("/proc/sys/kernel/pid_max") as file:
            self.pid_width = max(3, len(str(int(file.read()) - 1)))
        self.max_cpu = os.sysconf("SC_NPROCESSORS_ONLN") * 100
        self.cpu_width = len(f"{self.max_cpu:.1f}")
        self.header = (f"{'PID':>{self.pid_width}} {'%CPU':>{self.cpu_width}} "
                       f"{'%MEM':>5} COMMAND")

    def plain(self, output):
        output = re.sub(r"\x1b\[[0-9;]*m", "", output)
        self.assertNotIn("\x1b", output)
        return output

    def start_tree(self, token=None):
        token = token or "ptree-tree-" + uuid.uuid4().hex
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
        lines = self.plain(output).splitlines()
        self.assertEqual(lines[0], self.header)
        rows = []
        for line in lines[1:]:
            match = re.fullmatch(r"\s*(\d+)\s+(\d+\.\d)\s+(\d+\.\d) (.*)", line)
            self.assertIsNotNone(match, line)
            rows.append((int(match[1]), float(match[2]), float(match[3]), match[4]))
        return rows

    def run_tree(self, *args):
        result = subprocess.run(["./ptree", *args], capture_output=True, text=True,
                                timeout=3, env=self.env)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(result.stderr, "")
        self.assertNotIn("\x1b", result.stdout)
        return self.rows(result.stdout)

    def run_terminal(self, *args, lines, env=None):
        output = Watch(self, args, terminal=True, env=env or self.env)
        text = b"".join(output.read_until(b"\n") for _ in range(lines)).decode()
        self.assertEqual(output.proc.wait(timeout=2), 0)
        self.assertEqual(output.proc.stderr.read(), b"")
        return text

    def test_terminal_tree_adds_color_without_changing_layout(self):
        """Color the terminal tree while preserving its layout"""
        pids, command, _ = self.start_tree()
        output = self.run_terminal("-p", str(pids[0]), lines=5)
        self.assertRegex(output, r"\x1b\[[0-9;]*3[0-7]m")
        rows = self.rows(output)
        self.assertEqual([row[0] for row in rows], pids)
        self.assertEqual([row[3] for row in rows], [
            command, " \\_ " + command, " |   \\_ " + command, " \\_ " + command,
        ])
        for line in output.splitlines():
            self.assertEqual(re.findall(r"\x1b\[[0-9;]*m", line)[-1], "\x1b[0m")

    def test_terminal_color_respects_environment_opt_out(self):
        """Disable color with NO_COLOR or TERM=dumb"""
        pids, _, _ = self.start_tree()
        for override in ({"NO_COLOR": "1"}, {"TERM": "dumb"}):
            with self.subTest(override=override):
                output = self.run_terminal("-p", str(pids[0]), lines=5,
                                           env={**self.env, **override})
                self.assertNotIn("\x1b", output)
                self.assertEqual([row[0] for row in self.rows(output)], pids)

    def test_terminal_command_highlights_basename_and_dims_path(self):
        """Highlight the command name and dim its directory path"""
        for executable in ("/usr/bin/sleep", "sleep", "/path with spaces/my sleep"):
            with self.subTest(executable=executable):
                proc = subprocess.Popen([executable, "60"], executable="/usr/bin/sleep")
                self.addCleanup(proc.wait, timeout=2)
                self.addCleanup(proc.terminate)
                output = self.run_terminal("-p", str(proc.pid), lines=2)
                prefix, name = os.path.split(executable)
                styled = "\x1b[97m" + name + "\x1b[0m 60"
                if prefix:
                    styled = "\x1b[90m" + prefix + "/" + styled
                self.assertTrue(output.endswith(styled + "\r\n"), repr(output))
                self.assertEqual(self.rows(output)[0][3], executable + " 60")

    def test_terminal_command_highlights_script_matching_comm(self):
        """Highlight the script name when it matches the process name"""
        for name in ("worker script", "long-worker-script-name"):
            with self.subTest(name=name), tempfile.TemporaryDirectory() as directory:
                path = os.path.join(directory, name)
                with open(path, "w") as file:
                    file.write("#!/bin/sh\nprintf 'ready\\n'\nread -r line\n")
                os.chmod(path, 0o700)
                proc = subprocess.Popen([path], stdin=subprocess.PIPE,
                                        stdout=subprocess.PIPE, text=True)
                try:
                    self.assertEqual(proc.stdout.readline(), "ready\n")
                    output = self.run_terminal("-p", str(proc.pid), lines=2)
                    self.assertTrue(output.endswith(
                        f"\x1b[90m/bin/sh {directory}/\x1b[97m{name}\x1b[0m\r\n"),
                        repr(output))
                    self.assertEqual(self.rows(output)[0][3], "/bin/sh " + path)
                finally:
                    proc.terminate()
                    proc.wait(timeout=2)
                    proc.stdin.close()
                    proc.stdout.close()

    def test_usage_colors_change_at_thresholds(self):
        """Change colors at CPU and memory usage thresholds"""
        # Render controlled snapshots without burning CPU or allocating RAM.
        source = r'''
#define main ptree_main
#include "ptree.c"
#undef main
int main(int argc, char **argv) {
    if (argc != 3) return 2;
    long ticks = sysconf(_SC_CLK_TCK);
    long pages = sysconf(_SC_PHYS_PAGES);
    proc_t old = {.pid = getpid(), .selected = 1, .starttime = 1};
    proc_t proc = old;
    proc.utime = (unsigned long long)ticks * strtoul(argv[1], NULL, 10);
    proc.rss = (pages * strtol(argv[2], NULL, 10) + 99) / 100;
    snapshot_t previous = {.procs = &old, .n = 1, .sampled_at = {.tv_sec = 1}};
    snapshot_t current = {.procs = &proc, .n = 1, .sampled_at = {.tv_sec = 101}};
    buff_t buff = {0};
    cli_opts_t inputs = {.format = FORMAT_TREE, .color = 1};
    int result = print_trees(stdout, &inputs, &current, &previous, &buff);
    free(buff.data);
    return result;
}
'''
        with tempfile.TemporaryDirectory() as directory:
            path = os.path.join(directory, "render.c")
            binary = os.path.join(directory, "render")
            with open(path, "w") as file:
                file.write(source)
            build = subprocess.run([
                *shlex.split(os.environ.get("CC", "cc")), "-std=c99",
                "-Wall", "-Wextra", "-Wpedantic",
                "-I", str(Path(__file__).resolve().parent.parent), path, "-o", binary,
            ], capture_output=True, text=True, timeout=30)
            self.assertEqual(build.returncode, 0, build.stderr)
            for cpu, mem, cpu_color, mem_color in (
                    (0, 0, 32, 32), (49, 9, 32, 32), (50, 10, 33, 33),
                    (89, 24, 33, 33), (90, 25, 31, 31), (200, 80, 31, 31),
                    (0, 25, 32, 31), (90, 0, 31, 32),
                    (100, 100, 31, 31), (self.max_cpu, 100, 31, 31)):
                with self.subTest(cpu=cpu, mem=mem):
                    output = subprocess.check_output([binary, str(cpu), str(mem)],
                                                     text=True, timeout=3)
                    self.assertIn(f"\x1b[{cpu_color}m{cpu:{self.cpu_width}.1f}\x1b[0m "
                                  f"\x1b[{mem_color}m{mem:5.1f}\x1b[0m", output)
                    header, row = self.plain(output).splitlines()
                    self.assertEqual(header, self.header)
                    self.assertEqual(row.index(binary), header.index("COMMAND"))

    def test_terminal_pid_lists_remain_plain(self):
        """Print PID lists in plain text even on a terminal"""
        pids, _, _ = self.start_tree()
        for flags, lines in ((["-l"], 4), (["--list"], 4), (["-c"], 1), (["--comma"], 1)):
            with self.subTest(flags=flags):
                output = self.run_terminal(*flags, "-p", str(pids[0]), lines=lines)
                self.assertNotIn("\x1b", output)
                self.assertEqual(set(map(int, output.replace(",", " ").split())), set(pids))

    def test_pid_tree_has_depth_first_branches_and_full_commands(self):
        """Show full command lines in a depth-first tree"""
        pids, command, _ = self.start_tree()
        rows = self.run_tree("-p", str(pids[0]))
        self.assertEqual([row[0] for row in rows], pids)
        self.assertEqual([row[3] for row in rows], [
            command, " \\_ " + command, " |   \\_ " + command, " \\_ " + command,
        ])

    def test_filter_forest_keeps_overlapping_matches_under_their_parents(self):
        """Keep matching descendants under their parents"""
        token = "ptree-tree-" + uuid.uuid4().hex
        first, _, _ = self.start_tree(token)
        second, _, _ = self.start_tree(token)
        rows = self.run_tree("-f", token)
        self.assertEqual([row[0] for row in rows], first + second)
        self.assertFalse(rows[0][3].startswith(" "))
        self.assertFalse(rows[4][3].startswith(" "))

    def test_long_commands_and_control_characters_stay_on_one_line(self):
        """Keep long commands with control characters on one line"""
        pids, command, _ = self.start_tree("x" * 5000 + "\n\t\x1btail")
        rows = self.run_tree("-p", str(pids[0]))
        self.assertEqual(len(rows), 4)
        self.assertEqual(rows[0][3], command.replace("\n", "?").replace("\t", "?").replace("\x1b", "?"))

    def test_cpu_and_memory_percentages_are_similar_to_ps(self):
        """Report CPU and memory usage close to ps"""
        proc = subprocess.Popen([sys.executable, "-c",
                                 "import time; data = bytearray(16000000); "
                                 "exec('while True: pass')"])
        self.addCleanup(proc.wait, timeout=2)
        self.addCleanup(proc.terminate)
        time.sleep(1.1)
        row = self.run_tree("-p", str(proc.pid))[0]
        expected = subprocess.check_output(
            ["ps", "-o", "pcpu=,pmem=", "-p", str(proc.pid)], text=True).split()
        self.assertGreater(row[1], 0)
        self.assertAlmostEqual(row[1], float(expected[0]), delta=20)
        self.assertAlmostEqual(row[2], float(expected[1]), delta=0.1)

    def test_empty_command_line_uses_name_with_parentheses(self):
        """Show process names with parentheses for empty command lines"""
        proc = subprocess.Popen([
            sys.executable, "-u", "-c",
            "import os, sys\n"
            "child = os.fork()\n"
            "if child == 0:\n"
            "    with open('/proc/self/comm', 'w') as f: f.write('tree/) (name')\n"
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
        rows = self.run_tree("-p", str(child))
        self.assertEqual(len(rows), 1)
        self.assertEqual(rows[0][0], child)
        self.assertEqual(rows[0][3], "[tree/) (name]")
        output = self.run_terminal("-p", str(child), lines=2)
        self.assertIn("[\x1b[97mtree/) (name\x1b[0m]", output)

    def test_watch_renders_tree_frames(self):
        """Redraw the colored process tree on each refresh"""
        pids, command, _ = self.start_tree()
        watch = Watch(self, ["--watch=0.05", "-p", str(pids[0])],
                      terminal=True, rows=20, cols=1000, env=self.env)
        for _ in range(2):
            frame = watch.frame().decode()
            self.assertRegex(frame, r"\x1b\[[0-9;]*3[0-7]m")
            rows = self.rows(frame)
            self.assertEqual([row[0] for row in rows], pids)
            self.assertEqual(rows[2][3], " |   \\_ " + command)

    def test_watch_resets_color_when_frame_is_clipped(self):
        """Reset terminal color after clipping a frame"""
        pids, _, _ = self.start_tree()
        watch = Watch(self, ["--watch=0.05", "-p", str(pids[0])],
                      terminal=True, rows=1, cols=5, env=self.env)
        frame = watch.frame().decode()
        self.assertTrue(frame.endswith("\x1b[0m"), repr(frame))
        self.assertEqual(self.plain(frame), self.header[:5])

    def test_watch_cpu_tracks_each_interval_when_process_stops_and_resumes(self):
        """Refresh CPU usage as a process stops and resumes"""
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
                watch = Watch(self, ["--watch=0.05", "-p", str(proc.pid)],
                              terminal=terminal, cols=1000, env=self.env)

                def cpu():
                    if terminal:
                        frame = watch.frame()
                    else:
                        frame = watch.read_until(b"\n") + watch.read_until(b"\n")
                        self.assertNotIn(b"\x1b", frame)
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
        """Redraw the tree when matches return after an empty frame"""
        token = "ptree-tree-" + uuid.uuid4().hex
        watch = Watch(self, ["--watch=0.05", "-f", f"^{token} 60$"],
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
        """Wrap Unicode by display width and keep characters intact"""
        proc = subprocess.Popen(["éé界e\u0301", "60"], executable="/usr/bin/sleep")
        self.addCleanup(proc.wait, timeout=2)
        self.addCleanup(proc.terminate)
        command_column = self.header.index("COMMAND")
        narrow_prefix = "é\r\né" if command_column % 2 else "éé"
        for cols, suffix in ((command_column + 1, "é\r\né界e\u0301 60\r\n"),
                             (2, narrow_prefix + "\r\n界\r\ne\u0301 \r\n60\r\n")):
            with self.subTest(cols=cols):
                watch = Watch(self, ["--watch=0.05", "-p", str(proc.pid)],
                              terminal=True, rows=50, cols=cols,
                              env={**self.env, "LC_ALL": "C.UTF-8"})
                frame = self.plain(watch.frame().decode("utf-8"))
                self.assertTrue(frame.endswith(suffix), frame)

    def test_watch_display_locale_does_not_change_filter_matching(self):
        """Keep C-locale regex matching with a Unicode display environment"""
        token = "ptree-locale-" + uuid.uuid4().hex
        proc = subprocess.Popen(["é" + token, "60"], executable="/usr/bin/sleep")
        self.addCleanup(proc.wait, timeout=2)
        self.addCleanup(proc.terminate)
        watch = Watch(self, ["--watch=0.05", "-f", f"^[[:alpha:]]{re.escape(token)} 60$"],
                      terminal=True, env={**self.env, "LC_ALL": "C.UTF-8"})
        # C.UTF-8 considers é alphabetic; the CLI's C-locale regex does not.
        self.assertEqual(watch.frame(), b"")
        self.assertEqual(watch.frame(), b"")

    def test_empty_selection_and_removed_tree_options(self):
        """Report no matches and reject removed tree options"""
        for args, status in ((["-f", "^ptree-tree-missing$"], 1),
                             (["-t", "-p", str(os.getpid())], 2),
                             (["--tree", "-p", str(os.getpid())], 2)):
            with self.subTest(args=args):
                result = subprocess.run(["./ptree", *args], capture_output=True,
                                        timeout=3)
                self.assertEqual(result.returncode, status)
                self.assertEqual(result.stdout, b"")


if __name__ == "__main__":
    if "--fixture" in sys.argv:
        fixture()
    else:
        unittest.main()
