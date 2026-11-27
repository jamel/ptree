#!/usr/bin/env python3
import json
import os
import select
import subprocess
import sys
import unittest
import uuid


def read_fixture_pids(proc):
    if not select.select([proc.stdout], [], [], 3)[0]:
        raise AssertionError("timed out waiting for process tree fixture")
    return json.loads(proc.stdout.readline())


def stop_fixture(proc):
    proc.stdin.close()
    try:
        proc.wait(timeout=3)
    finally:
        proc.stdout.close()


def fixture():
    role, token = sys.argv[2:4]
    if role == "root":
        args = [sys.executable, __file__, "--fixture", "child",
                token + "-child", "ready"]
    else:
        # The grandchild has no matching token in its command line.
        args = [sys.executable, "-c", "import sys; sys.stdin.read()"]
    child = subprocess.Popen(args, stdin=subprocess.PIPE,
                             stdout=subprocess.PIPE, text=True)
    try:
        descendants = read_fixture_pids(child) if role == "root" else [child.pid]
        print(json.dumps([os.getpid(), *descendants]), flush=True)
        # EOF cascades down the tree, with each parent reaping its child.
        sys.stdin.read()
    finally:
        stop_fixture(child)


class PtreeTests(unittest.TestCase):
    def setUp(self):
        self.token = "ptree-filter-" + uuid.uuid4().hex

    def start_tree(self, name="first"):
        args = [sys.executable, __file__, "--fixture", "root",
                f"{self.token}-{name}", "0" * 5000, self.token + "-tail"]
        proc = subprocess.Popen(args, stdin=subprocess.PIPE,
                                stdout=subprocess.PIPE, text=True)
        self.addCleanup(stop_fixture, proc)
        return read_fixture_pids(proc)

    def start_forest(self):
        return self.start_tree("first") + self.start_tree("second")

    def run_ptree(self, *args, status=0):
        result = subprocess.run(["./ptree", *args], capture_output=True,
                                text=True, timeout=3)
        self.assertEqual(result.returncode, status, result.stderr)
        if status:
            self.assertEqual(result.stdout, "")
        else:
            self.assertEqual(result.stderr, "")
        return result.stdout

    def test_pid_options_select_root_and_descendants_in_order(self):
        """List the selected PID and its descendants in order"""
        pids = self.start_tree()
        root = str(pids[0])
        expected = "".join(f"{pid}\n" for pid in pids)
        for args in (["-p", root], ["--pid", root], [f"--pid={root}"]):
            with self.subTest(args=args):
                self.assertEqual(self.run_ptree(*args), expected)

    def test_pid_comma_options(self):
        """Print descendants as a comma-separated PID list"""
        pids = self.start_tree()
        root = str(pids[0])
        expected = ",".join(map(str, pids)) + "\n"
        for args in (["-c", "-p", root], ["--comma", "--pid", root]):
            with self.subTest(args=args):
                self.assertEqual(self.run_ptree(*args), expected)

    def test_filter_options_include_descendants_without_duplicates(self):
        """Include each matching process and descendant once"""
        pids = self.start_forest()
        for flag in ("-f", "--filter"):
            with self.subTest(flag=flag):
                output = self.run_ptree(flag, self.token)
                self.assertCountEqual(output.splitlines(), list(map(str, pids)))

    def test_filter_matches_full_command_line_with_extended_regex(self):
        """Match full command lines with extended regex"""
        pids = self.start_forest()
        pattern = f" {self.token}-(first|second)(-child)? "
        output = self.run_ptree("-f", pattern)
        self.assertCountEqual(output.splitlines(), list(map(str, pids)))

    def test_filter_matches_beyond_initial_read_buffer(self):
        """Match the end of long command lines"""
        pids = self.start_forest()
        output = self.run_ptree("-f", self.token + "-tail$")
        self.assertCountEqual(output.splitlines(), list(map(str, pids)))

    def test_filter_selects_only_matching_tree(self):
        """List only the matching process tree"""
        pids = self.start_tree("first")
        self.start_tree("second")
        output = self.run_ptree("-f", self.token + "-first ")
        self.assertCountEqual(output.splitlines(), list(map(str, pids)))

    def test_filter_comma_options(self):
        """Print filter results as a comma-separated PID list"""
        pids = self.start_forest()
        for args in (["-c", "-f", self.token],
                     [f"--filter={self.token}", "--comma"]):
            with self.subTest(args=args):
                output = self.run_ptree(*args)
                self.assertRegex(output, r"\A\d+(,\d+)+\n\Z")
                self.assertCountEqual(output[:-1].split(","), list(map(str, pids)))

    def test_filter_without_matches(self):
        """Exit with status 1 when the filter finds no matches"""
        self.start_tree()
        self.run_ptree("-f", self.token + "-missing", status=1)

    def test_filter_excludes_own_process(self):
        """Exclude ptree itself from filter results"""
        self.run_ptree("-f", r"^\./ptree -f ", status=1)

    def test_invalid_filter(self):
        """Reject invalid filter patterns"""
        self.run_ptree("-f", "[", status=2)

    def test_missing_or_positional_arguments(self):
        """Reject missing options or unexpected positional arguments"""
        root = str(os.getpid())
        for args in ([], [root], ["--", root], ["-p"], ["--pid"],
                     ["-f"], ["--filter"], ["-p", root, root],
                     ["-f", self.token, root]):
            with self.subTest(args=args):
                self.run_ptree(*args, status=2)

    def test_conflicting_selectors(self):
        """Reject combining --pid and --filter"""
        root = str(os.getpid())
        for args in (["-p", root, "-f", self.token],
                     ["-f", self.token, "-p", root]):
            with self.subTest(args=args):
                self.run_ptree(*args, status=2)

    def test_invalid_pids(self):
        """Reject invalid PIDs"""
        for pid in ("", "0", "-1", "abc", "2147483648"):
            with self.subTest(pid=pid):
                self.run_ptree("-p", pid, status=2)


if __name__ == "__main__":
    if "--fixture" in sys.argv:
        fixture()
    else:
        unittest.main()
