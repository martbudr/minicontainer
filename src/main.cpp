#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include <cerrno>
#include <csignal>
#include <cstddef>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <sched.h>
#include <string>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <sys/wait.h>
#include <unistd.h>
#include <vector>

namespace {

struct ChildCommand {
    char** args;
    char** environment;
    const char* hostname;
    const char* rootfs;
};

void setup_error(const char* operation) {
    std::cerr << operation << " failed: " << std::strerror(errno) << "\n";
    _exit(1);
}

int execute_command(void* data) {
    const auto* command = static_cast<ChildCommand*>(data);
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
    execve(command->args[0], command->args, command->environment);
    std::cerr << "execve failed for " << command->args[0] << ": "
              << std::strerror(errno) << "\n";
    _exit(127);
}

int print_usage(const char* program_name) {
    std::cerr << "usage: " << program_name
              << " run [--hostname <name>] <rootfs> <command> [args...]\n";
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
    int command_index = 2;
    if (std::string(argv[command_index]) == "--hostname") {
        if (argc < 6) {
            return print_usage(argv[0]);
        }
        hostname = argv[3];
        command_index = 4;
    }
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

    constexpr std::size_t stack_size = 1024 * 1024;
    std::vector<std::max_align_t> child_stack(
        stack_size / sizeof(std::max_align_t));
    ChildCommand command{child_args.data(), envp, hostname.c_str(), rootfs.c_str()};
    const pid_t child_pid = clone(execute_command,
                                 child_stack.data() + child_stack.size(),
                                 CLONE_NEWPID | CLONE_NEWUTS | CLONE_NEWNS | SIGCHLD,
                                 &command);
    if (child_pid < 0) {
        std::cerr << "clone failed: " << std::strerror(errno) << "\n";
        return 1;
    }

    int status = 0;
    while (waitpid(child_pid, &status, 0) < 0) {
        if (errno != EINTR) {
            std::cerr << "waitpid failed: " << std::strerror(errno) << "\n";
            return 1;
        }
    }

    if (WIFEXITED(status)) {
        return WEXITSTATUS(status);
    }

    if (WIFSIGNALED(status)) {
        return 128 + WTERMSIG(status);
    }

    return 1;
}
