#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include <cerrno>
#include <csignal>
#include <cstddef>
#include <cstring>
#include <iostream>
#include <sched.h>
#include <string>
#include <sys/mount.h>
#include <sys/wait.h>
#include <unistd.h>
#include <vector>

namespace {

struct ChildCommand {
    char** args;
    char** environment;
    const char* hostname;
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
    if (mount("proc", "/proc", "proc", MS_NOSUID | MS_NODEV | MS_NOEXEC, nullptr) < 0) {
        setup_error("mounting /proc");
    }
    execve(command->args[0], command->args, command->environment);
    std::cerr << "execve failed for " << command->args[0] << ": "
              << std::strerror(errno) << "\n";
    _exit(127);
}

int print_usage(const char* program_name) {
    std::cerr << "usage: " << program_name
              << " run [--hostname <name>] <command> [args...]\n";
    return 1;
}

}  // namespace

int main(int argc, char* argv[], char* envp[]) {
    if (argc < 3) {
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
        if (argc < 5) {
            return print_usage(argv[0]);
        }
        hostname = argv[3];
        command_index = 4;
    }
    if (hostname.empty() || hostname.size() > 64) {
        std::cerr << "hostname must contain between 1 and 64 bytes\n";
        return 1;
    }
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
    ChildCommand command{child_args.data(), envp, hostname.c_str()};
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
