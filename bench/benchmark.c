#define main ptree_main
#include "../ptree.c"
#undef main

static double milliseconds(struct timespec start, struct timespec end) {
    return (end.tv_sec - start.tv_sec) * 1000.0 + (end.tv_nsec - start.tv_nsec) / 1e6;
}

int main(int argc, char **argv) {
    if (argc < 3) return 2;
    const char *mode = argv[1];
    size_t n = strtoul(argv[2], NULL, 10);
    if (!n || n > 1000000) return 2;
    FILE *sink = fopen("/dev/null", "w");
    proc_t *procs = calloc(n, sizeof(*procs));
    proc_t *old = calloc(n, sizeof(*old));
    if (!sink || !procs || !old) return 1;
    for (size_t i = 0; i < n; i++) {
        procs[i] = (proc_t){
            .pid = 100000000 + (pid_t)i,
            .ppid = i ? 100000000 + (pid_t)((i - 1) / 2) : 0,
            .selected = i == 0, .starttime = 1, .utime = 100,
        };
        strcpy(procs[i].comm, "benchmark");
        old[i] = procs[i];
    }
    snapshot_t current = {.procs = procs, .n = n, .sampled_at = {.tv_sec = 2}};
    snapshot_t previous = {.procs = old, .n = n, .sampled_at = {.tv_sec = 1}};
    proc_t self = {0};
    if (read_stat(getpid(), &self) != 0) return 1;
    int commands = strcmp(mode, "commands") == 0 || strcmp(mode, "commands-color") == 0;
    int display = strcmp(mode, "redraw") == 0;
    if (display && !freopen("/dev/null", "w", stdout)) return 1;

    struct timespec start, end;
    clock_gettime(CLOCK_MONOTONIC, &start);
    int result = 0;
    if (commands) {
        command_buffer_t buffer = {0};
        for (size_t i = 0; i < n && result == 0; i++) {
            char *command;
            command_name_t name;
            result = read_cmdline(self.pid, &buffer, &command, self.comm,
                                  strcmp(mode, "commands-color") == 0 ? &name : NULL);
        }
        free(buffer.data);
    } else if (display) {
        for (size_t i = 0; i < n; i++) {
            redraw("\033[1mPID %CPU %MEM COMMAND\033[0m\n"
                   "123 0.0 0.0 /path/é界é --argument\n", 1);
        }
    } else if (strcmp(mode, "list") == 0 || strcmp(mode, "tree") == 0 ||
               strcmp(mode, "watch") == 0) {
        result = print_trees(sink, &current, 0, strcmp(mode, "list") != 0, 0,
                             strcmp(mode, "watch") == 0 ? &previous : NULL);
    } else {
        result = 2;
    }
    fflush(sink);
    if (display) fflush(stdout);
    clock_gettime(CLOCK_MONOTONIC, &end);
    /* stdout is the redraw sink; timings always use stderr. */
    fprintf(stderr, "%.6f\n", milliseconds(start, end));
    fclose(sink);
    free(procs);
    free(old);
    return result != 0;
}
