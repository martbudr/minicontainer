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
#include <sys/wait.h>
#include <unistd.h>
#include <vector>

namespace {

struct ChildCommand {
    char** args;
    char** environment;
};

int execute_command(void* data) {
    const auto* command = static_cast<ChildCommand*>(data);
    execve(command->args[0], command->args, command->environment);
    std::cerr << "execve failed for " << command->args[0] << ": "
              << std::strerror(errno) << "\n";
    _exit(127);
}

int print_usage(const char* program_name) {
    std::cerr << "usage: " << program_name << " run <command> [args...]\n";
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

    std::vector<char*> child_args;
    for (int i = 2; i < argc; ++i) {
        child_args.push_back(argv[i]);
    }
    child_args.push_back(nullptr);

    constexpr std::size_t stack_size = 1024 * 1024;
    std::vector<std::max_align_t> child_stack(
        stack_size / sizeof(std::max_align_t));
    ChildCommand command{child_args.data(), envp};
    const pid_t child_pid = clone(execute_command,
                                 child_stack.data() + child_stack.size(),
                                 SIGCHLD, &command);
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
