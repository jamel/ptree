#!/usr/bin/env python3
"""Build in a temporary directory and report median phase timings as CSV."""
import argparse
import csv
import os
import platform
import shlex
import statistics
import subprocess
import sys
import tempfile
from pathlib import Path


def positive(value):
    number = int(value)
    if number < 1:
        raise argparse.ArgumentTypeError("must be positive")
    return number


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--sizes", nargs="+", type=positive, default=[1000, 5000, 10000])
    parser.add_argument("--samples", type=positive, default=7)
    parser.add_argument("--modes", nargs="+", choices=["list", "tree", "watch", "commands",
                                                     "commands-color", "redraw"],
                        default=["list", "tree", "watch", "commands", "commands-color", "redraw"])
    args = parser.parse_args()
    if max(args.sizes) > 1000000:
        parser.error("sizes must not exceed 1000000")
    root = Path(__file__).resolve().parent.parent
    compiler = shlex.split(os.environ.get("CC", "cc"))
    flags = shlex.split(os.environ.get("CFLAGS", "-std=c99 -Wall -Wextra -Wpedantic -O2"))
    print(f"# {platform.platform()}; build: {shlex.join(compiler + flags)}; "
          f"samples={args.samples}; LC_ALL=C.UTF-8", file=sys.stderr)
    writer = csv.writer(sys.stdout)
    writer.writerow(["mode", "size", "median_ms", "min_ms", "max_ms"])
    with tempfile.TemporaryDirectory(prefix="ptree-benchmark-") as directory:
        binary = Path(directory) / "benchmark"
        subprocess.run([*compiler, *flags, str(root / "bench/benchmark.c"),
                        *shlex.split(os.environ.get("LDFLAGS", "")), "-o", str(binary),
                        *shlex.split(os.environ.get("LDLIBS", ""))], check=True)
        for n in args.sizes:
            for mode in args.modes:
                command = [str(binary), mode, str(n)]
                if mode.startswith("commands"):
                    command.append("x" * 8192)  # Exercise dynamic buffer growth.
                samples = []
                for _ in range(args.samples + 1):
                    run = subprocess.run(command, stdout=subprocess.DEVNULL, stderr=subprocess.PIPE,
                                         text=True, check=True, env={**os.environ, "LC_ALL": "C.UTF-8"})
                    samples.append(float(run.stderr))
                samples = samples[1:]  # One untimed warmup run per case.
                writer.writerow([mode, n, *(f"{value:.6f}" for value in (
                    statistics.median(samples), min(samples), max(samples)))])
                sys.stdout.flush()


if __name__ == "__main__":
    main()
