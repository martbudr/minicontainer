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

## Build and Run

Run these commands from the project directory inside Linux.

### Build

```bash
cmake -S . -B build
cmake --build build
```

### Run with the Example Rootfs

`rootfs/` is generated locally and is not included in the repository. If it is
missing after cloning, prepare it inside Linux:

```bash
bash scripts/create_rootfs.sh ./rootfs
```

Use the existing `./rootfs` to run a command:

```bash
sudo ./build/minicontainer run ./rootfs /bin/echo hello
```

Expected output: `hello`, exit code `0`. To open an interactive shell:

```bash
sudo ./build/minicontainer run ./rootfs /bin/bash
```

Run `exit` to leave the container.

### Customize the Run

Set a hostname and resource limits before the rootfs path:

```bash
sudo ./build/minicontainer run \
    --hostname box --memory 256M --cpus 0.5 --pids 64 \
    ./rootfs /bin/bash
```

Inside, `hostname` shows `box`, `pwd` shows `/`, and `ps aux` shows the container's
processes. See [Command-Line Interface](#command-line-interface) for all options.

Replace `./rootfs` to use another rootfs, or `/bin/bash` to run another command.
Executables, their loaders, and libraries must exist inside the chosen rootfs;
changes to its files persist after exit.

### Create Another Rootfs

Inside Linux, run the helper with a destination that does not already exist.
It copies a small set of host executables and their loaders and shared libraries:

```bash
bash scripts/create_rootfs.sh ./my-rootfs
sudo ./build/minicontainer run ./my-rootfs /bin/bash
```

To add programs later, copy their executables and required libraries into the
corresponding paths inside the rootfs.

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

To check exit-code propagation:

```bash
sudo ./build/minicontainer run ./rootfs /bin/sh -c 'exit 42'
echo "$?"  # 42
```

## Tests

Build first, then run the Linux integration suite:

```bash
ctest --test-dir build --output-on-failure
```

CTest prepares a separate `build/test-rootfs` fixture without modifying `./rootfs`.
Tests require the runtime's Linux/root/cgroups prerequisites. Privileged checks
use passwordless sudo or are marked skipped if unavailable. To allow a sudo prompt:

```bash
sudo ctest --test-dir build --output-on-failure
```

| Script (`tests/`) | Coverage |
| --- | --- |
| `test_process_execution.sh` | Output, arguments, exit codes, missing executable, invalid invocation |
| `test_namespace_isolation.sh` | Hostname, PID 1 supervision, isolated proc and namespaces, unchanged host state |
| `test_rootfs.sh` | Root and working directory, hidden host paths, proc, devices, invalid rootfs |
| `test_control.sh` | Resource limits under load, signal forwarding, forced shutdown, parent death, orphan reaping, process and cgroup cleanup |

Run only the control tests:

```bash
ctest --test-dir build -L milestone4 --output-on-failure
```

To omit tests when configuring, use `-DBUILD_TESTING=OFF`.

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
| [main.cpp](src/main.cpp), [cli.cpp](src/cli.cpp), [config.hpp](src/config.hpp) | Entry point, parsing, shared configuration |
| [container.cpp](src/container.cpp) | Host-side startup, supervision, cleanup |
| [container_init.cpp](src/container_init.cpp) | Container setup and PID 1 supervision |
| [rootfs.cpp](src/rootfs.cpp) | Rootfs validation, mounts, devices |
| [cgroup.cpp](src/cgroup.cpp) | Resource limits and cgroup management |
| [process_signals.cpp](src/process_signals.cpp) | Signal and exit-status helpers |
| [setup_error.cpp](src/setup_error.cpp) | Inline fatal setup-error helper |
| [create_rootfs.sh](scripts/create_rootfs.sh) | Minimal rootfs preparation |
| [tests/](tests/) | Integration checks, test workload, and test-rootfs preparation |
| [CMakeLists.txt](CMakeLists.txt) | Build and CTest configuration |

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

## macOS Development (Optional)

Use a Linux VM such as [Lima](https://lima-vm.io/docs/usage/) to build and run
this project on macOS. Make the project directory writable inside the VM, then
follow [Build and Run](#build-and-run) from its Linux shell. Use a separate Linux
build directory if `build/` contains a macOS CMake cache, and prepare the rootfs
inside the VM so its executables match the Linux environment.

## References

- [cgroups v2](https://docs.kernel.org/admin-guide/cgroup-v2.html)
- [PID namespaces](https://man7.org/linux/man-pages/man7/pid_namespaces.7.html)
- [`clone()`](https://man7.org/linux/man-pages/man2/clone.2.html)
- [`chroot()`](https://man7.org/linux/man-pages/man2/chroot.2.html)
- [`signalfd()`](https://man7.org/linux/man-pages/man2/signalfd.2.html)
- [Lima usage](https://lima-vm.io/docs/usage/)
