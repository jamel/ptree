/*
 * pidtree - print root PIDs and all of their descendants.
 *
 * Usage: pidtree [OPTIONS] (-p <pid> | -f <pattern>)
 * Filters are extended regular expressions matched against full command lines.
 *
 * Walks /proc once to build a flat (pid, ppid) table, then BFS-traverses
 * from the given or matching roots.
 */

#define _POSIX_C_SOURCE 200809L
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <getopt.h>
#include <math.h>
#include <regex.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

typedef struct {
    pid_t pid;
    pid_t ppid;
    int selected;
} proc_t;

typedef struct {
    pid_t root;
    int comma;
    int use_filter;
    regex_t filter;
    int watch;
    struct timespec interval;
} cli_opts_t;

typedef enum {
    PARSE_OK,
    PARSE_HELP,
    PARSE_ERROR
} parse_result_t;

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
    if (!f) {
        return -1;
    }

    char buf[4096];
    size_t n = fread(buf, 1, sizeof(buf) - 1, f);
    fclose(f);
    if (n == 0) {
        return -1;
    }
    buf[n] = '\0';

    char *rparen = strrchr(buf, ')');
    if (!rparen) {
        return -1;
    }

    char state;
    int ppid;
    if (sscanf(rparen + 1, " %c %d", &state, &ppid) != 2) {
        return -1;
    }

    *ppid_out = (pid_t)ppid;
    return 0;
}

static int is_all_digits(const char *s) {
    if (!*s) {
        return 0;
    }
    for (; *s; ++s) {
        if (!isdigit((unsigned char)*s)) {
            return 0;
        }
    }
    return 1;
}

/* Return 1 for a match, 0 for unreadable/empty command lines, -1 on allocation failure. */
static int matches_filter(pid_t pid, const regex_t *filter) {
    if (pid == getpid()) {
        return 0;
    }

    char path[64];
    snprintf(path, sizeof(path), "/proc/%d/cmdline", (int)pid);
    FILE *f = fopen(path, "r");
    if (!f) {
        return 0;
    }

    size_t cap = 4096, n = 0;
    char *cmdline = malloc(cap);
    if (!cmdline) {
        fclose(f);
        return -1;
    }

    size_t count;
    while ((count = fread(cmdline + n, 1, cap - n - 1, f)) > 0) {
        n += count;
        if (n == cap - 1) {
            cap *= 2;
            char *t = realloc(cmdline, cap);
            if (!t) {
                free(cmdline);
                fclose(f);
                return -1;
            }
            cmdline = t;
        }
    }
    int readable = !ferror(f);
    fclose(f);

    if (n > 0 && cmdline[n - 1] == '\0') {
        n--;
    }
    for (size_t i = 0; i < n; i++) {
        if (cmdline[i] == '\0') {
            cmdline[i] = ' ';
        }
    }
    cmdline[n] = '\0';
    int matched = readable && n > 0 && regexec(filter, cmdline, 0, NULL, 0) == 0;
    free(cmdline);
    return matched;
}

/* Scan /proc and collect (pid, ppid) for every process. */
static int collect_procs(proc_t **out, size_t *out_n, pid_t root, const regex_t *filter) {
    DIR *d = opendir("/proc");
    if (!d) {
        return -1;
    }

    size_t cap = 256, n = 0;
    proc_t *arr = malloc(cap * sizeof(*arr));
    if (!arr) {
        closedir(d);
        return -1;
    }

    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        if (!is_all_digits(e->d_name)) {
            continue;
        }

        pid_t pid = (pid_t)atoi(e->d_name);
        pid_t ppid;
        if (read_ppid(pid, &ppid) != 0) {
            continue; /* process gone, skip */
        }

        if (n == cap) {
            cap *= 2;
            proc_t *t = realloc(arr, cap * sizeof(*arr));
            if (!t) {
                free(arr);
                closedir(d);
                return -1;
            }
            arr = t;
        }
        arr[n].pid = pid;
        arr[n].ppid = ppid;
        arr[n].selected = filter ? matches_filter(pid, filter) : pid == root;
        if (arr[n].selected < 0) {
            free(arr);
            closedir(d);
            return -1;
        }
        n++;
    }
    closedir(d);

    *out = arr;
    *out_n = n;
    return 0;
}

