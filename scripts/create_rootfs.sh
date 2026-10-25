#!/usr/bin/env bash
set -euo pipefail

if [[ $(uname -s) != Linux || $# -ne 1 ]]; then
    echo "Usage (on Linux): bash $0 <new-rootfs-directory>" >&2
    exit 1
fi
destination=$1
if [[ -e "$destination" || -L "$destination" ]]; then
    echo "Refusing to overwrite existing rootfs: $destination" >&2
    exit 1
fi
mkdir -p "$destination"/{bin,proc,dev,etc,tmp,root,usr/bin}
chmod 1777 "$destination/tmp"

# Copy dereferenced executables and the loader/shared libraries reported by ldd.
for executable in /bin/{bash,sh,echo,ls,hostname,ps,cat,true} /usr/bin/readlink; do
    cp -L --parents "$executable" "$destination"
    dependencies=$(ldd "$executable")
    while IFS= read -r line; do
        if [[ $line =~ (/[^[:space:]]+) ]]; then
            cp -L --parents "${BASH_REMATCH[1]}" "$destination"
        fi
    done <<< "$dependencies"
done
printf 'root:x:0:0:root:/root:/bin/bash\n' > "$destination/etc/passwd"
printf 'root:x:0:\n' > "$destination/etc/group"
printf 'passwd: files\ngroup: files\nhosts: files\n' > "$destination/etc/nsswitch.conf"
printf '127.0.0.1 localhost\n' > "$destination/etc/hosts"
echo "Created minimal rootfs: $destination"
