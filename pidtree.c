/*
 * pidtree - print a PID and all of its descendants.
 *
 * Usage: pidtree [-c|--comma] <pid>
 *
 * Walks /proc once to build a flat (pid, ppid) table, then BFS-traverses
 * from the given root.
 */

#define _POSIX_C_SOURCE 200809L
#include <ctype.h>
#include <dirent.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>

typedef struct {
    pid_t pid;
    pid_t ppid;
} proc_t;

/*
 * Read the PPID from /proc/<pid>/stat.
 *
 * The "stat" file has the form:
 *   <pid> (<comm>) <state> <ppid> ...
 * where <comm> may itself contain spaces and parentheses. To parse safely
 * we locate the LAST ')' and read fields after it.
 */
static int read_ppid(pid_t pid, pid_t *ppid_out) {
    char path[64];
    snprintf(path, sizeof(path), "/proc/%d/stat", (int)pid);

    FILE *f = fopen(path, "r");
    if (!f) return -1;

    char buf[4096];
    size_t n = fread(buf, 1, sizeof(buf) - 1, f);
    fclose(f);
    if (n == 0) return -1;
    buf[n] = '\0';

    char *rparen = strrchr(buf, ')');
    if (!rparen) return -1;

    char state;
    int ppid;
    if (sscanf(rparen + 1, " %c %d", &state, &ppid) != 2) return -1;

    *ppid_out = (pid_t)ppid;
    return 0;
}

static int is_all_digits(const char *s) {
    if (!*s) return 0;
    for (; *s; ++s) {
        if (!isdigit((unsigned char)*s)) return 0;
    }
    return 1;
}

static int is_comma_flag(const char *s) {
    return strcmp(s, "-c") == 0 || strcmp(s, "--comma") == 0;
}

/* Scan /proc and collect (pid, ppid) for every process. */
static int collect_procs(proc_t **out, size_t *out_n) {
    DIR *d = opendir("/proc");
    if (!d) return -1;

    size_t cap = 256, n = 0;
    proc_t *arr = malloc(cap * sizeof(*arr));
    if (!arr) { closedir(d); return -1; }

    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        if (!is_all_digits(e->d_name)) continue;

        pid_t pid = (pid_t)atoi(e->d_name);
        pid_t ppid;
        if (read_ppid(pid, &ppid) != 0) continue; /* process gone, skip */

        if (n == cap) {
            cap *= 2;
            proc_t *t = realloc(arr, cap * sizeof(*arr));
            if (!t) { free(arr); closedir(d); return -1; }
            arr = t;
        }
        arr[n].pid = pid;
        arr[n].ppid = ppid;
        n++;
    }
    closedir(d);

    *out = arr;
    *out_n = n;
    return 0;
}

static void print_pid(pid_t pid, int comma, int *printed) {
    if (comma) {
        if (*printed) printf(",");
        printf("%d", (int)pid);
        *printed = 1;
        return;
    }

    printf("%d\n", (int)pid);
}

/*
 * BFS from `root`, printing each visited PID. Each iteration scans the
 * proc table for children of the current frontier. O(n^2) worst case,
 * which is trivial for realistic process counts (a few thousand).
 */
static void print_tree(pid_t root, const proc_t *procs, size_t n, int comma) {
    pid_t *queue = malloc((n + 1) * sizeof(*queue));
    if (!queue) return;

    size_t head = 0, tail = 0;
    int printed = 0;

    queue[tail++] = root;
    print_pid(root, comma, &printed);

    while (head < tail) {
        pid_t cur = queue[head++];
        for (size_t i = 0; i < n; i++) {
            if (procs[i].ppid == cur) {
                print_pid(procs[i].pid, comma, &printed);
                queue[tail++] = procs[i].pid;
            }
        }
    }
    if (comma) printf("\n");
    free(queue);
}

int main(int argc, char **argv) {
    int comma = 0;
    const char *pid_arg;

    if (argc == 2) {
        pid_arg = argv[1];
    } else if (argc == 3 && is_comma_flag(argv[1])) {
        comma = 1;
        pid_arg = argv[2];
    } else {
        fprintf(stderr, "usage: %s [-c|--comma] <pid>\n", argv[0]);
        return 2;
    }

    char *end;
    long v = strtol(pid_arg, &end, 10);
    if (*end != '\0' || v <= 0 || v > INT32_MAX) {
        fprintf(stderr, "invalid pid: %s\n", pid_arg);
        return 2;
    }
    pid_t root = (pid_t)v;

    proc_t *procs;
    size_t n;
    if (collect_procs(&procs, &n) != 0) {
        perror("collect_procs");
        return 1;
    }

    int found = 0;
    for (size_t i = 0; i < n; i++) {
        if (procs[i].pid == root) { found = 1; break; }
    }
    if (!found) {
        fprintf(stderr, "pid %d not found\n", (int)root);
        free(procs);
        return 1;
    }

    print_tree(root, procs, n, comma);
    free(procs);
    return 0;
}
