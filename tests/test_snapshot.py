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
#include <stdlib.h>
#include <unistd.h>
#include <dirent.h>
#include <fcntl.h>
static pid_t fixture_getpid(void);
static int fixture_scan;
static int fixture_commands;
static int fixture_oom;
static int fixture_limit_reads;
static void *fixture_realloc(void *data, size_t size);
static DIR *fixture_opendir(const char *path);
static struct dirent *fixture_readdir(DIR *dir);
static int fixture_closedir(DIR *dir);
static int fixture_open(const char *path, int flags);
static long fixture_sysconf(int name);
#define getpid fixture_getpid
#define opendir fixture_opendir
#define readdir fixture_readdir
#define closedir fixture_closedir
#define open fixture_open
#define sysconf fixture_sysconf
#define realloc fixture_realloc
#define main ptree_main
#include "ptree.c"
#undef main
#undef realloc
#undef open
#undef sysconf
#undef opendir
#undef readdir
#undef closedir
#undef getpid

static void *fixture_realloc(void *data, size_t size) {
    return fixture_oom ? NULL : realloc(data, size);
}

static pid_t fixture_getpid(void) {
    return 1000000; /* The harness is outside the synthetic process table. */
}

static DIR *fixture_opendir(const char *path) {
    return fixture_scan ? (DIR *)&fixture_scan : opendir(path);
}

static struct dirent *fixture_readdir(DIR *dir) {
    static size_t cursor;
    static const char *pids[] = {"60", "20", "40", "10", "50", "30"};
    static struct dirent entry;
    if (!fixture_scan) return readdir(dir);
    if (cursor == 6) return NULL;
    strcpy(entry.d_name, pids[cursor++]);
    return &entry;
}

static int fixture_closedir(DIR *dir) {
    return fixture_scan ? 0 : closedir(dir);
}

/* Serve fixture contents through a pipe so the production read and close apply. */
static int fixture_file(const char *data, size_t length) {
    int fds[2];
    if (pipe(fds) != 0) return -1;
    ssize_t written = write(fds[1], data, length);
    close(fds[1]);
    if (written == (ssize_t)length) return fds[0];
    close(fds[0]);
    return -1;
}

static int fixture_open(const char *path, int flags) {
    static const char uptime[] = "1000.00 0.00\n";
    static const char limit[] = "4194304\n";
    (void)flags;
    if (strcmp(path, "/proc/uptime") == 0) return fixture_file(uptime, sizeof(uptime) - 1);
    if (strcmp(path, "/proc/sys/kernel/pid_max") == 0) {
        fixture_limit_reads++;
        return fixture_file(limit, sizeof(limit) - 1);
    }
    if (fixture_commands && strstr(path, "/cmdline")) {
        static char long_command[5006];
        static const char short_command[] = "short";
        static const char empty[] = "";
        static const char control[] = "line\nbreak";
        static const char before[] = "before";
        static const char after[] = "after";
        static int changed;
        int pid = atoi(path + 6);
        switch (pid) {
        case 10:
            memset(long_command, 'x', 5000);
            memcpy(long_command + 5000, "\0tail", 6);
            return fixture_file(long_command, sizeof(long_command));
        case 20: return fixture_file(short_command, sizeof(short_command));
        case 30: return fixture_file(empty, sizeof(empty));
        case 50: return fixture_file(control, sizeof(control));
        case 60:
            if (changed++) return fixture_file(after, sizeof(after));
            return fixture_file(before, sizeof(before));
        }
    }
    if (fixture_scan && strstr(path, "/stat")) {
        char stat[256];
        int pid = atoi(path + 6);
        int length = snprintf(stat, sizeof(stat),
            "%d (process) S 0 0 0 0 0 0 0 0 0 0 %d 0 0 0 0 0 0 0 90000 0 100\n",
            pid, pid * 10);
        return fixture_file(stat, (size_t)length);
    }
    errno = ENOENT;
    return -1;
}

static long fixture_sysconf(int name) {
    switch (name) {
    case _SC_CLK_TCK: return 100;
    case _SC_PHYS_PAGES: return 10000;
    case _SC_NPROCESSORS_ONLN: return 8;
    default: return sysconf(name);
    }
}

