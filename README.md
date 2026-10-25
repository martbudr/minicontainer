# minicontainer

A small Linux container runtime written in C++20 using Linux system calls directly.
It runs a command in PID, UTS, and mount namespaces, provides a separate rootfs,
and optionally limits resources through cgroups v2. An educational project for
trusted workloads; it is not a production sandbox.

## Requirements

- Linux 5.14+ with PID/UTS/mount namespaces, `pidfd_open`, and `cgroup.kill`.
- Root privileges and a writable cgroups v2 hierarchy at `/sys/fs/cgroup`,
  with the requested resource controllers available.
- CMake 3.20+, a C++20 compiler, and Make or Ninja.

Build and run inside Linux. On macOS, use a Linux VM such as Lima.

## Quick Start on Linux

Install dependencies on Ubuntu/Debian, then build:

```bash
sudo apt-get update
sudo apt-get install -y build-essential cmake procps hostname coreutils libc-bin
cmake -S . -B build
cmake --build build
```

Create a minimal rootfs and run a command:

```bash
bash scripts/create_rootfs.sh ./rootfs
sudo ./build/minicontainer run ./rootfs /bin/echo hello
```

Expected output: `hello`, exit code `0`. The helper copies host Linux binaries
and their libraries and refuses to overwrite an existing path. Reuse your rootfs
on later runs. Custom executables need their loaders and libraries inside it.

## Development on macOS with Lima

From the project directory on your Mac:

```bash
brew install lima
limactl start --name=minicontainer --vm-type=vz \
    --cpus=2 --memory=2 --disk=10 --containerd=none \
    --mount-only="$(pwd -P):w" --yes template:ubuntu
limactl shell --workdir="$(pwd -P)" minicontainer
```

For an existing VM, use `limactl start minicontainer` instead of creating one.
Inside the VM, follow the Linux Quick Start. Sources are shared with the Mac;
use a fresh Linux build directory if an existing one contains a macOS CMake cache.
Exit the VM shell and run `limactl stop minicontainer` on macOS to stop it.

## Command-Line Interface

```text
minicontainer run [options] <rootfs> <command> [args...]
```

| Option | Accepted values | Default |
| --- | --- | --- |
| `--hostname` | Nonempty name, at most 64 bytes | `minicontainer` |
| `--memory` | Positive bytes, optionally suffixed with `K`, `M`, or `G` | No explicit limit |
| `--cpus` | Decimal CPU count, `0.01`–`1024` | No explicit limit |
| `--pids` | Integer, `2`–`2147483647` | No explicit limit |

Options precede the rootfs; `--` ends option parsing. The rootfs must be an
existing host directory other than `/`, with real, nonsymlink `proc` and `dev`
directories. Commands resolve inside it, starting from `/`, with no `PATH`
search. Arguments and the runtime's environment are passed to the command;
shell syntax requires an explicit shell.

Open a shell with resource limits:

```bash
sudo ./build/minicontainer run \
    --hostname box --memory 256M --cpus 0.5 --pids 64 \
    ./rootfs /bin/bash
```

Inside, `hostname` shows `box`, `pwd` shows `/`, and `ps aux` shows the container's
processes. To check exit-code propagation:

```bash
sudo ./build/minicontainer run ./rootfs /bin/sh -c 'exit 42'
echo "$?"  # 42
```

## Architecture

1. The host runtime creates a cgroup and uses `clone()` to create a child in
   new PID, UTS, and mount namespaces.
2. The child waits on a startup pipe until the host attaches it to the cgroup.
3. As container PID 1, it sets the hostname, makes mounts private, enters the
   rootfs with `chroot()`, and mounts fresh `/proc`, `/dev`, and `/dev/pts`.
4. PID 1 forks the command, which calls `execve()`. It stays alive to forward
   signals and reap exited children, including adopted orphans.
5. The host supervises PID 1 and removes the cgroup after termination.

Both supervisors consume signals through `signalfd()`. Standard streams are
inherited. The rootfs remains writable and persistent after exit; namespace
mounts disappear when the namespace is destroyed.

## Resource Limits and Shutdown

- **Memory:** suffixes use powers of 1024. A limit disables cgroup swap and
  enables group OOM killing.
- **CPU:** bandwidth across all threads, using a 100 ms period. `--cpus 0.5`
  allows 50 ms of aggregate CPU time per period, without pinning CPUs.
- **PIDs:** counts threads as well as processes; PID 1 and the command use two slots.

Each run creates a cgroup even without explicit limits. Ancestor restrictions
still apply. Requested controllers may be enabled at the hierarchy root;
that enablement remains after cleanup.

SIGINT/SIGTERM is forwarded through PID 1 to the workload. A second shutdown
signal or a two-second timeout forces termination. When the main command exits,
PID 1 exits and Linux kills remaining namespace processes. Parent-death
protection also kills PID 1 if the host runtime dies, though an empty cgroup
may remain after an abrupt host-runtime exit.

| Outcome | Exit code |
| --- | --- |
| Command exits | Command's exit code |
| Command killed by signal | `128 + signal number` |
| Runtime receives SIGINT / SIGTERM | `130` / `143` |
| Executable cannot start | `127` |
| Argument, setup, or cleanup error | `1` |

Runtime shutdown codes override the command's result; cleanup errors can return `1`.

## Project Structure

| Files | Responsibility |
| --- | --- |
| `src/main.cpp`, `src/cli.*`, `src/config.hpp` | Entry point, parsing, shared configuration |
| `src/container.*` | Host-side startup, supervision, cleanup |
| `src/container_init.*` | Container setup and PID 1 supervision |
| `src/rootfs.*` | Rootfs validation, mounts, devices |
| `src/cgroup.*` | Resource limits and cgroup management |
| `src/process_signals.*` | Signal and exit-status helpers |
| `src/setup_error.cpp` | Inline fatal setup-error helper |
| `scripts/create_rootfs.sh` | Minimal rootfs preparation |
| `CMakeLists.txt`, `Makefile` | Linux build configuration |

No automated test suite is currently included or registered with CMake.

## Troubleshooting

| Problem | Check |
| --- | --- |
| CMake rejects the platform | Build inside Linux with a Linux CMake cache. |
| Namespace or mount permission denied | Run with `sudo`; restricted containers may require using a Linux VM instead. |
| Cgroup setup fails | Check writable cgroups v2 access, controller availability, and `cgroup.kill`. |
| Existing executable reports “no such file” | Check its in-rootfs path, ELF loader, libraries, or script interpreter. |

## Limitations and Next Steps

Workloads run as privileged root, with no capability dropping, user namespace,
seccomp, or comprehensive inherited-descriptor cleanup. `chroot()` alone is
not a security boundary: **do not run untrusted programs**.

Network and IPC namespaces are shared with the Linux host. There is no disk/I/O
limit, overlay filesystem, image management, daemon, or managed console.
Programs and DNS configuration depend on the supplied rootfs.

Planned improvements include `pivot_root()` with old-root detachment, capability
and descriptor restrictions, seccomp, and network isolation.

## References

- [cgroups v2](https://docs.kernel.org/admin-guide/cgroup-v2.html)
- [PID namespaces](https://man7.org/linux/man-pages/man7/pid_namespaces.7.html)
- [`clone()`](https://man7.org/linux/man-pages/man2/clone.2.html)
- [`chroot()`](https://man7.org/linux/man-pages/man2/chroot.2.html)
- [`signalfd()`](https://man7.org/linux/man-pages/man2/signalfd.2.html)
- [Lima usage](https://lima-vm.io/docs/usage/)
