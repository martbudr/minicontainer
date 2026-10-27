#!/usr/bin/env bash
set -u

if (( EUID != 0 )); then
    if ! sudo -n true 2>/dev/null; then
        echo "SKIP: namespace tests require root or passwordless sudo"
        exit 77
    fi
    exec sudo -n bash "$0" "$@"
fi

runtime=$1
test_case=$2
rootfs=$3

fail() {
    printf '%s\n' "$1" >&2
    exit 1
}

case "$test_case" in
    basic_execution)
        output=$("$runtime" run "$rootfs" /bin/echo hello)
        status=$?
        [[ $status -eq 0 ]] || fail "Expected exit code 0, got $status"
        [[ "$output" == "hello" ]] || fail "Expected stdout 'hello', got '$output'"
        ;;
    arguments)
        output=$("$runtime" run "$rootfs" /bin/echo hello world)
        status=$?
        [[ $status -eq 0 ]] || fail "Expected exit code 0, got $status"
        [[ "$output" == "hello world" ]] || fail "Expected stdout 'hello world', got '$output'"
        ;;
    exit_code)
        "$runtime" run "$rootfs" /bin/sh -c 'exit 42'
        status=$?
        [[ $status -eq 42 ]] || fail "Expected exit code 42, got $status"
        ;;
    invalid_executable)
        "$runtime" run "$rootfs" /does/not/exist
        status=$?
        [[ $status -gt 0 && $status -lt 128 ]] || fail "Expected non-zero exit without a crash, got $status"
        ;;
    invalid_invocation)
        output=$("$runtime" 2>&1)
        status=$?
        [[ $status -gt 0 && $status -lt 128 ]] || fail "Expected non-zero exit without a crash, got $status"
        [[ "$output" == *"usage:"*"<command> [args...]"* ]] || fail "Expected usage message, got '$output'"
        ;;
    *)
        fail "Unknown test case: $test_case"
        ;;
esac
