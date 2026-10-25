#include "container.hpp"
#include "container_init.hpp"
#include "cgroup.hpp"
#include "process_signals.hpp"

#include <cerrno>
#include <cstring>
#include <iostream>
#include <signal.h>
#include <unistd.h>
#include <sys/poll.h>
#include <sys/wait.h>
#include <chrono>
#include <cstddef>
#include <vector>
#include <sched.h>
#include <fcntl.h>
#include <sys/syscall.h>

namespace {
int wait_container(pid_t pid, int signal_fd, Cgroup& cgroup) {
    using Clock = std::chrono::steady_clock;
    bool stopping = false;
    int shutdown_signal = 0;

    auto deadline = Clock::time_point::max();

    for (;;) {
        int status;
        const pid_t result = waitpid(pid, &status, WNOHANG);

        if (result == pid) return shutdown_signal ? 128 + shutdown_signal : exit_status(status);

        if (result < 0 && errno != EINTR) {
            std::cerr << "waitpid failed: " << std::strerror(errno) << "\n";
            cgroup.kill_all();
            return 1;
        }

        if (stopping && Clock::now() >= deadline) { // time between SIGINT/SIGTERM & kill
            cgroup.kill_all();
            kill(pid, SIGKILL);
            deadline = Clock::time_point::max();
        }

        pollfd event{signal_fd, POLLIN, 0};
        if (poll(&event, 1, 50) < 0 && errno != EINTR) {
            cgroup.kill_all();
            continue;
        }

        for (int signal; (signal = next_signal(signal_fd)) != 0;) {
            if (signal != SIGINT && signal != SIGTERM) continue;

            if (stopping) { // kill automatically if SIGINT/SIGTERM for the second time
                cgroup.kill_all();
                kill(pid, SIGKILL);
            } else {
                stopping = true;
                shutdown_signal = signal;
                deadline = Clock::now() + std::chrono::seconds(2);
                kill(pid, signal);
            }
        }
    }
}
}

int run_container(const Options& options, char** envp) {
    // Prepare host signal handling.
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
    if (!cgroup.create(options.limits)) {
        close(signal_fd);
        return 1;
    }

    // pipe
    int gate[2];
    if (pipe2(gate, O_CLOEXEC) < 0) { // synchro pipe between parent & child; parent writes that the child can start
        std::cerr << "startup pipe failed: " << std::strerror(errno) << "\n";
        close(signal_fd);
        return 1;
    }

    // opening parent pid
    const int parent_pid_fd = static_cast<int>(syscall(SYS_pidfd_open, getpid(), 0));
    if (parent_pid_fd < 0) {
        std::cerr << "opening parent pidfd failed: " << std::strerror(errno) << "\n";
        close(gate[0]);
        close(gate[1]);
        close(signal_fd);
        return 1;
    }

    // running the child process (aka container)
    constexpr std::size_t stack_size = 1024 * 1024;
    std::vector<std::max_align_t> child_stack(
        stack_size / sizeof(std::max_align_t));

    // Kopia, ponieważ options jest const, a potrzebujemy char*.
    std::vector<std::string> command_strings = options.command;
    std::vector<char*> command_args;
    command_args.reserve(command_strings.size() + 1);
    for (auto& arg : command_strings)
        command_args.push_back(arg.data());
    command_args.push_back(nullptr);

    ChildCommand command{command_args.data(), envp, options.hostname.c_str(), options.rootfs.c_str(),
                         gate[0], gate[1], signal_fd, parent_pid_fd};

    const pid_t child_pid = clone(
        container_main,
        child_stack.data() + child_stack.size(),
        CLONE_NEWPID | CLONE_NEWUTS | CLONE_NEWNS | SIGCHLD,
        &command);

    // closing the unnecessary
    close(parent_pid_fd);
    if (child_pid < 0) {
        std::cerr << "clone failed: " << std::strerror(errno) << "\n";
        close(gate[0]);
        close(gate[1]);
        close(signal_fd);
        return 1;
    }

    close(gate[0]);
    // attaching to a group
    const bool attached = cgroup.attach(child_pid);
    const bool released = attached && write(gate[1], "R", 1) == 1;
    close(gate[1]);

    // wrong cgroup attachment
    if (!released) {
        kill(child_pid, SIGKILL);
        while (waitpid(child_pid, nullptr, 0) < 0 && errno == EINTR) {}
        close(signal_fd);
        return 1;
    }

    // running the child
    const int result = wait_container(child_pid, signal_fd, cgroup);

    close(signal_fd);

    return cgroup.cleanup() ? result : 1;
}