static void print_pid(FILE *out, pid_t pid, int comma, int *printed) {
    if (comma) {
        if (*printed) {
            fprintf(out, ",");
        }
        fprintf(out, "%d", (int)pid);
        *printed = 1;
        return;
    }

    fprintf(out, "%d\n", (int)pid);
}

/*
 * BFS from all selected roots, printing each PID once. Each iteration scans the
 * proc table for children of the current frontier. O(n^2) worst case,
 * which is trivial for realistic process counts (a few thousand).
 */
static int print_trees(FILE *out, proc_t *procs, size_t n, int comma) {
    pid_t *queue = malloc(n * sizeof(*queue));
    if (!queue) {
        return -1;
    }

    size_t head = 0, tail = 0;
    int printed = 0;

    for (size_t i = 0; i < n; i++) {
        if (procs[i].selected) {
            queue[tail++] = procs[i].pid;
            print_pid(out, procs[i].pid, comma, &printed);
        }
    }

    while (head < tail) {
        pid_t cur = queue[head++];
        for (size_t i = 0; i < n; i++) {
            if (!procs[i].selected && procs[i].ppid == cur) {
                procs[i].selected = 1;
                print_pid(out, procs[i].pid, comma, &printed);
                queue[tail++] = procs[i].pid;
            }
        }
    }
    if (comma) {
        fprintf(out, "\n");
    }
    free(queue);
    return 0;
}

/* On PARSE_OK, the caller must free filter if use_filter is set. */
static parse_result_t parse_opts(int argc, char **argv, cli_opts_t *inputs) {
    *inputs = (cli_opts_t){.interval = {.tv_sec = 5}};
    const char *pid_arg = NULL;
    const char *pattern = NULL;
    const char *interval_arg = NULL;
    static const struct option options[] = {
        {"comma", no_argument, NULL, 'c'},
        {"pid", required_argument, NULL, 'p'},
        {"filter", required_argument, NULL, 'f'},
        {"watch", optional_argument, NULL, 'w'},
        {"help", no_argument, NULL, 'h'},
        {NULL, 0, NULL, 0}
    };
    int opt;
    while ((opt = getopt_long(argc, argv, "cp:f:h", options, NULL)) != -1) {
        switch (opt) {
        case 'c': inputs->comma = 1; break;
        case 'p': pid_arg = optarg; break;
        case 'f': pattern = optarg; break;
        case 'w':
            inputs->watch = 1;
            interval_arg = optarg;
            if (!interval_arg && optind < argc && argv[optind][0] != '-') {
                interval_arg = argv[optind++];
            }
            break;
        case 'h':
            printf("usage: %s [OPTIONS] (-p <pid> | -f <pattern>)\n"
                   "\n"
                   "Print root PIDs and all of their descendants.\n"
                   "\n"
                   "Options:\n"
                   "  -c, --comma             Separate PIDs with commas instead of newlines.\n"
                   "  -p, --pid <pid>         Select a root process by PID.\n"
                   "  -f, --filter <pattern>  Match full command lines using an extended regex.\n"
                   "      --watch [interval]  Refresh every interval seconds (default: 5; Ctrl-C to stop).\n"
                   "  -h, --help              Display this help and exit.\n",
                   argv[0]);
            return PARSE_HELP;
        default: return PARSE_ERROR;
        }
    }
    if (optind != argc || (!pid_arg && !pattern) || (pid_arg && pattern)) {
        fprintf(stderr, "usage: %s [OPTIONS] (-p <pid> | -f <pattern>)\n", argv[0]);
        return PARSE_ERROR;
    }

    if (interval_arg) {
        char *end;
        errno = 0;
        double seconds = strtod(interval_arg, &end);
        if (errno != 0 || end == interval_arg || *end != '\0' ||
            !isfinite(seconds) || seconds < 1e-9 || seconds > INT32_MAX) {
            fprintf(stderr, "invalid watch interval: %s\n", interval_arg);
            return PARSE_ERROR;
        }
        inputs->interval.tv_sec = (time_t)seconds;
        inputs->interval.tv_nsec = (long)((seconds - (double)inputs->interval.tv_sec) * 1e9);
    }

    if (pattern) {
        int error = regcomp(&inputs->filter, pattern, REG_EXTENDED | REG_NOSUB);
        if (error != 0) {
            char message[256];
            regerror(error, &inputs->filter, message, sizeof(message));
            fprintf(stderr, "invalid filter: %s\n", message);
            return PARSE_ERROR;
        }
        inputs->use_filter = 1;
    } else {
        char *end;
        long v = strtol(pid_arg, &end, 10);
        if (*end != '\0' || v <= 0 || v > INT32_MAX) {
            fprintf(stderr, "invalid pid: %s\n", pid_arg);
            return PARSE_ERROR;
        }
        inputs->root = (pid_t)v;
    }
    return PARSE_OK;
}

