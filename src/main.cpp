#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "control.h"

#include <cerrno>
#include <csignal>
#include <cstddef>
#include <cstring>
#include <filesystem>
#include <fcntl.h>
#include <iostream>
#include <poll.h>
#include <sched.h>
#include <string>
#include <sys/mount.h>
#include <sys/prctl.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>
#include <vector>

namespace {

struct ChildCommand {
    char** args;
    char** environment;
    const char* hostname;
    const char* rootfs;
    int gate_read;
    int gate_write;
    int parent_signal_fd;
    int parent_pid_fd;
};

void setup_error(const char* operation) {
    std::cerr << operation << " failed: " << std::strerror(errno) << "\n";
    _exit(1);
}

int execute_command(void* data) {
    const auto* command = static_cast<ChildCommand*>(data);
    close(command->gate_write);
    close(command->parent_signal_fd);
    if (prctl(PR_SET_PDEATHSIG, SIGKILL) < 0) setup_error("setting parent death signal");
    // A pidfd catches parent death that happened before PR_SET_PDEATHSIG was set.
    pollfd parent{command->parent_pid_fd, POLLIN, 0};
    if (poll(&parent, 1, 0) != 0) _exit(1);
    close(command->parent_pid_fd);
    char ready;
    ssize_t received;
    do {
        received = read(command->gate_read, &ready, 1);
    } while (received < 0 && errno == EINTR);
    close(command->gate_read);
    if (received != 1 || ready != 'R') _exit(1);
    if (sethostname(command->hostname, std::strlen(command->hostname)) < 0) {
        setup_error("sethostname");
    }
    // Prevent the new /proc mount from propagating back to the host.
    if (mount(nullptr, "/", nullptr, MS_REC | MS_PRIVATE, nullptr) < 0) {
        setup_error("making mounts private");
    }
    if (mount(command->rootfs, command->rootfs, nullptr, MS_BIND, nullptr) < 0) {
        setup_error("bind mounting rootfs");
    }
    if (chdir(command->rootfs) < 0 || chroot(".") < 0 || chdir("/") < 0) {
        setup_error("entering rootfs");
    }
    if (mount("proc", "/proc", "proc", MS_NOSUID | MS_NODEV | MS_NOEXEC, nullptr) < 0) {
        setup_error("mounting /proc");
    }
    if (mount("tmpfs", "/dev", "tmpfs", MS_NOSUID | MS_NOEXEC, "mode=755,size=4m") < 0) {
        setup_error("mounting /dev");
    }
    struct Device { const char* name; unsigned int major; unsigned int minor; };
    const Device devices[] = {{"/dev/null", 1, 3}, {"/dev/zero", 1, 5},
                              {"/dev/random", 1, 8}, {"/dev/urandom", 1, 9},
                              {"/dev/tty", 5, 0}};
    for (const auto& device : devices) {
        if (mknod(device.name, S_IFCHR | 0666, makedev(device.major, device.minor)) < 0 ||
            chmod(device.name, 0666) < 0) {
            setup_error("creating device");
        }
    }
    if (mkdir("/dev/pts", 0755) < 0 ||
        mount("devpts", "/dev/pts", "devpts", MS_NOSUID | MS_NOEXEC,
              "newinstance,ptmxmode=0666,mode=0620") < 0 ||
        symlink("pts/ptmx", "/dev/ptmx") < 0 ||
        symlink("/proc/self/fd", "/dev/fd") < 0 ||
        symlink("/proc/self/fd/0", "/dev/stdin") < 0 ||
        symlink("/proc/self/fd/1", "/dev/stdout") < 0 ||
        symlink("/proc/self/fd/2", "/dev/stderr") < 0) {
        setup_error("setting up /dev links and terminals");
    }
    _exit(supervise_command(command->args, command->environment));
}

int print_usage(const char* program_name) {
    std::cerr << "usage: " << program_name
              << " run [--hostname <name>] [--memory <bytes|K|M|G>]"
              << " [--cpus <count>] [--pids <count>] <rootfs> <command> [args...]\n";
    return 1;
}

}  // namespace

