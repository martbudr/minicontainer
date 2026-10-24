#include <iostream>
#include <unistd.h>
#include <signal.h>
#include <sys/fcntl.h>
#include <sys/poll.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <sys/prctl.h>
#include <cstring>
#include <filesystem>
#include <sys/signalfd.h>
#include <sys/syscall.h>
#include <sys/sysmacros.h>
#include <vector>

#include "container.hpp"
#include "cgroup.hpp"

namespace fs = std::filesystem;

namespace {
    void setup_error(const char* operation) {
        std::cerr << operation << " failed: " << std::strerror(errno) << "\n";
        _exit(1);
    }

    int exit_status(int status) {
        if (WIFEXITED(status)) return WEXITSTATUS(status);
        if (WIFSIGNALED(status)) return 128 + WTERMSIG(status);
        return 1;
    }

    void signal_handler(int) {}

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
}

int supervise_command(char** args, char** environment) {
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

    // defining for the kernel
    struct Device { const char* name; unsigned int major; unsigned int minor; };
    const Device devices[] = {
        {"/dev/null", 1, 3},
        {"/dev/zero", 1, 5},
        {"/dev/random", 1, 8},
        {"/dev/urandom", 1, 9},
        {"/dev/tty", 5, 0}};

    for (const auto& device : devices) {
        if (mknod(device.name, S_IFCHR | 0666, makedev(device.major, device.minor)) < 0 ||
            chmod(device.name, 0666) < 0) {
            setup_error("creating device");
        }
    }

    // terminals & I/O
    if (mkdir("/dev/pts", 0755) < 0 ||
        mount("devpts", "/dev/pts", "devpts", MS_NOSUID | MS_NOEXEC,
              "newinstance,ptmxmode=0666,mode=0620") < 0 ||
        symlink("pts/ptmx", "/dev/ptmx") < 0 ||
        symlink("/proc/self/fd", "/dev/fd") < 0 ||
        symlink("/proc/self/fd/0", "/dev/stdin") < 0 ||
        symlink("/proc/self/fd/1", "/dev/stdout") < 0 ||
        symlink("/proc/self/fd/2", "/dev/stderr") < 0)
    {
        setup_error("setting up /dev links and terminals");
    }

    _exit(supervise_command(command->args, command->environment));
}

int run_container(const Options& options, char** envp) {
    // 1. Prepare synchronization between parent and child.
    // 2. Create child with the requested Linux namespaces.

    // Child:
    //   Wait until parent has applied resource limits.
    //   Set hostname.
    //   Prepare mounts and switch to rootfs.
    //   Mount /proc inside the new root.
    //   Prepare signal handling for the command.
    //   Execute the command.

    // Parent:
    //   Create/configure the cgroup and put child into it.
    //   Allow child to continue.
    //   Supervise child and forward termination signals.
    //   Collect its exit status.
    //   Clean up cgroup and other resources.
    //   Return the command's exit code.


    // prepare the pipe
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