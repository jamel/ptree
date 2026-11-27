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
#include <fcntl.h>
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

#ifndef PTREE_GIT_COMMIT
#define PTREE_GIT_COMMIT "unknown"
#endif

#ifndef PTREE_BUILD_DATE
#define PTREE_BUILD_DATE "unknown"
#endif

/* --- Types -------------------------------------------------------------- */

typedef struct {
	pid_t pid;
	pid_t ppid;
	int selected;
	unsigned long long utime, stime, starttime;
	long rss;
	char comm[256];
	size_t order; /* position in the /proc scan */
	/* table indices or SIZE_MAX, linked by print_trees */
	size_t parent, child, sibling;
} proc_t;

typedef struct {
	proc_t *procs;
	size_t n;
	size_t matched; /* processes the scan selected */
	struct timespec sampled_at;
} snapshot_t;

typedef struct {
	size_t start, end;
} command_name_t;

/* Growable storage. The owner frees data and may reuse its capacity. */
typedef struct {
	char *data;
	size_t len, cap;
} buff_t;

typedef enum {
	FORMAT_TREE,
	FORMAT_LINES,
	FORMAT_COMMA,
} format_t;

typedef struct {
	pid_t root;
	format_t format;
	int color;
	int use_filter;
	regex_t filter;
	int watch;
	struct timespec interval;
	/* System values for tree output, read once per run by read_system. */
	long ticks, pages;
	int pid_width, cpu_width, mem_width;
} cli_opts_t;

typedef enum {
	PARSE_OK,
	PARSE_EXIT,
	PARSE_ERROR,
} parse_result_t;

/* --- Growable buffer ---------------------------------------------------- */

/* Running out of memory is not recoverable here: report it and stop. */
static void *xrealloc(void *data, size_t size)
{
	data = realloc(data, size);
	if (!data) {
		fputs("ptree: out of memory\n", stderr);
		exit(1);
	}

	return data;
}

/* Make room for at least size bytes, doubling so repeated growth stays cheap. */
static void buff_reserve(buff_t *buff, size_t size)
{
	if (size <= buff->cap) {
		return;
	}
	size_t cap = buff->cap ? buff->cap : 4096;
	while (cap < size) {
		cap *= 2;
	}
	buff->data = xrealloc(buff->data, cap);
	buff->cap = cap;
}

/*
 * Replace the contents with the whole file, NUL-terminated. Returns whether
 * the file could be read; errno describes a failure.
 */
static int buff_read_file(buff_t *buff, const char *path)
{
	int fd = open(path, O_RDONLY);
	if (fd < 0) {
		return 0;
	}

	ssize_t count;
	buff->len = 0;

	for (;;) {
		buff_reserve(buff, buff->len + 2); /* one byte and the NUL */
		count = read(fd, buff->data + buff->len,
			     buff->cap - buff->len - 1);
		if (count <= 0) {
			break;
		}
		buff->len += (size_t)count;
	}

	int error = errno;
	close(fd);
	errno = error;
	buff->data[buff->len] = '\0';

	return count == 0;
}

/* --- Scanning /proc ----------------------------------------------------- */

/*
 * Read the parent, CPU times, resident pages, and name from /proc/<pid>/stat.
 * Returns whether the process was there to read.
 *
 * The "stat" file has the form:
 *   <pid> (<comm>) <state> <ppid> ...
 * where <comm> may itself contain spaces and parentheses. To parse safely
 * we locate the LAST ')' and read fields after it.
 */
static int read_stat(pid_t pid, proc_t *proc, buff_t *buff)
{
	char path[64];
	snprintf(path, sizeof(path), "/proc/%d/stat", (int)pid);
	if (!buff_read_file(buff, path)) {
		return 0;
	}

	char *lparen = strchr(buff->data, '(');
	char *rparen = strrchr(buff->data, ')');
	if (!lparen || !rparen || rparen <= lparen) {
		return 0;
	}

	char state;
	int ppid;
	if (sscanf(rparen + 1,
		   " %c %d"
		   " %*s %*s %*s %*s %*s %*s %*s %*s %*s" /* fields 5-13 */
		   " %llu %llu" /* utime, stime */
		   " %*s %*s %*s %*s %*s %*s" /* fields 16-21 */
		   " %llu %*s %ld", /* starttime, vsize, rss */
		   &state, &ppid, &proc->utime, &proc->stime, &proc->starttime,
		   &proc->rss) != 6) {
		return 0;
	}

	proc->pid = pid;
	proc->ppid = (pid_t)ppid;
	snprintf(proc->comm, sizeof(proc->comm), "%.*s",
		 (int)(rparen - lparen - 1), lparen + 1);

	return 1;
}