static int run(int argc, char **argv, buff_t *buff) {
    if (argc != 2) return 2;
    if (strcmp(argv[1], "locale") == 0) {
        locale_t caller = newlocale(LC_CTYPE_MASK, "C", (locale_t)0);
        locale_t display = newlocale(LC_CTYPE_MASK, "C.UTF-8", (locale_t)0);
        if (!caller || !display) return 2;
        locale_t original = uselocale(caller);
        redraw("é界é", 0, display);
        int restored = uselocale((locale_t)0) == caller;
        uselocale(original);
        freelocale(display);
        freelocale(caller);
        if (!restored) fputs("redraw did not restore the caller locale\n", stderr);
        return restored ? 0 : 1;
    }
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
    cli_opts_t tree = {.format = FORMAT_TREE}, lines = {.format = FORMAT_LINES};
    cli_opts_t comma = {.format = FORMAT_COMMA};
    if (read_system(&tree, buff) != 0) return 2;
    if (strcmp(argv[1], "oom") == 0) {
        fixture_scan = 1;
        fixture_oom = 1;
        cli_opts_t inputs = tree;
        inputs.root = 10;
        return print_snapshot(&inputs, stdout, NULL, buff);
    }
    if (strcmp(argv[1], "duplicates") == 0) {
        proc_t repeated[] = {
            {.pid = 20, .ppid = 10}, {.pid = 10, .selected = 1},
            {.pid = 20, .ppid = 10}, {.pid = 30, .ppid = 20},
        };
        snapshot_t scan = {.procs = repeated, .n = 4};
        sort_procs(repeated, scan.n);
        return print_trees(stdout, &lines, &scan, NULL, buff);
    }
    if (strcmp(argv[1], "siblings") == 0) {
        proc_t family[] = {
            {.pid = 10}, {.pid = 20, .ppid = 10, .selected = 1}, {.pid = 30, .ppid = 10},
            {.pid = 40, .ppid = 10, .selected = 1}, {.pid = 50, .ppid = 20},
        };
        snapshot_t scan = {.procs = family, .n = 5};
        sort_procs(family, scan.n);
        return print_trees(stdout, &tree, &scan, NULL, buff);
    }
    /* Hand-built tables stand in for a scan, so sort them as collect_procs does. */
    if (strcmp(argv[1], "system-once") == 0) {
        sort_procs(procs, n);
        if (print_trees(stdout, &tree, &current, NULL, buff) != 0) return 1;
        if (print_trees(stdout, &tree, &current, NULL, buff) != 0) return 1;
        printf("pid_max reads: %d\n", fixture_limit_reads);
        return 0;
    }
    if (strncmp(argv[1], "commands", 8) == 0) {
        fixture_commands = 1;
        if (strcmp(argv[1], "commands-filter") == 0) {
            fixture_scan = 1;
            cli_opts_t inputs = tree;
            inputs.use_filter = 1;
            if (regcomp(&inputs.filter, "^short$", REG_EXTENDED | REG_NOSUB) != 0) return 2;
            int result = print_snapshot(&inputs, stdout, NULL, buff);
            regfree(&inputs.filter);
            return result;
        }
        current.n = 6;
        for (size_t i = 0; i < current.n; i++) {
            procs[i].pid = (pid_t)(i + 1) * 10;
            procs[i].ppid = 0;
            procs[i].selected = 1;
        }
        sort_procs(procs, current.n);
        tree.color = strcmp(argv[1], "commands-color") == 0;
        if (print_trees(stdout, &tree, &current, NULL, buff) != 0) return 1;
        return print_trees(stdout, &tree, &current, NULL, buff);
    }
    if (strncmp(argv[1], "cpu", 3) == 0) {
        current.n = 6;
        for (size_t i = 0; i < current.n; i++) {
            procs[i].pid = (pid_t)(i + 1) * 10;
            procs[i].ppid = 0;
            procs[i].selected = 1;
        }
        procs[3].stime = 100;
        sort_procs(procs, current.n);
        proc_t old[] = {
            {.pid = 10, .starttime = 90000, .utime = 900},
            {.pid = 20, .starttime = 89999, .utime = 900},
            {.pid = 30, .starttime = 90000, .utime = 1001},
            {.pid = 40, .starttime = 90000, .utime = 900, .stime = 101},
            {.pid = 60, .starttime = 90000, .utime = 950},
        };
        snapshot_t previous = {.procs = old, .n = 5, .sampled_at = {.tv_sec = 9}};
        if (strcmp(argv[1], "cpu-after-empty-frame") == 0) {
            fixture_scan = 1;
            snapshot_t empty = {0};
            cli_opts_t inputs = tree;
            inputs.root = 999;
            inputs.watch = 1;
            if (print_snapshot(&inputs, stdout, &empty, buff) != 0) return 1;
            empty.sampled_at.tv_sec = 9;
            empty.sampled_at.tv_nsec = 0;
            int result = print_trees(stdout, &tree, &current, &empty, buff);
            free(empty.procs);
            return result;
        }
        if (strcmp(argv[1], "cpu-zero-interval") == 0) previous.sampled_at = current.sampled_at;
        if (strcmp(argv[1], "cpu-empty") == 0) previous.n = 0;
        return print_trees(stdout, &tree, &current,
                           strcmp(argv[1], "cpu-first") == 0 ? NULL : &previous, buff);
    }
    sort_procs(procs, n);
    const cli_opts_t *inputs = strcmp(argv[1], "comma") == 0 ? &comma
                               : strcmp(argv[1], "tree") == 0 ? &tree : &lines;
    return print_trees(stdout, inputs, &current, NULL, buff);
}

