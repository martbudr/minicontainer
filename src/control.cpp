#include "control.h"

#include <cerrno>
#include <charconv>
#include <chrono>
#include <cmath>
#include <csignal>
#include <cstring>
#include <fcntl.h>
#include <fstream>
#include <iostream>
#include <iterator>
#include <limits>
#include <poll.h>
#include <sys/signalfd.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#include <utility>

namespace {

bool write_control(const std::string& path, const std::string& value) {
    const int fd = open(path.c_str(), O_WRONLY | O_CLOEXEC);
    if (fd < 0) {
        std::cerr << "cannot open " << path << ": " << std::strerror(errno) << "\n";
        return false;
    }
    ssize_t written;
    do {
        written = write(fd, value.data(), value.size());
    } while (written < 0 && errno == EINTR);
    const int error = errno;
    close(fd);
    if (written != static_cast<ssize_t>(value.size())) {
        std::cerr << "cannot write " << path << ": "
                  << (written < 0 ? std::strerror(error) : "short write") << "\n";
        return false;
    }
    return true;
}

bool positive_integer(const std::string& text, unsigned long long& value) {
    const auto result = std::from_chars(text.data(), text.data() + text.size(), value);
    return result.ec == std::errc{} && result.ptr == text.data() + text.size() && value > 0;
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

}  // namespace

bool parse_limit(const std::string& option, const std::string& value, Limits& limits) {
    unsigned long long number = 0;
    if (option == "--memory") {
        std::string digits = value;
        unsigned long long multiplier = 1;
        if (!digits.empty()) {
            switch (digits.back()) {
                case 'K': multiplier = 1024; break;
                case 'M': multiplier = 1024 * 1024; break;
                case 'G': multiplier = 1024ULL * 1024 * 1024; break;
                default: break;
            }
            if (multiplier != 1) digits.pop_back();
        }
        if (!positive_integer(digits, number) ||
            number > std::numeric_limits<unsigned long long>::max() / multiplier) return false;
        limits.memory = std::to_string(number * multiplier);
        return true;
    }
    if (option == "--pids") {
        if (!positive_integer(value, number) || number < 2 ||
            number > static_cast<unsigned long long>(std::numeric_limits<int>::max())) return false;
        limits.pids = std::to_string(number);
        return true;
    }
    if (option == "--cpus") {
        // Accept ordinary decimal CPU counts, excluding NaN, infinity and exponents.
        if (value.empty() || value.find_first_not_of("0123456789.") != std::string::npos) return false;
        try {
            std::size_t used = 0;
            const double cpus = std::stod(value, &used);
            if (used != value.size() || !std::isfinite(cpus) || cpus < 0.01 || cpus > 1024) return false;
            limits.cpu = std::to_string(static_cast<unsigned long long>(std::llround(cpus * 100000)))
                         + " 100000";
            return true;
        } catch (...) {
            return false;
        }
    }
    return false;
}

bool Cgroup::create(const Limits& limits) {
    const std::string root = "/sys/fs/cgroup";
    std::ifstream controllers(root + "/cgroup.controllers");
    if (!controllers) {
        std::cerr << "cgroups v2 must be mounted writable at /sys/fs/cgroup\n";
        return false;
    }
    std::string available((std::istreambuf_iterator<char>(controllers)), {});
    std::ifstream subtree(root + "/cgroup.subtree_control");
    std::string enabled((std::istreambuf_iterator<char>(subtree)), {});
    std::string enable;
    for (const auto& controller : {std::pair{"memory", !limits.memory.empty()},
                                   std::pair{"cpu", !limits.cpu.empty()},
                                   std::pair{"pids", !limits.pids.empty()}}) {
        if (!controller.second) continue;
        if (available.find(controller.first) == std::string::npos) {
            std::cerr << "cgroup controller unavailable: " << controller.first << "\n";
            return false;
        }
        if (enabled.find(controller.first) == std::string::npos) {
            enable += std::string("+") + controller.first + " ";
        }
    }
    if (!enable.empty() && !write_control(root + "/cgroup.subtree_control", enable)) return false;
    std::string pattern = root + "/minicontainer-" + std::to_string(getpid()) + "-XXXXXX";
    if (!mkdtemp(pattern.data())) {
        std::cerr << "creating cgroup failed: " << std::strerror(errno) << "\n";
        return false;
    }
    path_ = pattern;
    if (!limits.memory.empty() &&
        (!write_control(path_ + "/memory.max", limits.memory) ||
         !write_control(path_ + "/memory.swap.max", "0") ||
         !write_control(path_ + "/memory.oom.group", "1"))) return false;
    if (!limits.cpu.empty() && !write_control(path_ + "/cpu.max", limits.cpu)) return false;
    if (!limits.pids.empty() && !write_control(path_ + "/pids.max", limits.pids)) return false;
    // Require whole-cgroup killing so shutdown covers descendants in other process groups.
    if (access((path_ + "/cgroup.kill").c_str(), W_OK) < 0) {
        std::cerr << "cgroup.kill is required (Linux 5.14 or newer)\n";
        return false;
    }
    return true;
}

bool Cgroup::attach(pid_t pid) {
    return write_control(path_ + "/cgroup.procs", std::to_string(pid));
}

bool Cgroup::kill_all() {
    return path_.empty() || write_control(path_ + "/cgroup.kill", "1");
}

bool Cgroup::cleanup() {
    if (path_.empty()) return true;
    // Setup can fail before cgroup.kill is available; an empty group needs only rmdir.
    if (rmdir(path_.c_str()) == 0 || errno == ENOENT) {
        path_.clear();
        return true;
    }
    if (!kill_all()) return false;
    for (int attempt = 0; attempt < 100; ++attempt) {
        if (rmdir(path_.c_str()) == 0 || errno == ENOENT) {
            path_.clear();
            return true;
        }
        if (errno != EBUSY && errno != ENOTEMPTY) break;
        usleep(10000);
    }
    std::cerr << "removing cgroup " << path_ << " failed: " << std::strerror(errno) << "\n";
    return false;
}

Cgroup::~Cgroup() {
    cleanup();
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