int main(int argc, char* argv[], char* envp[]) {
    if (argc < 4) {
        return print_usage(argv[0]);
    }

    const std::string action = argv[1];
    if (action != "run") {
        std::cerr << "unknown command: " << action << "\n";
        return print_usage(argv[0]);
    }

    std::string hostname = "minicontainer";
    Limits limits;
    int command_index = 2;
    while (command_index < argc && std::string(argv[command_index]).starts_with("--")) {
        const std::string option = argv[command_index++];
        if (option == "--") break;
        if (command_index >= argc) return print_usage(argv[0]);
        const std::string value = argv[command_index++];
        if (option == "--hostname") {
            hostname = value;
        } else if (!parse_limit(option, value, limits)) {
            std::cerr << "invalid option or value: " << option << " " << value
                      << "\nUse positive memory bytes (or K/M/G), CPU counts 0.01-1024,"
                      << " and a PID limit of at least 2.\n";
            return 1;
        }
    }
    if (argc - command_index < 2) return print_usage(argv[0]);
    if (hostname.empty() || hostname.size() > 64) {
        std::cerr << "hostname must contain between 1 and 64 bytes\n";
        return 1;
    }
    std::error_code rootfs_error;
    const auto rootfs = std::filesystem::canonical(argv[command_index], rootfs_error);
    if (rootfs_error || !std::filesystem::is_directory(rootfs, rootfs_error) ||
        rootfs_error || rootfs == "/") {
        std::cerr << "rootfs must be an existing directory other than /\n";
        return 1;
    }
    for (const char* directory : {"proc", "dev"}) {
        const auto path = rootfs / directory;
        const auto status = std::filesystem::symlink_status(path, rootfs_error);
        if (rootfs_error || !std::filesystem::is_directory(status)) {
            std::cerr << "rootfs must contain real /proc and /dev directories\n";
            return 1;
        }
    }
    ++command_index;
    if (geteuid() != 0) {
        std::cerr << "namespace creation requires root; run with sudo\n";
        return 1;
    }

    std::vector<char*> child_args;
    for (int i = command_index; i < argc; ++i) {
        child_args.push_back(argv[i]);
    }
    child_args.push_back(nullptr);

    struct sigaction ignore_pipe{};
    ignore_pipe.sa_handler = SIG_IGN;
    sigemptyset(&ignore_pipe.sa_mask);
    sigaction(SIGPIPE, &ignore_pipe, nullptr);
    const int signal_fd = create_signal_fd();
    if (signal_fd < 0) {
        std::cerr << "signal setup failed: " << std::strerror(errno) << "\n";
        return 1;
    }
    Cgroup cgroup;
    if (!cgroup.create(limits)) {
        close(signal_fd);
        return 1;
    }
    int gate[2];
    if (pipe2(gate, O_CLOEXEC) < 0) { // synchro pipe between parent & child; parent writes that the child can start
        std::cerr << "startup pipe failed: " << std::strerror(errno) << "\n";
        close(signal_fd);
        return 1;
    }
    const int parent_pid_fd = static_cast<int>(syscall(SYS_pidfd_open, getpid(), 0));
    if (parent_pid_fd < 0) {
        std::cerr << "opening parent pidfd failed: " << std::strerror(errno) << "\n";
        close(gate[0]);
        close(gate[1]);
        close(signal_fd);
        return 1;
    }
    constexpr std::size_t stack_size = 1024 * 1024;
    std::vector<std::max_align_t> child_stack(
        stack_size / sizeof(std::max_align_t));
    ChildCommand command{child_args.data(), envp, hostname.c_str(), rootfs.c_str(),
                         gate[0], gate[1], signal_fd, parent_pid_fd};
    const pid_t child_pid = clone(execute_command,
                                 child_stack.data() + child_stack.size(),
                                 CLONE_NEWPID | CLONE_NEWUTS | CLONE_NEWNS | SIGCHLD,
                                 &command);
    close(parent_pid_fd);
    if (child_pid < 0) {
        std::cerr << "clone failed: " << std::strerror(errno) << "\n";
        close(gate[0]);
        close(gate[1]);
        close(signal_fd);
        return 1;
    }
    close(gate[0]);
    const bool attached = cgroup.attach(child_pid);
    const bool released = attached && write(gate[1], "R", 1) == 1;
    close(gate[1]);
    if (!released) {
        kill(child_pid, SIGKILL);
        while (waitpid(child_pid, nullptr, 0) < 0 && errno == EINTR) {}
        close(signal_fd);
        return 1;
    }
    const int result = wait_container(child_pid, signal_fd, cgroup);
    close(signal_fd);
    return cgroup.cleanup() ? result : 1;
}
