#!/usr/bin/env bash
set -eu

if (( EUID != 0 )); then
    if ! sudo -n true 2>/dev/null; then
        echo "SKIP: namespace tests require root or passwordless sudo"
        exit 77
    fi
    exec sudo -n bash "$0" "$@"
fi

runtime=$1
test_case=$2

case "$test_case" in
    hostname)
        actual=$("$runtime" run --hostname box /bin/hostname)
        [[ "$actual" == box ]]
        ;;
    pid1)
        "$runtime" run /bin/bash -c '[[ $$ -eq 1 ]]'
        ;;
    proc)
        "$runtime" run /bin/bash -c '
            processes=(/proc/[0-9]*)
            [[ ${#processes[@]} -eq 1 && ${processes[0]} == /proc/1 ]] || exit 1
            while read -r key value rest; do
                if [[ $key == Pid: ]]; then
                    [[ $value -eq 1 ]] || exit 1
                    exit 0
                fi
            done < /proc/self/status
            exit 1'
        ;;
    namespaces)
        for namespace in pid uts mnt; do
            host_namespace=$(readlink "/proc/self/ns/$namespace")
            child_namespace=$("$runtime" run /usr/bin/readlink "/proc/self/ns/$namespace")
            [[ "$host_namespace" != "$child_namespace" ]]
        done
        ;;
    host_unchanged)
        hostname_before=$(hostname)
        mounts_before=$(cat /proc/self/mountinfo)
        "$runtime" run --hostname box /bin/bash -c 'hostname changed-inside'
        [[ $(hostname) == "$hostname_before" ]]
        [[ $(cat /proc/self/mountinfo) == "$mounts_before" ]]
        ;;
    invalid_hostname)
        if "$runtime" run --hostname '' /bin/true; then
            echo "Empty hostname unexpectedly accepted" >&2
            exit 1
        fi
        if "$runtime" run --hostname box; then
            echo "Missing executable unexpectedly accepted" >&2
            exit 1
        fi
        ;;
    *)
        echo "Unknown test case: $test_case" >&2
        exit 1
        ;;
esac
