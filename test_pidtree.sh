#!/usr/bin/env bash
set -euo pipefail

make >/dev/null

root_pid=
cleanup() {
    if [[ -n "$root_pid" ]]; then
        pkill -TERM -P "$root_pid" 2>/dev/null || true
        kill "$root_pid" 2>/dev/null || true
        wait "$root_pid" 2>/dev/null || true
    fi
}
trap cleanup EXIT

bash -c 'sleep 30 & wait' &
root_pid=$!

for _ in {1..50}; do
    if ./pidtree "$root_pid" | awk 'END { exit NR >= 2 ? 0 : 1 }'; then
        break
    fi
    sleep 0.05
done

newline_output=$(./pidtree "$root_pid")
if ! printf '%s\n' "$newline_output" | awk 'END { exit NR >= 2 ? 0 : 1 }'; then
    printf 'test process tree did not expose a child process\n' >&2
    exit 1
fi

expected=$(printf '%s\n' "$newline_output" | paste -sd, -)

actual_short=$(./pidtree -c "$root_pid")
if [[ "$actual_short" != "$expected" ]]; then
    printf 'expected -c output: %s\nactual -c output:   %s\n' "$expected" "$actual_short" >&2
    exit 1
fi

actual_long=$(./pidtree --comma "$root_pid")
if [[ "$actual_long" != "$expected" ]]; then
    printf 'expected --comma output: %s\nactual --comma output:   %s\n' "$expected" "$actual_long" >&2
    exit 1
fi

if [[ "$actual_short" == *, ]]; then
    printf 'comma output has trailing comma: %s\n' "$actual_short" >&2
    exit 1
fi

if [[ "$actual_short" != *,* ]]; then
    printf 'comma output did not contain a delimiter: %s\n' "$actual_short" >&2
    exit 1
fi