static int is_all_digits(const char *s)
{
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

/*
 * Pick the part of a NUL-separated command line to highlight: the argument
 * whose basename is comm, or else argv[0]'s basename when a process has
 * renamed itself or supplied a different argv[0]. Linux may truncate comm to
 * 15 bytes.
 */
static command_name_t find_command_name(const char *cmdline, size_t n,
					const char *comm)
{
	command_name_t name = { 0 };
	size_t comm_len = strlen(comm);

	for (size_t arg = 0; arg < n;) {
		size_t end = arg + strnlen(cmdline + arg, n - arg);
		size_t base = arg;
		for (size_t i = arg; i < end; i++) {
			if (cmdline[i] == '/') {
				base = i + 1;
			}
		}

		int matched = comm_len > 0 && end - base >= comm_len &&
			      (end - base == comm_len || comm_len == 15) &&
			      memcmp(cmdline + base, comm, comm_len) == 0;
		if (matched) {
			return (command_name_t){ .start = base, .end = end };
		}
		if (arg == 0) {
			name = (command_name_t){ .start = base, .end = end };
		}
		arg = end + 1;
	}

	return name;
}

/* Read a command line into buff with its arguments joined by spaces, leaving
 * its length in buff->len. Returns whether it could be read. */
static int read_cmdline(pid_t pid, buff_t *buff, const char *comm,
			command_name_t *name)
{
	char path[64];
	snprintf(path, sizeof(path), "/proc/%d/cmdline", (int)pid);

	if (!buff_read_file(buff, path)) {
		return 0;
	}

	char *cmdline = buff->data;
	if (buff->len > 0 && cmdline[buff->len - 1] == '\0') {
		buff->len--;
	}

	if (name) {
		/* NULs still separate the arguments here, so spaces in paths are safe. */
		*name = find_command_name(cmdline, buff->len, comm);
	}

	for (size_t i = 0; i < buff->len; i++) {
		if (cmdline[i] == '\0') {
			cmdline[i] = ' ';
		}
	}

	cmdline[buff->len] = '\0';

	return 1;
}

/* Unreadable and empty command lines never match. */
static int matches_filter(pid_t pid, const regex_t *filter, buff_t *buff)
{
	return read_cmdline(pid, buff, NULL, NULL) && *buff->data &&
	       regexec(filter, buff->data, 0, NULL, 0) == 0;
}

static int compare_pid(const void *a, const void *b)
{
	pid_t left = ((const proc_t *)a)->pid;
	pid_t right = ((const proc_t *)b)->pid;

	return (left > right) - (left < right);
}

/* Sort a scanned table by PID for lookups, remembering each scan position. */
static void sort_procs(proc_t *procs, size_t n)
{
	for (size_t i = 0; i < n; i++) {
		procs[i].order = i;
	}

	if (n > 1) {
		qsort(procs, n, sizeof(*procs), compare_pid);
	}
}

/* Scan /proc into a table sorted by PID, selecting the root or filter matches. */
static int collect_procs(snapshot_t *out, pid_t root, const regex_t *filter,
			 buff_t *buff)
{
	DIR *d = opendir("/proc");
	if (!d) {
		return -1;
	}

	buff_t table = { 0 };
	size_t n = 0, matched = 0;
	pid_t self = getpid();
	struct dirent *e;

	while ((e = readdir(d)) != NULL) {
		if (!is_all_digits(e->d_name)) {
			continue;
		}

		pid_t pid = (pid_t)atoi(e->d_name);
		buff_reserve(&table, (n + 1) * sizeof(proc_t));
		proc_t *proc = (proc_t *)table.data + n;
		*proc = (proc_t){ 0 };
		if (!read_stat(pid, proc, buff)) {
			continue; /* process gone, skip */
		}
		if (filter) {
			/* ptree's own arguments contain the pattern. */
			proc->selected = pid != self &&
					 matches_filter(pid, filter, buff);
		} else {
			proc->selected = pid == root;
		}
		matched += (size_t)proc->selected;
		n++;
	}
	closedir(d);

	sort_procs((proc_t *)table.data, n);
	out->procs = (proc_t *)table.data;
	out->n = n;
	out->matched = matched;

	return 0;
}

/* --- Selection and output ----------------------------------------------- */

/* Look up a PID in a sorted process table. */
static size_t find_proc(const proc_t *procs, size_t n, pid_t pid)
{
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

/* Keep text on one display line, including in terminal watch mode: control
 * characters become '?'. */
static void print_text(FILE *out, const char *text, size_t n)
{
	size_t start = 0;

	for (size_t i = 0; i < n; i++) {
		if (iscntrl((unsigned char)text[i])) {
			fwrite(text + start, 1, i - start, out);
			fputc('?', out);
			start = i + 1;
		}
	}

	fwrite(text + start, 1, n - start, out);
}

/* Print a command, dimming what precedes its name and brightening the name. */
static void print_command(FILE *out, const char *command, size_t n,
			  command_name_t name, int color)
{
	if (!color) {
		print_text(out, command, n);
		return;
	}

	if (name.start) {
		fputs("\033[90m", out);
		print_text(out, command, name.start);
	}

	fputs("\033[97m", out);
	print_text(out, command + name.start, name.end - name.start);
	fputs("\033[0m", out);
	print_text(out, command + name.end, n - name.end);
}

static const char *usage_color(double value, double yellow, double red)
{
	if (value >= red) {
		return "\033[31m";
	}
	if (value >= yellow) {
		return "\033[33m";
	}
	return "\033[32m";
}

static double cpu_usage(const proc_t *proc, const snapshot_t *previous,
			long ticks, double uptime, double interval)
{
	/* Watch frames use the CPU time consumed since the last scan. */
	if (interval > 0) {
		size_t index =
			find_proc(previous->procs, previous->n, proc->pid);
		if (index != SIZE_MAX) {
			const proc_t *old = &previous->procs[index];
			/* A reused PID belongs to a different process. */
			if (old->starttime == proc->starttime &&
			    proc->utime >= old->utime &&
			    proc->stime >= old->stime) {
				double used =
					(double)(proc->utime - old->utime) +
					(double)(proc->stime - old->stime);
				return 100.0 * used / ticks / interval;
			}
		}
	}

	/* One-shot output and a process's first sample use the lifetime average. */
	double elapsed = uptime - (double)proc->starttime / ticks;
	return elapsed > 0 ? 100.0 * ((double)proc->utime + proc->stime) /
				     ticks / elapsed :
			     0;
}

/* Render the tree under each root. print_trees has linked every child list. */
static int print_forest(FILE *out, const cli_opts_t *inputs,
			const snapshot_t *current, const snapshot_t *previous,
			const size_t *roots, size_t nroots, buff_t *buff)
{
	const proc_t *procs = current->procs;
	int color = inputs->color;
	double uptime;

	if (!buff_read_file(buff, "/proc/uptime")) {
		return -1;
	}

	if (sscanf(buff->data, "%lf", &uptime) != 1) {
		errno = EIO;
		return -1;
	}

	double interval = 0;
	if (previous && previous->procs) {
		interval = (double)(current->sampled_at.tv_sec -
				    previous->sampled_at.tv_sec) +
			   (current->sampled_at.tv_nsec -
			    previous->sampled_at.tv_nsec) /
				   1e9;
	}

	size_t *ancestors = xrealloc(NULL, current->n * sizeof(*ancestors));

	const char *reset = color ? "\033[0m" : "";
	fprintf(out, "%s%*s %*s %*s %s%s\n", color ? "\033[1m" : "",
		inputs->pid_width, "PID", inputs->cpu_width, "%CPU",
		inputs->mem_width, "%MEM", "COMMAND", reset);

	size_t depth = 0, root = 0;
	for (size_t cur = nroots ? roots[0] : SIZE_MAX; cur != SIZE_MAX;) {
		const proc_t *proc = &procs[cur];
		command_name_t name = { 0 };
		int readable = read_cmdline(proc->pid, buff, proc->comm,
					    color ? &name : NULL);
		double cpu = cpu_usage(proc, previous, inputs->ticks, uptime,
				       interval);
		double mem = proc->rss > 0 ? 100.0 * proc->rss / inputs->pages :
					     0;
		fprintf(out, "%s%*d%s %s%*.1f%s %s%*.1f%s ",
			color ? "\033[36m" : "", inputs->pid_width,
			(int)proc->pid, reset,
			color ? usage_color(cpu, 50, 90) : "",
			inputs->cpu_width, cpu, reset,
			color ? usage_color(mem, 10, 25) : "",
			inputs->mem_width, mem, reset);
		if (color && depth) {
			fputs("\033[2m", out);
		}
		for (size_t i = 1; i < depth; i++) {
			fputs(procs[ancestors[i]].sibling != SIZE_MAX ? " |  " :
									"    ",
			      out);
		}
		if (depth) {
			fputs(" \\_ ", out);
			fputs(reset, out);
		}
		if (readable && buff->len) {
			print_command(out, buff->data, buff->len, name, color);
		} else {
			size_t length = strlen(proc->comm);
			fputc('[', out);
			print_command(out, proc->comm, length,
				      (command_name_t){ .end = length }, color);
			fputc(']', out);
		}
		fputc('\n', out);

		if (proc->child != SIZE_MAX) {
			ancestors[depth++] = cur;
			cur = proc->child;
			continue;
		}
		while (depth > 0 && procs[cur].sibling == SIZE_MAX) {
			cur = ancestors[--depth];
		}
		if (depth > 0) {
			cur = procs[cur].sibling;
		} else {
			/* A root's sibling link belongs to its unselected parent. */
			cur = ++root < nroots ? roots[root] : SIZE_MAX;
		}
	}

	free(ancestors);

	return 0;
}

/*
 * Link children, then BFS from the selected processes, selecting each
 * descendant once. Lists keep scan order; tree siblings are in PID order.
 * The table must be sorted by sort_procs, as collect_procs returns it.
 */
static int print_trees(FILE *out, const cli_opts_t *inputs, snapshot_t *current,
		       const snapshot_t *previous, buff_t *buff)
{
	proc_t *procs = current->procs;
	size_t n = current->n;
	int tree = inputs->format == FORMAT_TREE;
	size_t *queue = xrealloc(NULL, n * sizeof(*queue));

	/* Queue every process in output order, then link back to front so that
     * each child list follows the same order. */
	for (size_t i = 0; i < n; i++) {
		queue[tree ? i : procs[i].order] = i;
		procs[i].child = SIZE_MAX;
	}

	for (size_t i = n; i-- > 0;) {
		size_t child = queue[i];
		size_t parent = find_proc(procs, n, procs[child].ppid);
		if (parent == child) {
			parent = SIZE_MAX;
		}
		procs[child].parent = parent;
		procs[child].sibling = SIZE_MAX;
		if (parent != SIZE_MAX) {
			procs[child].sibling = procs[parent].child;
			procs[parent].child = child;
		}
	}

	/* Keep the matched processes at the front and append their descendants. */
	size_t head = 0, tail = 0;
	for (size_t i = 0; i < n; i++) {
		if (procs[queue[i]].selected) {
			queue[tail++] = queue[i];
		}
	}

	size_t matched = tail;
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

	int result = 0;
	if (tree) {
		/* Of the processes matched up front, those whose parent is absent or
         * unselected are the roots of the displayed forest. */
		size_t nroots = 0;
		for (size_t i = 0; i < matched; i++) {
			size_t parent = procs[queue[i]].parent;
			if (parent == SIZE_MAX || !procs[parent].selected) {
				queue[nroots++] = queue[i];
			}
		}
		result = print_forest(out, inputs, current, previous, queue,
				      nroots, buff);
	} else {
		char separator = inputs->format == FORMAT_COMMA ? ',' : '\n';
		for (size_t i = 0; i < tail; i++) {
			fprintf(out, "%d", (int)procs[queue[i]].pid);
			fputc(i + 1 < tail ? separator : '\n', out);
		}
	}

	free(queue);

	return result;
}

/* --- Command-line options ----------------------------------------------- */

/* On PARSE_OK, the caller must free filter if use_filter is set. */
static parse_result_t parse_opts(int argc, char **argv, cli_opts_t *inputs)
{
	*inputs = (cli_opts_t){ .interval = { .tv_sec = 5 } };

	const char *pid_arg = NULL;
	const char *pattern_arg = NULL;
	const char *interval_arg = NULL;

	static const struct option options[] = {
		{ "comma", no_argument, NULL, 'c' },
		{ "list", no_argument, NULL, 'l' },
		{ "pid", required_argument, NULL, 'p' },
		{ "filter", required_argument, NULL, 'f' },
		{ "watch", optional_argument, NULL, 'w' },
		{ "version", no_argument, NULL, 'v' },
		{ "help", no_argument, NULL, 'h' },
		{ NULL, 0, NULL, 0 }
	};

	int opt;
	while ((opt = getopt_long(argc, argv, "clp:f:h", options, NULL)) !=
	       -1) {
		switch (opt) {
		case 'c':
			inputs->format = FORMAT_COMMA;
			break;
		case 'l':
			/* -c wins in either order */
			if (inputs->format == FORMAT_TREE) {
				inputs->format = FORMAT_LINES;
			}
			break;
		case 'p':
			pid_arg = optarg;
			break;
		case 'f':
			pattern_arg = optarg;
			break;
		case 'w':
			inputs->watch = 1;
			interval_arg = optarg;
			if (!interval_arg && optind < argc &&
			    argv[optind][0] != '-') {
				interval_arg = argv[optind++];
			}
			break;
		case 'v':
			printf("ptree 1.0.0 (%s, %s)\n"
			       "Copyright (C) 2026 Sergey Polovko <sergey@polovko.me>\n"
			       "License MIT <https://opensource.org/license/mit>\n",
			       PTREE_GIT_COMMIT, PTREE_BUILD_DATE);
			return PARSE_EXIT;
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
			       "      --version           Display version information and exit.\n"
			       "  -h, --help              Display this help and exit.\n",
			       argv[0]);
			return PARSE_EXIT;
		default:
			return PARSE_ERROR;
		}
	}

	if (optind != argc || (!pid_arg && !pattern_arg) ||
	    (pid_arg && pattern_arg)) {
		fprintf(stderr,
			"usage: %s [OPTIONS] (-p <pid> | -f <pattern>)\n",
			argv[0]);
		return PARSE_ERROR;
	}

	if (interval_arg) {
		char *end;
		errno = 0;
		double seconds = strtod(interval_arg, &end);
		if (errno != 0 || end == interval_arg || *end != '\0' ||
		    !isfinite(seconds) || seconds < 1e-9 ||
		    seconds > INT32_MAX) {
			fprintf(stderr, "invalid watch interval: %s\n",
				interval_arg);
			return PARSE_ERROR;
		}
		inputs->interval.tv_sec = (time_t)seconds;
		inputs->interval.tv_nsec =
			(long)((seconds - (double)inputs->interval.tv_sec) *
			       1e9);
	}

	if (pattern_arg) {
		int error = regcomp(&inputs->filter, pattern_arg,
				    REG_EXTENDED | REG_NOSUB);
		if (error != 0) {
			char message[256];
			regerror(error, &inputs->filter, message,
				 sizeof(message));
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

/* --- Frames and main loop ----------------------------------------------- */

static int print_snapshot(const cli_opts_t *inputs, FILE *out,
			  snapshot_t *previous, buff_t *buff)
{
	snapshot_t current = { 0 };
	int result = collect_procs(&current, inputs->root,
				   inputs->use_filter ? &inputs->filter : NULL,
				   buff);

	if (result != 0) {
		perror("collect_procs");
		return 1;
	}

	if (previous &&
	    clock_gettime(CLOCK_MONOTONIC, &current.sampled_at) != 0) {
		perror("clock_gettime");
		free(current.procs);
		return 1;
	}

	if (!current.matched) {
		if (!inputs->watch && !inputs->use_filter) {
			fprintf(stderr, "pid %d not found\n",
				(int)inputs->root);
		}
		result = inputs->watch ? 0 : 1;
	} else {
		result = print_trees(out, inputs, &current, previous, buff);
		if (result != 0) {
			perror("print_trees");
		}
	}

	if (previous) {
		free(previous->procs);
		*previous = current;
	} else {
		free(current.procs);
	}

	return result != 0 ? 1 : 0;
}

/* Bound each frame to the current terminal size so refreshes never scroll. */
static void redraw(const char *frame, int color, locale_t display_locale)
{
	struct winsize size = { 0 };
	ioctl(STDOUT_FILENO, TIOCGWINSZ, &size);
	int rows = size.ws_row ? size.ws_row : 24;
	int cols = size.ws_col ? size.ws_col : 80;
	int row = 0, col = 0;
	mbstate_t state = { 0 };
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
				state = (mbstate_t){ 0 };
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
		   locale_t display_locale, buff_t *buff)
{
	if (!terminal) {
		return print_snapshot(inputs, stdout, previous, buff);
	}

	char *frame = NULL;
	size_t size = 0;
	FILE *out = open_memstream(&frame, &size);
	if (!out) {
		perror("open_memstream");
		return 1;
	}

	int result = print_snapshot(inputs, out, previous, buff);
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

/* Read the system values that tree output uses for the whole run. */
static int read_system(cli_opts_t *inputs, buff_t *buff)
{
	inputs->ticks = sysconf(_SC_CLK_TCK);
	inputs->pages = sysconf(_SC_PHYS_PAGES);
	long ncores = sysconf(_SC_NPROCESSORS_ONLN);

	if (inputs->ticks <= 0 || inputs->pages <= 0 || ncores <= 0) {
		errno = EIO;
		return -1;
	}

	/* pid_max is the allocation wrap value; use the PID type's limit if hidden. */
	int pid_max;
	if (!buff_read_file(buff, "/proc/sys/kernel/pid_max") ||
	    sscanf(buff->data, "%d", &pid_max) != 1 || pid_max <= 1) {
		pid_max = INT32_MAX;
	}

	inputs->pid_width = snprintf(NULL, 0, "%d", pid_max - 1);
	if (inputs->pid_width < 3) {
		inputs->pid_width = 3;
	}

	inputs->cpu_width = snprintf(NULL, 0, "%.1f", ncores * 100.0);
	inputs->mem_width = snprintf(NULL, 0, "%.1f", 100.0);

	return 0;
}

int main(int argc, char **argv)
{
	cli_opts_t inputs;
	parse_result_t parsed = parse_opts(argc, argv, &inputs);

	if (parsed == PARSE_EXIT) {
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
	inputs.color = inputs.format == FORMAT_TREE && terminal &&
		       !(no_color && *no_color) &&
		       !(term && strcmp(term, "dumb") == 0);

	locale_t display_locale = (locale_t)0;
	if (inputs.watch && terminal) {
		display_locale = newlocale(LC_CTYPE_MASK, "", (locale_t)0);
	}

	/* scratch buffer for every /proc read, reused across frames */
	buff_t buff = { 0 };
	snapshot_t previous = { 0 };
	int result = 0;

	if (inputs.format == FORMAT_TREE && read_system(&inputs, &buff) != 0) {
		perror("read_system");
		result = 1;
	}

	while (result == 0) {
		result = refresh(&inputs, inputs.watch && terminal,
				 inputs.watch && inputs.format == FORMAT_TREE ?
					 &previous :
					 NULL,
				 display_locale, &buff);

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
	}

	free(previous.procs);
	free(buff.data);

	if (display_locale) {
		freelocale(display_locale);
	}

	if (inputs.use_filter) {
		regfree(&inputs.filter);
	}

	return result;
}
