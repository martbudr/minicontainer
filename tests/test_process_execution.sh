#!/usr/bin/env bash
set -u

runtime=$1
test_case=$2

fail() {
    printf '%s\n' "$1" >&2
    exit 1
}

case "$test_case" in
    basic_execution)
        output=$("$runtime" run /bin/echo hello)
        status=$?
        [[ $status -eq 0 ]] || fail "Expected exit code 0, got $status"
        [[ "$output" == "hello" ]] || fail "Expected stdout 'hello', got '$output'"
        ;;
    arguments)
        output=$("$runtime" run /bin/echo hello world)
        status=$?
        [[ $status -eq 0 ]] || fail "Expected exit code 0, got $status"
        [[ "$output" == "hello world" ]] || fail "Expected stdout 'hello world', got '$output'"
        ;;
    exit_code)
        "$runtime" run /bin/sh -c 'exit 42'
        status=$?
        [[ $status -eq 42 ]] || fail "Expected exit code 42, got $status"
        ;;
    invalid_executable)
        "$runtime" run /does/not/exist
        status=$?
        [[ $status -gt 0 && $status -lt 128 ]] || fail "Expected non-zero exit without a crash, got $status"
        ;;
    invalid_invocation)
        output=$("$runtime" 2>&1)
        status=$?
        [[ $status -gt 0 && $status -lt 128 ]] || fail "Expected non-zero exit without a crash, got $status"
        [[ "$output" == *"usage:"*"run <command> [args...]"* ]] || fail "Expected usage message, got '$output'"
        ;;
    *)
        fail "Unknown test case: $test_case"
        ;;
esac