static int print_snapshot(const cli_opts_t *inputs, FILE *out) {
    proc_t *procs;
    size_t n;
    int result = collect_procs(&procs, &n, inputs->root,
                              inputs->use_filter ? &inputs->filter : NULL);
    if (result != 0) {
        perror("collect_procs");
        return 1;
    }

    int found = 0;
    for (size_t i = 0; i < n; i++) {
        if (procs[i].selected) {
            found = 1;
            break;
        }
    }
    if (!found) {
        if (!inputs->watch && !inputs->use_filter) {
            fprintf(stderr, "pid %d not found\n", (int)inputs->root);
        }
        free(procs);
        return inputs->watch ? 0 : 1;
    }

    result = print_trees(out, procs, n, inputs->comma);
    if (result != 0) {
        perror("print_trees");
    }
    free(procs);
    return result != 0 ? 1 : 0;
}

/* Bound each frame to the current terminal size so refreshes never scroll. */
static void redraw(const char *frame) {
    struct winsize size = {0};
    ioctl(STDOUT_FILENO, TIOCGWINSZ, &size);
    int rows = size.ws_row ? size.ws_row : 24;
    int cols = size.ws_col ? size.ws_col : 80;
    int row = 0, col = 0;

    printf("\033[H\033[J");
    for (; *frame && row < rows; frame++) {
        if (*frame == '\n') {
            row++;
            if (row < rows) {
                putchar('\n');
            }
            col = 0;
        } else {
            if (col == cols) {
                row++;
                if (row == rows) {
                    break;
                }
                putchar('\n');
                col = 0;
            }
            putchar(*frame);
            col++;
        }
    }
}

static int refresh(const cli_opts_t *inputs, int terminal) {
    if (!terminal) {
        return print_snapshot(inputs, stdout);
    }

    char *frame = NULL;
    size_t size = 0;
    FILE *out = open_memstream(&frame, &size);
    if (!out) {
        perror("open_memstream");
        return 1;
    }
    int result = print_snapshot(inputs, out);
    if (fclose(out) != 0) {
        perror("fclose");
        result = 1;
    }
    if (result == 0) {
        redraw(frame);
    }
    free(frame);
    return result;
}

int main(int argc, char **argv) {
    cli_opts_t inputs;
    parse_result_t parsed = parse_opts(argc, argv, &inputs);
    if (parsed == PARSE_HELP) {
        return 0;
    }
    if (parsed == PARSE_ERROR) {
        return 2;
    }

    /* Send each frame in one write instead of one per line, so a terminal
     * never shows a refresh partly drawn. glibc ignores the size unless it is
     * given storage, and stdio still flushes that storage at exit. */
    static char output[1 << 16];
    setvbuf(stdout, output, _IOFBF, sizeof(output));

    int terminal = inputs.watch && isatty(STDOUT_FILENO);
    int result;
    do {
        result = refresh(&inputs, terminal);
        if (fflush(stdout) != 0) {
            perror("stdout");
            result = 1;
        }
        if (result != 0 || !inputs.watch) {
            break;
        }

        struct timespec remaining = inputs.interval;
        while (nanosleep(&remaining, &remaining) != 0) {
            if (errno != EINTR) {
                perror("nanosleep");
                result = 1;
                break;
            }
        }
    } while (result == 0);

    if (inputs.use_filter) {
        regfree(&inputs.filter);
    }
    return result;
}
