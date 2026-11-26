#!/usr/bin/env bash
set -euo pipefail

make >/dev/null

root_pid=
filter_roots=()
fixture_dir=$(mktemp -d)
cleanup() {
    for pid in "${filter_roots[@]}"; do
        for child in $(pgrep -P "$pid" || true); do
            pkill -TERM -P "$child" 2>/dev/null || true
            kill "$child" 2>/dev/null || true
        done
        kill "$pid" 2>/dev/null || true
        wait "$pid" 2>/dev/null || true
    done
    if [[ -n "$root_pid" ]]; then
        pkill -TERM -P "$root_pid" 2>/dev/null || true
        kill "$root_pid" 2>/dev/null || true
        wait "$root_pid" 2>/dev/null || true
    fi
    rm -rf "$fixture_dir"
}
trap cleanup EXIT

bash -c 'sleep 30 & wait' &
root_pid=$!

for _ in {1..50}; do
    if ./pidtree -p "$root_pid" | awk 'END { exit NR >= 2 ? 0 : 1 }'; then
        break
    fi
    sleep 0.05
done

newline_output=$(./pidtree -p "$root_pid")
if ! printf '%s\n' "$newline_output" | awk 'END { exit NR >= 2 ? 0 : 1 }'; then
    printf 'test process tree did not expose a child process\n' >&2
    exit 1
fi

expected=$(printf '%s\n' "$newline_output" | paste -sd, -)

actual_pid_long=$(./pidtree --pid="$root_pid")
if [[ "$actual_pid_long" != "$newline_output" ]]; then
    printf 'expected --pid output: %s\nactual --pid output:   %s\n' "$newline_output" "$actual_pid_long" >&2
    exit 1
fi

actual_short=$(./pidtree -c -p "$root_pid")
if [[ "$actual_short" != "$expected" ]]; then
    printf 'expected -c output: %s\nactual -c output:   %s\n' "$expected" "$actual_short" >&2
    exit 1
fi

actual_long=$(./pidtree --comma --pid "$root_pid")
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

# Two matching roots, each with a matching child and a nonmatching grandchild.
# The token occurs only in argv, and overlapping matches must not duplicate PIDs.
token="pidtree-filter-$$-$RANDOM"
printf -v padding '%05000d' 0
for name in first second; do
    bash -c '
        bash -c '\''sleep 60 & printf "%s\n" "$BASHPID" "$!" > "$1"; wait'\'' "$0-child" "$1" &
        wait
    ' "$token-$name" "$fixture_dir/$name" "$padding" "$token-tail" &
    filter_roots+=("$!")
done

for _ in {1..50}; do
    if [[ -s "$fixture_dir/first" && -s "$fixture_dir/second" ]]; then
        break
    fi
    sleep 0.05
done
expected_filter=$(printf '%s\n' "${filter_roots[@]}"; cat "$fixture_dir/first" "$fixture_dir/second")
expected_filter=$(printf '%s\n' "$expected_filter" | sort -n)

for flag in -f --filter; do
    actual=$(./pidtree "$flag" "$token" | sort -n)
    if [[ "$actual" != "$expected_filter" ]]; then
        printf 'incorrect %s process forest\nexpected:\n%s\nactual:\n%s\n' "$flag" "$expected_filter" "$actual" >&2
        exit 1
    fi
done

# Full-command-line matching joins argv with spaces and supports extended regexes.
pattern=" $token-(first|second)(-child)? "
actual=$(./pidtree -f "$pattern" | sort -n)
[[ "$actual" == "$expected_filter" ]]

# A match beyond the initial read buffer still selects both entire trees.
actual=$(./pidtree -f "$token-tail$" | sort -n)
[[ "$actual" == "$expected_filter" ]]

expected_first=$(printf '%s\n' "${filter_roots[0]}"; cat "$fixture_dir/first")
actual=$(./pidtree -f "$token-first " | sort -n)
[[ "$actual" == "$(printf '%s\n' "$expected_first" | sort -n)" ]]

for args in short long; do
    if [[ "$args" == short ]]; then
        actual=$(./pidtree -c -f "$token")
    else
        actual=$(./pidtree --filter="$token" --comma)
    fi
    [[ "$actual" != *$'\n'* && "$actual" != ,* && "$actual" != *, ]]
    [[ "$(printf '%s\n' "$actual" | tr ',' '\n' | sort -n)" == "$expected_filter" ]]
done

expect_status() {
    local expected_status=$1
    shift
    local status=0
    ./pidtree "$@" > "$fixture_dir/stdout" 2> "$fixture_dir/stderr" || status=$?
    if [[ "$status" != "$expected_status" ]]; then
        printf 'expected status %s, got %s for %s\n' "$expected_status" "$status" "$*" >&2
        exit 1
    fi
    [[ ! -s "$fixture_dir/stdout" ]]
}

expect_status 1 -f "$token-missing"
expect_status 1 -f '^\./pidtree -f '
expect_status 2 -f '['
expect_status 2 -f
expect_status 2 --filter
expect_status 2 -f "$token" "$root_pid"
expect_status 2
expect_status 2 "$root_pid"
expect_status 2 -- "$root_pid"
expect_status 2 -p
expect_status 2 --pid
expect_status 2 -p "$root_pid" "$root_pid"
expect_status 2 -p "$root_pid" -f "$token"
expect_status 2 -f "$token" -p "$root_pid"
for pid in '' 0 -1 abc 2147483648; do
    expect_status 2 -p "$pid"
done
