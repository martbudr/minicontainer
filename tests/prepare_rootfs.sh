#!/usr/bin/env bash
set -euo pipefail
if [[ ! -d "$2" ]]; then
    bash "$1" "$2"
fi
printf 'container-only\n' > "$2/rootfs-marker"
