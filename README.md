# minicontainer

`minicontainer` is a small Linux container runtime written in C++20. It launches
a command with its own process tree, hostname, mount namespace, and root
filesystem, then supervises its lifetime and optionally limits memory, CPU,
and process usage through cgroups v2.

The project is built in milestones to make the underlying Linux mechanisms
visible. It uses system calls directly rather than Docker, containerd, an OCI
runtime, or a container-management library. It is an educational runtime for
trusted workloads, not a production sandbox.

```bash
sudo ./build/minicontainer run \
    --hostname box --memory 256M --cpus 0.5 --pids 64 \
    ./rootfs /bin/bash
```

## Features

| Area | Implementation |
| --- | --- |
| Processes | PID namespace, PID 1 supervisor, command execution with `execve()` |
| Hostname | UTS namespace with a configurable hostname |
| Mounts | Private mount namespace and propagation, container-specific `/proc` |
| Filesystem | Separate rootfs entered with `chroot()`, fresh `/dev` and `/dev/pts` |
| Resources | cgroups v2 memory, CPU bandwidth, and PID/thread limits |
| Lifecycle | SIGINT/SIGTERM forwarding, orphan reaping, forced shutdown, exit-code propagation |
| Cleanup | Descendant termination, namespace mount teardown, cgroup deletion |
| Tests | CTest suites for each completed milestone |

There is one command, `run`. There is no daemon, image registry, image format,
container database, or separate `start`, `stop`, or `exec` interface.

## Requirements

- A Linux build host and Linux target. CMake deliberately rejects other systems,
  including cross-compilation from macOS.
- Linux 5.14 or newer, with PID/UTS/mount namespaces, cgroups v2, `pidfd_open`,
  and `cgroup.kill` available.
- Root privileges at runtime for namespaces, mounts, `chroot()`, and cgroup setup.
- A writable cgroup v2 hierarchy at `/sys/fs/cgroup`. Requested resource
  controllers must be available there.
- CMake 3.20 or newer, a C++20 compiler, and a build tool such as Make or Ninja.
- Bash, GNU coreutils, `ldd`, `hostname`, and `ps` for the rootfs helper and tests.

The documented rootfs workflow has been exercised on Ubuntu in Lima on an
Apple Silicon Mac. The helper copies Linux executables and their libraries
from the machine where it runs, so the rootfs must match the guest architecture.

## Quick Start on Linux

From a checkout of this project, install dependencies. For Ubuntu/Debian:

```bash
sudo apt-get update
sudo apt-get install -y build-essential cmake procps hostname coreutils libc-bin
```

Build the runtime:

```bash
cmake -S . -B build
cmake --build build
```

Create a rootfs once, then execute a command:

```bash
bash scripts/create_rootfs.sh ./rootfs
sudo ./build/minicontainer run ./rootfs /bin/echo hello
```

Expected output is `hello`, with exit code `0`. The rootfs helper refuses to
overwrite an existing path. Reuse an existing rootfs or choose a new directory
when rebuilding it.

## Development on macOS with Lima

