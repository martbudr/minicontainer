#include "container_init.hpp"
#include "process_signals.hpp"
#include "rootfs.hpp"
#include "setup_error.cpp"

#include <cerrno>
#include <cstring>
#include <iostream>
#include <signal.h>
#include <unistd.h>
#include <sys/poll.h>
#include <sys/wait.h>
#include <sys/prctl.h>


static int supervise_command(char** args, char** environment) {
    const int signal_fd = create_signal_fd();

    if (signal_fd < 0) return 1;

    const pid_t command = fork();

    if (command < 0) {
        std::cerr << "forking command failed: " << std::strerror(errno) << "\n";
        close(signal_fd);
        return 1;
    }
    if (command == 0) { // child
        close(signal_fd); // no need for supervisor's signalfd

        struct sigaction action{};
        action.sa_handler = SIG_DFL; // restoring default behavior of signals
        sigemptyset(&action.sa_mask);

        for (int signal : {SIGINT, SIGTERM, SIGCHLD, SIGPIPE}) sigaction(signal, &action, nullptr);

        sigset_t empty;
        sigemptyset(&empty); // signals not blocked on the child
        sigprocmask(SIG_SETMASK, &empty, nullptr);

        execve(args[0], args, environment); // executing the requested program

        std::cerr << "execve failed for " << args[0] << ": " << std::strerror(errno) << "\n";
        _exit(127);
    }

    for (;;) { // main (PID 1)
        int status;
        pid_t reaped;
        while ((reaped = waitpid(-1, &status, WNOHANG)) > 0) { // non-hanging
            if (reaped == command) { // PID 1 close
                close(signal_fd);
                // Exiting namespace PID 1 makes the kernel kill remaining descendants.
                return exit_status(status);
            }
        }
        if (reaped < 0 && errno != EINTR) return 1;
        pollfd event{signal_fd, POLLIN, 0}; // sleeping till signalfd has sth to read

        if (poll(&event, 1, -1) < 0 && errno != EINTR) return 1;

        for (int signal; (signal = next_signal(signal_fd)) != 0;) {
            if (signal == SIGINT || signal == SIGTERM) kill(-1, signal); // handling signals, sending to all PID >1 processes
        }
    }
}

int container_main(void* args) {
    const auto* command = static_cast<ChildCommand*>(args);

    close(command->gate_write);
    close(command->parent_signal_fd);

    // sigkill to child on parent death (parent is a supervisor)
    if (prctl(PR_SET_PDEATHSIG, SIGKILL) < 0) setup_error("setting parent death signal");

    // A pidfd catches parent death that happened before PR_SET_PDEATHSIG was set.
    pollfd parent{command->parent_pid_fd, POLLIN, 0};
    if (poll(&parent, 1, 0) != 0) _exit(1);
    close(command->parent_pid_fd);

    // waiting for ready from parent
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

    setup_rootfs(command->rootfs);
    setup_devices();

    _exit(supervise_command(command->args, command->environment));
}

