/*
 * ptree - print root PIDs and all of their descendants.
 *
 * Usage: ptree [OPTIONS] (-p <pid> | -f <pattern>)
 * Filters are extended regular expressions matched against full command lines.
 *
 * Walks /proc once to build a flat (pid, ppid) table, then BFS-traverses
 * from the given or matching roots.
 */

#define _POSIX_C_SOURCE 200809L
#define _XOPEN_SOURCE 700
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <getopt.h>
#include <locale.h>
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
#include <wchar.h>

typedef struct {
    pid_t pid;
    pid_t ppid;
    int selected;
    unsigned long long utime, stime, starttime;
    long rss;
    char comm[256];
    size_t child, sibling;
} proc_t;

typedef struct {
    proc_t *procs;
    size_t n;
    struct timespec sampled_at;
} snapshot_t;

typedef struct {
    size_t start, end;
} command_name_t;

typedef struct {
    char *data;
    size_t cap;
} command_buffer_t;

typedef struct {
    pid_t root;
    int comma;
    int tree;
    int color;
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
 * Read the parent, CPU times, resident pages, and name from /proc/<pid>/stat.
 *
 * The "stat" file has the form:
 *   <pid> (<comm>) <state> <ppid> ...
 * where <comm> may itself contain spaces and parentheses. To parse safely
 * we locate the LAST ')' and read fields after it.
 */
static int read_stat(pid_t pid, proc_t *proc) {
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

    char *lparen = strchr(buf, '(');
    char *rparen = strrchr(buf, ')');
    if (!lparen || !rparen || rparen <= lparen) {
        return -1;
    }

    char state;
    int ppid;
    if (sscanf(rparen + 1, " %c %d"
               " %*s %*s %*s %*s %*s %*s %*s %*s %*s" /* fields 5-13 */
               " %llu %llu"                            /* utime, stime */
               " %*s %*s %*s %*s %*s %*s"               /* fields 16-21 */
               " %llu %*s %ld",                        /* starttime, vsize, rss */
               &state, &ppid, &proc->utime, &proc->stime,
               &proc->starttime, &proc->rss) != 6) {
        return -1;
    }

    proc->pid = pid;
    proc->ppid = (pid_t)ppid;
    snprintf(proc->comm, sizeof(proc->comm), "%.*s", (int)(rparen - lparen - 1), lparen + 1);
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

/* *out borrows buffer storage until the next read; unreadable lines leave it
 * NULL. The caller owns buffer->data and may reuse its capacity across reads. */
static int read_cmdline(pid_t pid, command_buffer_t *buffer, char **out,
                        const char *comm, command_name_t *name) {
    *out = NULL;
    if (name) {
        *name = (command_name_t){0};
    }
    char path[64];
    snprintf(path, sizeof(path), "/proc/%d/cmdline", (int)pid);
    FILE *f = fopen(path, "r");
    if (!f) {
        return 0;
    }

    if (!buffer->data) {
        buffer->data = malloc(4096);
        if (!buffer->data) {
            fclose(f);
            return -1;
        }
        buffer->cap = 4096;
    }
    size_t cap = buffer->cap, n = 0;
    char *cmdline = buffer->data;

    size_t count;
    while ((count = fread(cmdline + n, 1, cap - n - 1, f)) > 0) {
        n += count;
        if (n == cap - 1) {
            cap *= 2;
            char *t = realloc(cmdline, cap);
            if (!t) {
                fclose(f);
                return -1;
            }
            cmdline = buffer->data = t;
            buffer->cap = cap;
        }
    }
    int readable = !ferror(f);
    fclose(f);

    if (n > 0 && cmdline[n - 1] == '\0') {
        n--;
    }
    if (name) {
        /* Match comm before joining arguments, so spaces in paths are safe.
         * Linux may truncate comm to 15 bytes. Fall back to argv[0]'s basename
         * when a process has renamed itself or supplied a different argv[0]. */
        size_t comm_len = strlen(comm);
        for (size_t arg = 0; arg < n;) {
            size_t end = arg + strnlen(cmdline + arg, n - arg);
            size_t base = arg;
            for (size_t i = arg; i < end; i++) {
                if (cmdline[i] == '/') {
                    base = i + 1;
                }
            }
            int matched = comm_len > 0 && end - base >= comm_len
                          && (end - base == comm_len || comm_len == 15)
                          && memcmp(cmdline + base, comm, comm_len) == 0;
            if (arg == 0 || matched) {
                *name = (command_name_t){.start = base, .end = end};
            }
            if (matched) {
                break;
            }
            arg = end + 1;
        }
    }
    for (size_t i = 0; i < n; i++) {
        if (cmdline[i] == '\0') {
            cmdline[i] = ' ';
        }
    }
    cmdline[n] = '\0';
    if (readable) {
        *out = cmdline;
    }
    return 0;
}

/* Return 1 for a match, 0 for unreadable/empty command lines, -1 on allocation failure. */
static int matches_filter(pid_t pid, const regex_t *filter, command_buffer_t *buffer) {
    if (pid == getpid()) {
        return 0;
    }
    char *cmdline;
    if (read_cmdline(pid, buffer, &cmdline, NULL, NULL) != 0) {
        return -1;
    }
    return cmdline && *cmdline && regexec(filter, cmdline, 0, NULL, 0) == 0;
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

    command_buffer_t command = {0};
    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        if (!is_all_digits(e->d_name)) {
            continue;
        }

        pid_t pid = (pid_t)atoi(e->d_name);
        proc_t proc = {0};
        if (read_stat(pid, &proc) != 0) {
            continue; /* process gone, skip */
        }

        if (n == cap) {
            cap *= 2;
            proc_t *t = realloc(arr, cap * sizeof(*arr));
            if (!t) {
                free(command.data);
                free(arr);
                closedir(d);
                return -1;
            }
            arr = t;
        }
        arr[n] = proc;
        arr[n].selected = filter ? matches_filter(pid, filter, &command) : pid == root;
        if (arr[n].selected < 0) {
            free(command.data);
            free(arr);
            closedir(d);
            return -1;
        }
        n++;
    }
    free(command.data);
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

static int compare_pid(const void *a, const void *b) {
    pid_t left = ((const proc_t *)a)->pid;
    pid_t right = ((const proc_t *)b)->pid;
    return (left > right) - (left < right);
}

/* Look up a PID in a sorted process table. */
static size_t find_proc(const proc_t *procs, size_t n, pid_t pid) {
    size_t low = 0, high = n;
    while (low < high) {
        size_t mid = low + (high - low) / 2;
        if (procs[mid].pid < pid) {
            low = mid + 1;
        } else if (procs[mid].pid > pid) {
            high = mid;
        } else {
            return mid;
        }
    }
    return SIZE_MAX;
}

/* Keep commands on one display line, including in terminal watch mode. */
static void print_command(FILE *out, const char *command, command_name_t name,
                          int color) {
    const char *start = command + name.start;
    const char *end = command + name.end;
    if (color && start != command) {
        fputs("\033[90m", out);
    }
    for (; *command; command++) {
        if (color && command == start) {
            fputs("\033[97m", out);
        }
        if (color && command == end) {
            fputs("\033[0m", out);
        }
        unsigned char c = (unsigned char)*command;
        fputc(iscntrl(c) ? '?' : c, out);
    }
    if (color && command == end) {
        fputs("\033[0m", out);
    }
}

static const char *usage_color(double value, double yellow, double red) {
    if (value >= red) {
        return "\033[31m";
    }
    if (value >= yellow) {
        return "\033[33m";
    }
    return "\033[32m";
}

static size_t forest_roots(proc_t *procs, size_t n) {
    /* Children are already linked in PID order. Only selected processes whose
     * parent is absent or unselected become roots of the displayed forest. */
    size_t root = SIZE_MAX;
    for (size_t i = n; i-- > 0;) {
        if (!procs[i].selected) {
            continue;
        }
        size_t parent = find_proc(procs, n, procs[i].ppid);
        if (parent != SIZE_MAX && parent != i && procs[parent].selected) {
            continue;
        }
        procs[i].sibling = root;
        root = i;
    }
    return root;
}

static double cpu_usage(const proc_t *proc, const snapshot_t *previous,
                        long ticks, double uptime, double interval) {
    /* Watch frames use the CPU time consumed since the last scan. */
    if (interval > 0) {
        size_t index = find_proc(previous->procs, previous->n, proc->pid);
        if (index != SIZE_MAX) {
            const proc_t *old = &previous->procs[index];
            /* A reused PID belongs to a different process. */
            if (old->starttime == proc->starttime &&
                proc->utime >= old->utime && proc->stime >= old->stime) {
                double used = (double)(proc->utime - old->utime)
                              + (double)(proc->stime - old->stime);
                return 100.0 * used / ticks / interval;
            }
        }
    }
    /* One-shot output and a process's first sample use the lifetime average. */
    double elapsed = uptime - (double)proc->starttime / ticks;
    return elapsed > 0 ? 100.0 * ((double)proc->utime + proc->stime) / ticks / elapsed : 0;
}

/* The current table is sorted by PID and its children are already linked. */
static int print_forest(FILE *out, snapshot_t *current, const snapshot_t *previous,
                        int color) {
    proc_t *procs = current->procs;
    size_t n = current->n;
    double uptime;
    FILE *f = fopen("/proc/uptime", "r");
    if (!f) {
        return -1;
    }
    int read = fscanf(f, "%lf", &uptime);
    fclose(f);
    long ticks = sysconf(_SC_CLK_TCK);
    long pages = sysconf(_SC_PHYS_PAGES);
    long ncores = sysconf(_SC_NPROCESSORS_ONLN);
    if (read != 1 || ticks <= 0 || pages <= 0 || ncores <= 0) {
        errno = EIO;
        return -1;
    }

    /* pid_max is the allocation wrap value; use the PID type's limit if hidden. */
    int pid_max = INT32_MAX;
    f = fopen("/proc/sys/kernel/pid_max", "r");
    if (f) {
        if (fscanf(f, "%d", &pid_max) != 1 || pid_max <= 1) {
            pid_max = INT32_MAX;
        }
        fclose(f);
    }
    int pid_width = snprintf(NULL, 0, "%d", pid_max - 1);
    if (pid_width < 3) {
        pid_width = 3;
    }
    int cpu_width = snprintf(NULL, 0, "%.1f", ncores * 100.0);
    int mem_width = snprintf(NULL, 0, "%.1f", 100.0);

    double interval = 0;
    if (previous && previous->procs) {
        interval = (double)(current->sampled_at.tv_sec - previous->sampled_at.tv_sec)
                   + (current->sampled_at.tv_nsec - previous->sampled_at.tv_nsec) / 1e9;
    }
    size_t *ancestors = malloc(n * sizeof(*ancestors));
    if (!ancestors) {
        return -1;
    }

    size_t root = forest_roots(procs, n);

    const char *reset = color ? "\033[0m" : "";
    fprintf(out, "%s%*s %*s %*s %s%s\n", color ? "\033[1m" : "",
            pid_width, "PID", cpu_width, "%CPU", mem_width, "%MEM", "COMMAND", reset);
    size_t depth = 0;
    command_buffer_t buffer = {0};
    for (size_t cur = root; cur != SIZE_MAX;) {
        proc_t *proc = &procs[cur];
        char *command;
        command_name_t name = {0};
        if (read_cmdline(proc->pid, &buffer, &command, proc->comm, color ? &name : NULL) != 0) {
            free(buffer.data);
            free(ancestors);
            return -1;
        }
        double cpu = cpu_usage(proc, previous, ticks, uptime, interval);
        double mem = proc->rss > 0 ? 100.0 * proc->rss / pages : 0;
        fprintf(out, "%s%*d%s %s%*.1f%s %s%*.1f%s ",
                color ? "\033[36m" : "", pid_width, (int)proc->pid, reset,
                color ? usage_color(cpu, 50, 90) : "", cpu_width, cpu, reset,
                color ? usage_color(mem, 10, 25) : "", mem_width, mem, reset);
        if (color && depth) {
            fputs("\033[2m", out);
        }
        for (size_t i = 1; i < depth; i++) {
            fputs(procs[ancestors[i]].sibling != SIZE_MAX ? " |  " : "    ", out);
        }
        if (depth) {
            fputs(" \\_ ", out);
            fputs(reset, out);
        }
        if (command && *command) {
            print_command(out, command, name, color);
        } else {
            fputc('[', out);
            print_command(out, proc->comm, (command_name_t){.end = strlen(proc->comm)}, color);
            fputc(']', out);
        }
        fputc('\n', out);

        if (proc->child != SIZE_MAX) {
            ancestors[depth++] = cur;
            cur = proc->child;
        } else {
            while (procs[cur].sibling == SIZE_MAX && depth > 0) {
                cur = ancestors[--depth];
            }
            cur = procs[cur].sibling;
        }
    }
    free(buffer.data);
    free(ancestors);
    return 0;
}

/*
 * Link children once, then BFS from all selected roots, selecting each PID once.
 * Lists retain discovery order; tree children are linked in PID order.
 */
static int print_trees(FILE *out, snapshot_t *current, int comma, int tree, int color,
                       const snapshot_t *previous) {
    proc_t *procs = current->procs;
    size_t n = current->n;
    size_t *queue = malloc(n * sizeof(*queue));
    if (!queue) {
        return -1;
    }

    /* Save discovery PIDs before sorting. After linking children, compact the
     * selected roots into this storage as indices for the BFS queue. */
    for (size_t i = 0; i < n; i++) {
        queue[i] = procs[i].pid;
    }
    qsort(procs, n, sizeof(*procs), compare_pid);
    for (size_t i = 0; i < n; i++) {
        procs[i].child = procs[i].sibling = SIZE_MAX;
    }
    for (size_t i = n; i-- > 0;) {
        size_t child = tree ? i : find_proc(procs, n, (pid_t)queue[i]);
        size_t parent = find_proc(procs, n, procs[child].ppid);
        if (parent != SIZE_MAX && parent != child) {
            procs[child].sibling = procs[parent].child;
            procs[parent].child = child;
        }
    }

    size_t head = 0, tail = 0;
    for (size_t i = 0; i < n; i++) {
        size_t index = find_proc(procs, n, (pid_t)queue[i]);
        if (procs[index].selected) {
            queue[tail++] = index;
        }
    }
    while (head < tail) {
        size_t cur = queue[head++];
        for (size_t child = procs[cur].child; child != SIZE_MAX;
             child = procs[child].sibling) {
            if (!procs[child].selected) {
                procs[child].selected = 1;
                queue[tail++] = child;
            }
        }
    }
    if (!tree) {
        int printed = 0;
        for (size_t i = 0; i < tail; i++) {
            print_pid(out, procs[queue[i]].pid, comma, &printed);
        }
        if (comma) {
            fprintf(out, "\n");
        }
    }
    free(queue);
    return tree ? print_forest(out, current, previous, color) : 0;
}

/* On PARSE_OK, the caller must free filter if use_filter is set. */
static parse_result_t parse_opts(int argc, char **argv, cli_opts_t *inputs) {
    *inputs = (cli_opts_t){.tree = 1, .interval = {.tv_sec = 5}};
    const char *pid_arg = NULL;
    const char *pattern = NULL;
    const char *interval_arg = NULL;
    static const struct option options[] = {
        {"comma", no_argument, NULL, 'c'},
        {"list", no_argument, NULL, 'l'},
        {"pid", required_argument, NULL, 'p'},
        {"filter", required_argument, NULL, 'f'},
        {"watch", optional_argument, NULL, 'w'},
        {"help", no_argument, NULL, 'h'},
        {NULL, 0, NULL, 0}
    };
    int opt;
    while ((opt = getopt_long(argc, argv, "clp:f:h", options, NULL)) != -1) {
        switch (opt) {
        case 'c': inputs->comma = 1; inputs->tree = 0; break;
        case 'l': inputs->tree = 0; break;
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
                   "Show process trees with PID, %%CPU, %%MEM, and command by default.\n"
                   "Color on terminals unless NO_COLOR is nonempty or TERM=dumb.\n"
                   "CPU colors: green <50%%, yellow <90%%, red >=90%%.\n"
                   "Memory colors: green <10%%, yellow <25%%, red >=25%%.\n"
                   "\n"
                   "Options:\n"
                   "  -l, --list              Show a newline-separated PID list instead of a tree.\n"
                   "  -c, --comma             Show a comma-separated PID list instead of a tree.\n"
                   "  -p, --pid <pid>         Select a root process by PID.\n"
                   "  -f, --filter <pattern>  Match full command lines using an extended regex.\n"
                   "      --watch [interval]  Refresh every interval seconds (default: 5; Ctrl-C to stop).\n"
                   "                         Tree CPU usage is measured between refreshes after the first sample.\n"
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

static int print_snapshot(const cli_opts_t *inputs, FILE *out, snapshot_t *previous) {
    snapshot_t current = {0};
    int result = collect_procs(&current.procs, &current.n, inputs->root,
                              inputs->use_filter ? &inputs->filter : NULL);
    if (result != 0) {
        perror("collect_procs");
        return 1;
    }
    if (previous && clock_gettime(CLOCK_MONOTONIC, &current.sampled_at) != 0) {
        perror("clock_gettime");
        free(current.procs);
        return 1;
    }

    int found = 0;
    for (size_t i = 0; i < current.n; i++) {
        if (current.procs[i].selected) {
            found = 1;
            break;
        }
    }
    if (!found) {
        if (!inputs->watch && !inputs->use_filter) {
            fprintf(stderr, "pid %d not found\n", (int)inputs->root);
        }
        result = inputs->watch ? 0 : 1;
    } else {
        result = print_trees(out, &current, inputs->comma, inputs->tree, inputs->color, previous);
        if (result != 0) {
            perror("print_trees");
        }
    }
    if (previous) {
        /* Traversal sorts nonempty frames; retain sorted empty ones too. */
        if (!found) {
            qsort(current.procs, current.n, sizeof(*current.procs), compare_pid);
        }
        free(previous->procs);
        *previous = current;
    } else {
        free(current.procs);
    }
    return result != 0 ? 1 : 0;
}

/* Bound each frame to the current terminal size so refreshes never scroll. */
static void redraw(const char *frame, int color, locale_t display_locale) {
    struct winsize size = {0};
    ioctl(STDOUT_FILENO, TIOCGWINSZ, &size);
    int rows = size.ws_row ? size.ws_row : 24;
    int cols = size.ws_col ? size.ws_col : 80;
    int row = 0, col = 0;
    mbstate_t state = {0};
    /* Apply the user's character encoding to display without changing regexes. */
    locale_t previous_locale = uselocale(display_locale);

    printf("\033[H\033[J");
    while (*frame && row < rows) {
        /* Renderer-generated SGR sequences take no display columns. */
        if (frame[0] == '\033' && frame[1] == '[') {
            size_t length = 2 + strspn(frame + 2, "0123456789;");
            if (frame[length] == 'm') {
                fwrite(frame, 1, length + 1, stdout);
                frame += length + 1;
                continue;
            }
        }
        if (*frame == '\n') {
            row++;
            if (row < rows) {
                putchar('\n');
            }
            col = 0;
            frame++;
        } else {
            wchar_t wc;
            size_t length = mbrtowc(&wc, frame, MB_CUR_MAX, &state);
            size_t count = length;
            const char *text = frame;
            int width;
            if (length == (size_t)-1 || length == (size_t)-2) {
                state = (mbstate_t){0};
                length = count = 1;
                text = "?";
                width = 1;
            } else {
                width = wcwidth(wc);
                if (width < 0 || width > cols) {
                    count = 1;
                    text = "?";
                    width = 1;
                }
            }
            if (col + width > cols) {
                row++;
                if (row == rows) {
                    break;
                }
                putchar('\n');
                col = 0;
            }
            fwrite(text, 1, count, stdout);
            col += width;
            frame += length;
        }
    }
    if (color && *frame) {
        /* Clipping may discard the reset at the end of a styled field. */
        fputs("\033[0m", stdout);
    }
    if (display_locale) {
        uselocale(previous_locale);
    }
}

static int refresh(const cli_opts_t *inputs, int terminal, snapshot_t *previous,
                   locale_t display_locale) {
    if (!terminal) {
        return print_snapshot(inputs, stdout, previous);
    }

    char *frame = NULL;
    size_t size = 0;
    FILE *out = open_memstream(&frame, &size);
    if (!out) {
        perror("open_memstream");
        return 1;
    }
    int result = print_snapshot(inputs, out, previous);
    if (fclose(out) != 0) {
        perror("fclose");
        result = 1;
    }
    if (result == 0) {
        redraw(frame, inputs->color, display_locale);
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

    int terminal = isatty(STDOUT_FILENO);
    const char *no_color = getenv("NO_COLOR");
    const char *term = getenv("TERM");
    inputs.color = inputs.tree && terminal && !(no_color && *no_color)
                   && !(term && strcmp(term, "dumb") == 0);
    locale_t display_locale = (locale_t)0;
    if (inputs.watch && terminal) {
        display_locale = newlocale(LC_CTYPE_MASK, "", (locale_t)0);
    }
    snapshot_t previous = {0};
    int result;
    do {
        result = refresh(&inputs, inputs.watch && terminal,
                         inputs.watch && inputs.tree ? &previous : NULL, display_locale);
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

    free(previous.procs);
    if (display_locale) {
        freelocale(display_locale);
    }
    if (inputs.use_filter) {
        regfree(&inputs.filter);
    }
    return result;
}