The runtime executes inside Linux; Lima provides that Linux VM. Install Lima
on macOS with Homebrew, following the [Lima installation guide](https://lima-vm.io/docs/installation/):

```bash
brew install lima
```

From the project directory on your Mac, create a VM if you do not already have
one named `minicontainer`:

```bash
limactl start --name=minicontainer --vm-type=vz \
    --cpus=2 --memory=2 --disk=10 --containerd=none \
    --mount-only="$(pwd -P):w" --yes template:ubuntu
```

This shares only the project directory, writable, at the same path inside the
VM. For an existing VM, start it with `limactl start minicontainer` instead.
Enter it from your Mac's project directory:

```bash
limactl shell --workdir="$(pwd -P)" minicontainer
```

Inside the VM, install the Linux dependencies shown above, then build and run:

```bash
cmake -S . -B build-linux
cmake --build build-linux
bash scripts/create_rootfs.sh ./rootfs
sudo ./build-linux/minicontainer run --hostname box ./rootfs /bin/bash
```

Skip rootfs creation if `./rootfs` already exists. Use a separate Linux build
directory to avoid mixing compiler caches and binaries with native macOS builds.
You can edit sources on your Mac and rebuild inside the VM. To stop the VM,
exit its shell and run `limactl stop minicontainer` on macOS.

The rest of this README uses `build`; substitute `build-linux` for the Lima
workflow.

## Command-Line Interface

```text
minicontainer run [options] <rootfs> <command> [args...]
```

| Option | Accepted values | Default |
| --- | --- | --- |
| `--hostname` | A nonempty name of at most 64 bytes | `minicontainer` |
| `--memory` | Positive integer bytes, optionally followed by uppercase `K`, `M`, or `G` | No explicit limit |
| `--cpus` | Decimal CPU count from `0.01` to `1024` | No explicit limit |
| `--pids` | Integer from `2` to `2147483647` | No explicit limit |

Options must precede the rootfs. An optional `--` ends option parsing. Arguments
after the command are passed to the program unchanged; shell syntax is interpreted
only when you explicitly run a shell.

The rootfs path is resolved on the Linux host. It must be an existing directory
other than `/`, containing real, nonsymlink `proc` and `dev` directories.
The command path is resolved **inside** the rootfs. Commands are executed with
`execve()`, so there is no `PATH` search: use `/bin/bash`, not just `bash`.
Relative paths resolve from the container's initial working directory, `/`.
The command inherits the environment passed to the runtime; with `sudo`, that
means the environment permitted by your sudo configuration.

### Examples

Pass arguments and inspect a command's exit code:

```bash
sudo ./build/minicontainer run ./rootfs /bin/echo hello world
sudo ./build/minicontainer run ./rootfs /bin/sh -c 'exit 42'
echo "$?"
```

Open an isolated shell:

```bash
sudo ./build/minicontainer run --hostname box ./rootfs /bin/bash
```

Inside that shell:

```bash
hostname    # box
pwd         # /
ls /        # rootfs directories, not the host's root directory
ps aux      # PID 1 supervisor, shell, and commands started inside the container
```

Apply resource limits:

```bash
sudo ./build/minicontainer run \
    --memory 256M --cpus 0.5 --pids 64 \
    ./rootfs /bin/bash
```

To run your own Linux executable, put it in the rootfs first:

```bash
cp ./program ./rootfs/bin/program
sudo ./build/minicontainer run ./rootfs /bin/program
```

If it is dynamically linked, also copy its loader and shared libraries to their
expected paths inside the rootfs. Inspect those dependencies with `ldd ./program`.
The rootfs helper's copy procedure is a useful reference. Only run `ldd` on
executables you trust.

## Architecture

```text
Linux host
  |
  +-- minicontainer parent
  |     validate arguments and rootfs
  |     create cgroup and configure limits
  |     clone namespace child
  |     attach child to cgroup, then release startup gate
  |     forward signals, wait, and delete cgroup
  |
  +-- namespace child: PID 1 supervisor
        set hostname and make mount propagation private
        bind mount rootfs, chroot, and change directory to /
        mount proc, tmpfs /dev, and a new devpts instance
        fork command and supervise its lifetime
          |
          +-- command: normally PID 2
                execve(command, arguments, environment)
                descendants remain in the same cgroup and namespaces
```

### Why These Choices?

**`clone()` creates the container boundary.** `CLONE_NEWPID`, `CLONE_NEWUTS`, and
`CLONE_NEWNS` give the namespace child separate process IDs, hostname state,
and mount state. This is still ordinary Linux process execution, not a separate
kernel or full virtual machine.

**A supervisor owns PID 1.** Namespace PID 1 has special signal and orphan-handling
responsibilities. Keeping it in the runtime allows arbitrary commands to run
without needing to implement init behavior themselves. The supervisor uses
`fork()` to create the workload, then `execve()` to replace that child with the
requested program. It reaps adopted descendants with `waitpid()`.

**`chroot()` is the current filesystem step.** The rootfs is bind-mounted in the
private mount namespace and becomes the process's root, followed by `chdir("/")`.
It is writable and persistent: files created inside it remain after exit.
There is no overlay or automatic rootfs reset. Replacing `chroot()` with
`pivot_root()` and detaching the old root is planned, not implemented.

**`/proc` must be remounted.** A fresh proc filesystem reflects the new PID
namespace. Mount propagation is made recursively private first so these mounts
do not propagate back to the host. `/dev` uses a small tmpfs with basic character
devices and a separate devpts instance; the host's `/dev` is not bind-mounted.
Standard streams are inherited, without a runtime-managed console or PTY proxy.

**Cgroup membership precedes setup.** The namespace child waits on a pipe until
the parent has attached it to the cgroup. Rootfs setup and subsequently forked
workloads are therefore accounted to the same group. The runtime creates a
unique `/sys/fs/cgroup/minicontainer-<host-pid>-<suffix>` directory for each run,
even when no resource limit is supplied.

**Signals are consumed through `signalfd()`.** Blocking SIGINT, SIGTERM, and
SIGCHLD lets the parent and supervisor handle them through polling loops.
PID 1 also installs signal handlers so Linux accepts its termination signals.
The workload restores default signal handling before `execve()`.

## Resource-Limit Semantics

Memory suffixes use powers of 1024: `256M` means 268435456 bytes. `--memory`
writes `memory.max`, disables swap for the group with `memory.swap.max=0`, and
sets `memory.oom.group=1` so an OOM event kills the group together.

CPU limits use `cpu.max` with a 100 ms period. `--cpus 0.5` permits 50 ms of
aggregate CPU time per period across the group's threads. This is bandwidth
control, not CPU pinning or a guarantee of CPU time under host contention; short
bursts within a period are possible.

PID limits use `pids.max` and count threads as well as processes. The supervisor
and initial command consume two slots, so `--pids 8` leaves room for six more
single-threaded processes. Further forks or thread creation fail when the limit
is reached.

Omitted limits retain kernel defaults and inherit any ancestor restrictions.
Requested controllers are enabled in the cgroup hierarchy root when necessary;
that controller enablement remains after the run. Per-run cgroup directories
are removed during cleanup. This direct hierarchy-management approach is for
development, not integration with a production service manager.

These settings follow the [kernel cgroup v2 interfaces](https://docs.kernel.org/admin-guide/cgroup-v2.html).

## Shutdown and Exit Codes

The parent forwards the first SIGINT or SIGTERM to the supervisor, which signals
the namespace's other processes. It allows two seconds for shutdown. A second
shutdown signal, or expiry of that deadline, immediately kills the entire cgroup.
When the main command exits normally, PID 1 exits too; Linux terminates any
remaining processes in that PID namespace. The parent waits for PID 1 and
removes the cgroup. Private mounts disappear with the namespace; the rootfs
directory and its contents are retained.

| Outcome | Runtime exit code |
| --- | --- |
| Command exits normally | Command's exit code |
| Command dies from a signal | `128 + signal number` |
| Runtime receives SIGINT | `130` |
| Runtime receives SIGTERM | `143` |
| Whole-group OOM SIGKILL | Typically `137` |
| Executable cannot be started | `127` |
| Argument, setup, or cleanup error | `1` |

Runtime-requested shutdown reports `130` or `143` even if the command handles
the signal and returns a different status. A cleanup error can override the
command's status with `1`.

SIGKILL cannot execute cleanup code. Parent-death signaling kills the supervisor
if the host runtime dies; a pidfd check covers death before that protection was
installed. Container processes stop, but an empty cgroup directory may remain.
Inspect the specific directory before manually removing it with `sudo rmdir`;
do not delete groups belonging to active runs.

## Tests

Build first, then use the same interface as the project grows:

```bash
ctest --test-dir build --output-on-failure
```

CTest prepares a separate `build/test-rootfs` fixture. It does not require or
modify your manually created `./rootfs`. The fixture copies the test workload
and its dependencies into its own filesystem.

Privileged tests use passwordless sudo when available. Without it, those tests
are marked skipped, not passed. To execute them with an interactive sudo prompt:

```bash
sudo ctest --test-dir build --output-on-failure
```

| Milestone | Coverage |
| --- | --- |
| 1: Process execution | Output, arguments, exit status, invalid executable, usage |
| 2: Namespaces | Hostname, PID 1 supervisor relationship, private proc, namespace identities, unchanged host state |
| 3: Rootfs | Container root and working directory, hidden host paths, proc, devices, invalid rootfs |
| 4: Control | Limit settings, actual CPU/memory/PID pressure, signals, escalation, parent death, orphan reaping, descendant and error cleanup |

The suite currently registers 31 checks, including fixture preparation. Select a
milestone with its label:

```bash
ctest --test-dir build -L milestone4 --output-on-failure
```

To build only the runtime without registering tests:

```bash
cmake -S . -B build -DBUILD_TESTING=OFF
cmake --build build
```

## Project Layout

```text
CMakeLists.txt                     Linux checks, build targets, CTest registration
src/main.cpp                       CLI, namespaces, rootfs mounts, startup coordination
src/control.h                      Resource and lifecycle interfaces
src/control.cpp                    Cgroup configuration, signals, supervision, cleanup
scripts/create_rootfs.sh            Minimal rootfs preparation from trusted host binaries
tests/prepare_rootfs.sh             CTest filesystem fixture
tests/test_process_execution.sh     Milestone 1 checks
tests/test_namespace_isolation.sh   Milestone 2 checks
tests/test_rootfs.sh                Milestone 3 checks
tests/test_control.sh               Milestone 4 checks
tests/workload.cpp                 Controlled resource and lifecycle test programs
```

Build directories, generated rootfs contents, editor metadata, and temporary
files are ignored by Git. The scripts and tests that recreate them are tracked.

## Troubleshooting

| Symptom | What to check |
| --- | --- |
| CMake rejects macOS or another platform | Configure inside Linux, such as the Lima VM; do not reuse a macOS build cache. |
| Namespace creation requires root | Run the runtime with `sudo` inside Linux. |
| `clone()` or mount setup fails with permission denied | The environment may restrict namespaces or mount capabilities, even for root. Use a Linux VM rather than a restricted nested container. |
| Cgroup creation or controller writes fail | Check `/sys/fs/cgroup/cgroup.controllers`, writable cgroup v2 access, and controller availability. |
| `cgroup.kill` is required | Use a kernel/environment exposing this interface; Linux 5.14+ is required by this runtime. |
| Rootfs helper refuses an existing path | Reuse that rootfs or choose a new destination. It never overwrites an existing rootfs. |
| `execve` reports no such file for an existing executable | Check the path inside the rootfs, its ELF loader, shared libraries, or script interpreter. |
| A command such as `sleep` is unavailable | The generated rootfs contains only the small tool set copied by the helper. Add the executable and its dependencies yourself. |
| Tests are skipped | Run CTest as root or provide passwordless sudo in the development VM. |

## Limitations and Next Steps

This runtime runs the workload as privileged root. It does not drop Linux
capabilities, create a user namespace, apply seccomp, or close every inherited
host file descriptor. [The Linux `chroot()` manual](https://man7.org/linux/man-pages/man2/chroot.2.html)
explains why changing the root directory alone is not a security boundary.
Do not run untrusted programs in this runtime.

Network and IPC namespaces are not created. The workload shares the host's
network and IPC namespaces; in Lima, that host is the Linux VM. There is no
resource limit for disk space or I/O, no read-only rootfs, no image management,
and no production-grade cgroup delegation. The minimal rootfs is not a complete
distribution: it has no package manager, DNS setup, or general application stack.

Future work includes `pivot_root()` with old-root detachment, capability and
descriptor restrictions, seccomp, network isolation, and more complete rootfs
and console handling. These are roadmap items, not current features.

## Contributing

Keep changes focused on a milestone, add behavior-level coverage to CTest, and
run the relevant privileged tests in Linux. Preserve the `run` interface and
document changes to its options, privilege requirements, or cleanup behavior.

## License

No project license file is currently included.

## References

- [Linux cgroup v2 documentation](https://docs.kernel.org/admin-guide/cgroup-v2.html)
- [PID namespaces](https://man7.org/linux/man-pages/man7/pid_namespaces.7.html)
- [`clone()`](https://man7.org/linux/man-pages/man2/clone.2.html)
- [`chroot()`](https://man7.org/linux/man-pages/man2/chroot.2.html)
- [`pivot_root()`](https://man7.org/linux/man-pages/man2/pivot_root.2.html)
- [`signalfd()`](https://man7.org/linux/man-pages/man2/signalfd.2.html)
- [Lima usage](https://lima-vm.io/docs/usage/)
