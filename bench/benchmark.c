#define _POSIX_C_SOURCE 200809L
#define _XOPEN_SOURCE 700

#include <fcntl.h>

static int benchmark_open(const char *path, int flags);

#define open benchmark_open
#define main ptree_main
#include "../ptree.c"
#undef main
#undef open

#define SYNTHETIC_PID 100000000

/* Synthetic processes have no /proc entries. Fail their opens without a
 * syscall, so list, tree, and watch time traversal and rendering instead of
 * ENOENT lookups. */
static int benchmark_open(const char *path, int flags)
{
	if (atoi(path + strlen("/proc/")) >= SYNTHETIC_PID) {
		errno = ENOENT;
		return -1;
	}
	return open(path, flags);
}

static double milliseconds(struct timespec start, struct timespec end)
{
	return (end.tv_sec - start.tv_sec) * 1000.0 +
	       (end.tv_nsec - start.tv_nsec) / 1e6;
}

int main(int argc, char **argv)
{
	if (argc < 3) {
		return 2;
	}

	const char *mode = argv[1];
	size_t n = strtoul(argv[2], NULL, 10);
	if (!n || n > 1000000) {
		return 2;
	}

	FILE *sink = fopen("/dev/null", "w");
	proc_t *procs = calloc(n, sizeof(*procs));
	proc_t *old = calloc(n, sizeof(*old));
	if (!sink || !procs || !old) {
		return 1;
	}

	for (size_t i = 0; i < n; i++) {
		procs[i] = (proc_t){
			.pid = SYNTHETIC_PID + (pid_t)i,
			.ppid = i ? SYNTHETIC_PID + (pid_t)((i - 1) / 2) : 0,
			.selected = i == 0,
			.starttime = 1,
			.utime = 100,
		};
		strcpy(procs[i].comm, "benchmark");
		old[i] = procs[i];
	}

	snapshot_t current = {
		.procs = procs,
		.n = n,
		.sampled_at = { .tv_sec = 2 },
	};
	snapshot_t previous = {
		.procs = old,
		.n = n,
		.sampled_at = { .tv_sec = 1 },
	};

	proc_t self = { 0 };
	buff_t buff = { 0 };

	if (read_stat(getpid(), &self, &buff) != 1) {
		return 1;
	}

	cli_opts_t inputs = {
		.format = strcmp(mode, "list") == 0 ? FORMAT_LINES :
						      FORMAT_TREE,
	};
	if (read_system(&inputs, &buff) != 0) {
		return 1;
	}

	int commands = strcmp(mode, "commands") == 0 ||
		       strcmp(mode, "commands-color") == 0;
	int display = strcmp(mode, "redraw") == 0;
	if (display && !freopen("/dev/null", "w", stdout)) {
		return 1;
	}

	struct timespec start, end;
	clock_gettime(CLOCK_MONOTONIC, &start);
	int result = 0;

	if (commands) {
		for (size_t i = 0; i < n && result == 0; i++) {
			command_name_t name;
			result = read_cmdline(self.pid, &buff, self.comm,
					      strcmp(mode, "commands-color") ==
							      0 ?
						      &name :
						      NULL) != 1;
		}
	} else if (display) {
		locale_t display_locale =
			newlocale(LC_CTYPE_MASK, "", (locale_t)0);
		for (size_t i = 0; i < n; i++) {
			redraw("\033[1mPID %CPU %MEM COMMAND\033[0m\n"
			       "123 0.0 0.0 /path/é界é --argument\n",
			       1, display_locale);
		}
		if (display_locale) {
			freelocale(display_locale);
		}
	} else if (strcmp(mode, "list") == 0 || strcmp(mode, "tree") == 0 ||
		   strcmp(mode, "watch") == 0) {
		sort_procs(procs, n);
		result = print_trees(
			sink, &inputs, &current,
			strcmp(mode, "watch") == 0 ? &previous : NULL, &buff);
	} else {
		result = 2;
	}

	fflush(sink);

	if (display) {
		fflush(stdout);
	}

	/* stdout is the redraw sink; timings always use stderr. */
	clock_gettime(CLOCK_MONOTONIC, &end);
	fprintf(stderr, "%.6f\n", milliseconds(start, end));

	fclose(sink);
	free(buff.data);
	free(procs);
	free(old);

	return result != 0;
}
