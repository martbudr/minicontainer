#!/usr/bin/env bash
set -euo pipefail

if (( EUID != 0 )); then
    if ! sudo -n true 2>/dev/null; then
        echo "SKIP: rootfs tests require root or passwordless sudo"
        exit 77
    fi
    exec sudo -n bash "$0" "$@"
fi

runtime=$1
test_case=$2
rootfs=$3

case "$test_case" in
    root)
        [[ $("$runtime" run "$rootfs" /bin/cat /rootfs-marker) == container-only ]]
        ;;
    cwd)
        "$runtime" run "$rootfs" /bin/bash -c '[[ $PWD == / && $(pwd -P) == / ]]'
        ;;
    host_hidden)
        marker=$(mktemp /tmp/minicontainer-host.XXXXXX)
        trap 'rm -f "$marker"' EXIT
        "$runtime" run "$rootfs" /bin/bash -c '[[ ! -e $1 && ! -e /proc/1/root$1 ]]' bash "$marker"
        ;;
    proc)
        "$runtime" run "$rootfs" /bin/bash -c '
            [[ -e /proc/1/status && -e /proc/self/mountinfo ]] || exit 1
            [[ $(readlink /proc/1/root) == / ]] || exit 1
            processes=(/proc/[0-9]*)
            [[ ${#processes[@]} -eq 2 && ${processes[0]} == /proc/1 && ${processes[1]} == /proc/2 ]]'
        ;;
    devices)
        "$runtime" run "$rootfs" /bin/bash -c '
            [[ -c /dev/null && -c /dev/zero && -c /dev/urandom && -c /dev/ptmx ]] || exit 1
            echo hello > /dev/null
            [[ $(cat /dev/fd/0) == input ]]' <<< input
        ;;
    invalid_rootfs)
        for invalid in / /does/not/exist "$rootfs/bin/bash"; do
            if "$runtime" run "$invalid" /bin/true; then
                echo "Invalid rootfs accepted: $invalid" >&2
                exit 1
            fi
        done
        if "$runtime" run "$rootfs"; then
            echo "Missing command accepted" >&2
            exit 1
        fi
        ;;
    *) echo "Unknown test: $test_case" >&2; exit 1 ;;
esac
