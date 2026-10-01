# minicontainer

A Linux learning runtime with PID, UTS and mount namespaces, a chroot rootfs,
and cgroup v2 resource limits. On macOS, enter the configured Linux VM first:

```bash
limactl shell minicontainer
cd /Users/mbudr/Documents/minicontainer
cmake -S . -B build-linux
cmake --build build-linux
```

Create the minimal rootfs once, if it does not already exist:

```bash
bash scripts/create_rootfs.sh ./rootfs
```

Run a command using paths inside the rootfs:

```bash
sudo ./build-linux/minicontainer run \
    --hostname box --memory 256M --cpus 0.5 --pids 64 \
    ./rootfs /bin/bash
```

Options precede the rootfs. Memory accepts positive bytes or binary `K`, `M`,
and `G` suffixes; `256M` is 268435456 bytes. A memory limit also disables swap
for that cgroup and enables whole-group OOM killing. CPU counts from `0.01` to
`1024` set a bandwidth quota with a 100 ms period: `0.5` permits 50 ms of total
CPU time per period. PID limits count processes and threads, including the PID 1
supervisor and the initial command, and must be at least two. Unspecified limits
use the kernel defaults and still inherit any ancestor limits.

The runtime requires root, a writable cgroup v2 hierarchy at `/sys/fs/cgroup`,
and Linux 5.14 or newer for `cgroup.kill`. It enables requested controllers in
the hierarchy root when necessary, then creates a unique `minicontainer-*`
cgroup. The namespace child waits until its cgroup membership is established,
so setup and command execution are subject to the limits from the start.

PID 1 supervises the command (normally PID 2), forwards SIGINT and SIGTERM,
and reaps orphaned children. A signal sent to the runtime gives the container
two seconds to stop, then kills the entire cgroup. SIGINT/SIGTERM shutdown returns
130/143; otherwise the command's exit status is preserved, including `128 + signal`
for signal deaths. Exiting the command terminates remaining namespace processes.
The parent reaps PID 1 and deletes the cgroup; private mounts disappear when the
namespace exits. Normal exits, setup failures and catchable shutdown signals all
run cleanup.

SIGKILL cannot run cleanup code: parent-death signaling stops the container, but
an empty cgroup directory may remain and need removal. This privileged chroot
runtime is for trusted learning workloads, not a security sandbox; pivot_root,
capability restrictions and seccomp are not implemented yet.

Run all milestone tests with the same CTest interface:

```bash
ctest --test-dir build-linux --output-on-failure
```

Tests use passwordless sudo when not already root; otherwise privileged tests
report a skip. Use `sudo ctest --test-dir build-linux --output-on-failure` when
passwordless sudo is unavailable. Milestone 4 includes actual CPU, memory and
PID pressure, signal forwarding, forced shutdown, orphan reaping, descendant
cleanup, invalid arguments and failed startup.

Reference: [kernel cgroup v2 documentation](https://docs.kernel.org/admin-guide/cgroup-v2.html).
