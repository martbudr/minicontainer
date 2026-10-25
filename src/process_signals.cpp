#include "process_signals.hpp"

#include <unistd.h>
#include <signal.h>
#include <sys/wait.h>
#include <sys/signalfd.h>
#include <initializer_list>

namespace {
    void signal_handler(int) {}
}

int exit_status(int status) {
    if (WIFEXITED(status)) return WEXITSTATUS(status);
    if (WIFSIGNALED(status)) return 128 + WTERMSIG(status);
    return 1;
}

int next_signal(int fd) {
    signalfd_siginfo info{};
    if (read(fd, &info, sizeof(info)) == sizeof(info)) {
        return static_cast<int>(info.ssi_signo);
    }
    return 0;
}

int create_signal_fd() {
    sigset_t mask;
    sigemptyset(&mask);

    for (int signal : {SIGINT, SIGTERM, SIGCHLD}) sigaddset(&mask, signal);

    struct sigaction action{};
    action.sa_handler = signal_handler;
    sigemptyset(&action.sa_mask);

    // PID 1 only accepts SIGINT/SIGTERM if it has a handler installed.
    // signals pending and consumed through signalfd, instead of being ignored automatically (as it's PID1)
    if (sigaction(SIGINT, &action, nullptr) < 0 ||
        sigaction(SIGTERM, &action, nullptr) < 0 ||
        sigaction(SIGCHLD, &action, nullptr) < 0 ||
        sigprocmask(SIG_BLOCK, &mask, nullptr) < 0) return -1;

    return signalfd(-1, &mask, SFD_CLOEXEC | SFD_NONBLOCK);
}
