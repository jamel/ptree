"""Deterministic output contracts for traversal and CPU history."""
import os
import shlex
import subprocess
import tempfile
import unittest
from pathlib import Path


SOURCE = r'''
#define _POSIX_C_SOURCE 200809L
#define _XOPEN_SOURCE 700
#include <stdio.h>
#include <unistd.h>
static FILE *fixture_fopen(const char *path, const char *mode);
static long fixture_sysconf(int name);
#define fopen fixture_fopen
#define sysconf fixture_sysconf
#define main ptree_main
#include "ptree.c"
#undef main
#undef fopen
#undef sysconf

static FILE *fixture_fopen(const char *path, const char *mode) {
    static char uptime[] = "1000.00 0.00\n";
    static char limit[] = "4194304\n";
    if (strcmp(path, "/proc/uptime") == 0) return fmemopen(uptime, sizeof(uptime) - 1, mode);
    if (strcmp(path, "/proc/sys/kernel/pid_max") == 0) return fmemopen(limit, sizeof(limit) - 1, mode);
    errno = ENOENT;
    return NULL;
}

static long fixture_sysconf(int name) {
    switch (name) {
    case _SC_CLK_TCK: return 100;
    case _SC_PHYS_PAGES: return 10000;
    case _SC_NPROCESSORS_ONLN: return 8;
    default: return sysconf(name);
    }
}

int main(int argc, char **argv) {
    if (argc != 2) return 2;
    proc_t procs[] = {
        {.pid = 60, .ppid = 20},
        {.pid = 90, .ppid = 900, .selected = 1},
        {.pid = 40, .ppid = 20, .selected = 1},
        {.pid = 20, .ppid = 10},
        {.pid = 10, .selected = 1},
        {.pid = 50, .ppid = 40},
        {.pid = 70, .ppid = 70, .selected = 1},
    };
    size_t n = sizeof(procs) / sizeof(*procs);
    for (size_t i = 0; i < n; i++) {
        procs[i].starttime = 90000;
        procs[i].utime = 1000;
        procs[i].rss = 100;
        strcpy(procs[i].comm, "process");
    }
    snapshot_t current = {.procs = procs, .n = n, .sampled_at = {.tv_sec = 10}};
    if (strncmp(argv[1], "cpu", 3) == 0) {
        current.n = 6;
        for (size_t i = 0; i < current.n; i++) {
            procs[i].pid = (pid_t)(i + 1) * 10;
            procs[i].ppid = 0;
            procs[i].selected = 1;
        }
        procs[3].stime = 100;
        proc_t old[] = {
            {.pid = 10, .starttime = 90000, .utime = 900},
            {.pid = 20, .starttime = 89999, .utime = 900},
            {.pid = 30, .starttime = 90000, .utime = 1001},
            {.pid = 40, .starttime = 90000, .utime = 900, .stime = 101},
            {.pid = 60, .starttime = 90000, .utime = 950},
        };
        snapshot_t previous = {.procs = old, .n = 5, .sampled_at = {.tv_sec = 9}};
        if (strcmp(argv[1], "cpu-zero-interval") == 0) previous.sampled_at = current.sampled_at;
        if (strcmp(argv[1], "cpu-empty") == 0) previous.n = 0;
        return print_trees(stdout, &current, 0, 1, 0,
                           strcmp(argv[1], "cpu-first") == 0 ? NULL : &previous);
    }
    return print_trees(stdout, &current, strcmp(argv[1], "comma") == 0,
                       strcmp(argv[1], "tree") == 0, 0, NULL);
}
'''


class SnapshotTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.directory = tempfile.TemporaryDirectory(prefix="ptree-snapshot-")
        cls.addClassCleanup(cls.directory.cleanup)
        source = Path(cls.directory.name) / "snapshot.c"
        source.write_text(SOURCE)
        cls.binary = Path(cls.directory.name) / "snapshot"
        subprocess.run([*shlex.split(os.environ.get("CC", "cc")), "-std=c99",
                        "-Wall", "-Wextra", "-Wpedantic", "-O2", "-I",
                        str(Path(__file__).resolve().parent.parent), str(source),
                        "-o", str(cls.binary)], check=True, capture_output=True)

    def run_snapshot(self, mode):
        result = subprocess.run([str(self.binary), mode], capture_output=True,
                                check=True, timeout=3)
        self.assertEqual(result.stderr, b"")
        return result.stdout.decode()

    def test_shuffled_overlapping_roots_preserve_list_discovery_order(self):
        """Preserve BFS discovery order with overlapping and self-parent roots"""
        self.assertEqual(self.run_snapshot("list"), "90\n40\n10\n70\n50\n20\n60\n")
        self.assertEqual(self.run_snapshot("comma"), "90,40,10,70,50,20,60\n")

    def test_shuffled_roots_render_sorted_depth_first_branches(self):
        """Sort tree siblings while retaining overlapping, missing, and self parents"""
        self.assertEqual(self.run_snapshot("tree"),
                         "    PID  %CPU  %MEM COMMAND\n"
                         "     10  10.0   1.0 [process]\n"
                         "     20  10.0   1.0  \\_ [process]\n"
                         "     40  10.0   1.0      \\_ [process]\n"
                         "     50  10.0   1.0      |   \\_ [process]\n"
                         "     60  10.0   1.0      \\_ [process]\n"
                         "     70  10.0   1.0 [process]\n"
                         "     90  10.0   1.0 [process]\n")

    def cpu_values(self, mode):
        return [float(line.split()[1]) for line in self.run_snapshot(mode).splitlines()[1:]]

    def test_cpu_history_preserves_identity_and_monotonic_counter_checks(self):
        """Use matching CPU samples, including previously unselected processes"""
        self.assertEqual(self.cpu_values("cpu"), [100, 10, 10, 11, 10, 50])

    def test_cpu_falls_back_without_usable_previous_interval(self):
        """Use lifetime CPU for first samples, empty history, and zero intervals"""
        for mode in ("cpu-first", "cpu-empty", "cpu-zero-interval"):
            with self.subTest(mode=mode):
                self.assertEqual(self.cpu_values(mode), [10, 10, 10, 11, 10, 10])