int main(int argc, char **argv) {
    buff_t buff = {0};
    int result = run(argc, argv, &buff);
    free(buff.data);
    return result;
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
        result = subprocess.run([str(self.binary), mode], capture_output=True, timeout=3)
        self.assertEqual(result.returncode, 0, result.stderr.decode())
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

    def test_repeated_scan_entries_are_listed_without_looping(self):
        """List a PID the scan reported twice without relinking its children"""
        self.assertEqual(self.run_snapshot("duplicates"), "10\n20\n20\n30\n")

    def test_roots_exclude_unselected_siblings(self):
        """Render selected roots without their unselected parent's other children"""
        self.assertEqual(self.run_snapshot("siblings"),
                         "    PID  %CPU  %MEM COMMAND\n"
                         "     20   0.0   0.0 []\n"
                         "     50   0.0   0.0  \\_ []\n"
                         "     40   0.0   0.0 []\n")

    def test_out_of_memory_stops_with_status_one(self):
        """Report an allocation failure and exit with status 1"""
        result = subprocess.run([str(self.binary), "oom"], capture_output=True, timeout=3)
        self.assertEqual(result.returncode, 1)
        self.assertEqual(result.stdout, b"")
        self.assertEqual(result.stderr, b"ptree: out of memory\n")

    def test_system_values_are_read_once_per_run(self):
        """Read pid_max once for all the frames of a run"""
        self.assertEqual(self.run_snapshot("system-once").splitlines()[-1], "pid_max reads: 1")

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

    def test_cpu_history_after_empty_frame_keeps_all_discovered_processes(self):
        """Use the prior sample after an empty frame with shuffled discovery"""
        self.assertEqual(self.cpu_values("cpu-after-empty-frame"), [900, 800, 700, 700, 500, 400])

    def test_commands_remain_fresh_after_long_empty_and_unreadable_reads(self):
        """Render fresh commands across long, short, empty, and unavailable reads"""
        lines = self.run_snapshot("commands").splitlines()
        commands = [line.split(maxsplit=3)[3] for line in lines if "COMMAND" not in line]
        expected = ["x" * 5000 + " tail", "short", "[process]", "[process]", "line?break", "before"]
        self.assertEqual(commands, expected + expected[:-1] + ["after"])

    def test_command_filter_does_not_reuse_empty_or_unreadable_contents(self):
        """Do not match stale command contents while filtering a process scan"""
        self.assertEqual(self.run_snapshot("commands-filter"),
                         "    PID  %CPU  %MEM COMMAND\n"
                         "     20   2.0   1.0 short\n")

    def test_command_highlighting_survives_buffer_reuse(self):
        """Preserve command highlighting after buffer growth and empty reads"""
        import re
        colored = self.run_snapshot("commands-color")
        self.assertIn("\x1b[97mshort\x1b[0m", colored)
        self.assertIn("[\x1b[97mprocess\x1b[0m]", colored)
        self.assertEqual(re.sub(r"\x1b\[[0-9;]*m", "", colored), self.run_snapshot("commands"))

    def test_redraw_restores_the_callers_thread_locale(self):
        """Render Unicode and restore a non-global caller locale"""
        self.assertEqual(self.run_snapshot("locale"), "\x1b[H\x1b[Jé界é")
