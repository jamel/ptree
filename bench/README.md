# Benchmarks

Run `make bench`, or capture CSV directly:

```sh
python3 bench/run.py > before.csv
python3 bench/run.py --sizes 1000 10000 --samples 11 > after.csv
```

The runner honors `CC`, `CFLAGS`, `LDFLAGS`, and `LDLIBS`, builds in a temporary
directory, warms each case once, and reports median/min/max milliseconds. Use
the same compiler, flags, machine, and sizes for comparisons. Timing is not a
pass/fail test; host load affects it.

- `list`, `tree`, `watch`: select and render a balanced synthetic tree to
  `/dev/null`, including topology preparation. Watch also matches a previous
  snapshot. Setup/allocation of the input snapshots is outside the timer.
  Synthetic PIDs start at 100000000; the benchmark fails their command opens
  without a syscall, so rows exercise the name fallback and no `/proc` lookup
  is timed. `/proc` collection and real command reading are excluded; rendering
  still reads real system metadata. These are phase timings, not end-to-end
  process-monitor performance.
- `commands`, `commands-color`: read the benchmark's actual command line N
  times, including an 8 KiB argument, without/with command-name extraction.
- `redraw`: redraw N small styled Unicode frames to `/dev/null`, including
  locale work and terminal-size fallback. There is no terminal emulator cost.

Default sizes are 1000, 5000, and 10000; default samples are 7. Use `--modes`
to select cases. CSV goes to stdout; build/environment details go to stderr.
