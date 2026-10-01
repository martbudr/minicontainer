#!/usr/bin/env bash
set -euo pipefail

if (( EUID != 0 )); then
    if ! sudo -n true 2>/dev/null; then
        echo "SKIP: control tests require root or passwordless sudo"
        exit 77
    fi
    exec sudo -n bash "$0" "$@"
fi
runtime=$1
test_case=$2
rootfs=$3
directory=$(mktemp -d)
runtime_pid=
cgroup=

cleanup() {
    if [[ -n $runtime_pid ]]; then
        kill -TERM "$runtime_pid" 2>/dev/null || true
        wait "$runtime_pid" 2>/dev/null || true
    fi
    if [[ -n $cgroup && -d $cgroup ]]; then
        echo 1 > "$cgroup/cgroup.kill"
        rmdir "$cgroup"
    fi
    rm -rf "$directory"
}
trap cleanup EXIT

fail() {
    echo "$1" >&2
    cat "$directory/output" 2>/dev/null || true
    exit 1
}

start() {
    local mode=$1
    shift
    "$runtime" run "$@" "$rootfs" /test_workload "$mode" > "$directory/output" 2>&1 &
    runtime_pid=$!
    for ((attempt = 0; attempt < 150; ++attempt)); do
        local groups=(/sys/fs/cgroup/minicontainer-"$runtime_pid"-*)
        if [[ -d ${groups[0]} && $(cat "$directory/output") == *ready* ]]; then
            cgroup=${groups[0]}
            return
        fi
        kill -0 "$runtime_pid" 2>/dev/null || fail "Runtime exited before readiness"
        sleep 0.02
    done
    fail "Runtime did not become ready"
}

finish() {
    local expected=$1 status=0
    wait "$runtime_pid" || status=$?
    runtime_pid=
    [[ $status -eq $expected ]] || fail "Expected exit $expected, got $status"
    [[ ! -d $cgroup ]] || fail "Cgroup was not deleted: $cgroup"
}

case "$test_case" in
    limits)
        start cpu --memory 256M --cpus 0.5 --pids 64
        [[ $(cat "$cgroup/memory.max") == 268435456 ]]
        [[ $(cat "$cgroup/memory.swap.max") == 0 ]]
        [[ $(cat "$cgroup/cpu.max") == '50000 100000' ]]
        [[ $(cat "$cgroup/pids.max") == 64 ]]
        kill -TERM "$runtime_pid"
        finish 143
        ;;
    cpu)
        start cpu --cpus 0.25
        read -r _ before < "$cgroup/cpu.stat"
        started=$(date +%s%N)
        sleep 2
        read -r _ after < "$cgroup/cpu.stat"
        elapsed=$(( ($(date +%s%N) - started) / 1000 ))
        # Allow scheduling/accounting jitter while rejecting an unconstrained CPU hog.
        (( (after - before) * 100 < elapsed * 40 )) || fail "CPU budget was exceeded"
        throttled=0
        while read -r key value; do
            [[ $key != nr_throttled ]] || throttled=$value
        done < "$cgroup/cpu.stat"
        (( throttled > 0 )) || fail "CPU hog was never throttled"
        kill -TERM "$runtime_pid"
        finish 143
        ;;
    memory)
        start memory --memory 32M
        [[ $(cat "$cgroup/memory.max") == 33554432 ]]
        [[ $(cat "$cgroup/memory.oom.group") == 1 ]]
        finish 137
        ;;
    pids)
        output=$("$runtime" run --pids 8 "$rootfs" /test_workload pids)
        [[ $output == children=6 ]]
        ;;
    sigint|sigterm|escalation)
        mode=signal
        [[ $test_case != escalation ]] || mode=ignore
        start "$mode"
        mapfile -t processes < "$cgroup/cgroup.procs"
        [[ ${#processes[@]} -ge 3 ]] || fail "Descendant was not created"
        if [[ $test_case == sigint ]]; then
            kill -INT "$runtime_pid"
            finish 130
        else
            kill -TERM "$runtime_pid"
            finish 143
        fi
        for pid in "${processes[@]}"; do
            [[ ! -e /proc/$pid ]] || fail "Container process survived: $pid"
        done
        ;;
    parent_death)
        start ignore
        mapfile -t processes < "$cgroup/cgroup.procs"
        kill -KILL "$runtime_pid"
        status=0
        wait "$runtime_pid" || status=$?
        runtime_pid=
        [[ $status -eq 137 ]] || fail "Runtime was not killed"
        for ((attempt = 0; attempt < 100; ++attempt)); do
            alive=0
            for pid in "${processes[@]}"; do
                [[ ! -e /proc/$pid ]] || alive=1
            done
            (( alive != 0 )) || break
            sleep 0.02
        done
        (( alive == 0 )) || fail "Container survived runtime SIGKILL"
        # SIGKILL cannot run the runtime's destructor; the test trap removes its group.
        ;;
    zombies)
        "$runtime" run "$rootfs" /test_workload zombies
        ;;
    descendants)
        before=$(find /sys/fs/cgroup -maxdepth 1 -name 'minicontainer-*' | sort)
        "$runtime" run "$rootfs" /test_workload descendant
        [[ $(find /sys/fs/cgroup -maxdepth 1 -name 'minicontainer-*' | sort) == "$before" ]]
        ;;
    signal_exit)
        status=0
        "$runtime" run "$rootfs" /test_workload self_signal || status=$?
        [[ $status -eq 143 ]]
        ;;
    invalid_limits)
        before=$(find /sys/fs/cgroup -maxdepth 1 -name 'minicontainer-*' | sort)
        for option_value in '--memory 0' '--memory -1' '--memory 2T' '--cpus nan' '--cpus 0' '--cpus 0.001' '--pids 1' '--pids 3.5' '--unknown 4'; do
            read -r option value <<< "$option_value"
            if "$runtime" run "$option" "$value" "$rootfs" /bin/true; then
                fail "Invalid limit was accepted: $option_value"
            fi
        done
        [[ $(find /sys/fs/cgroup -maxdepth 1 -name 'minicontainer-*' | sort) == "$before" ]]
        ;;
    startup_failure)
        before=$(find /sys/fs/cgroup -maxdepth 1 -name 'minicontainer-*' | sort)
        if "$runtime" run --memory 32M "$rootfs" /not/an/executable; then
            fail "Missing executable was accepted"
        fi
        [[ $(find /sys/fs/cgroup -maxdepth 1 -name 'minicontainer-*' | sort) == "$before" ]]
        ;;
    *) fail "Unknown test: $test_case" ;;
esac
