#!/usr/bin/env bash
set -euo pipefail
if [[ ! -d "$2" ]]; then
    bash "$1" "$2"
fi
printf 'container-only\n' > "$2/rootfs-marker"
cp "$3" "$2/test_workload"
dependencies=$(ldd "$3")
while IFS= read -r line; do
    if [[ $line =~ (/[^[:space:]]+) ]]; then
        cp -L --parents "${BASH_REMATCH[1]}" "$2"
    fi
done <<< "$dependencies"
