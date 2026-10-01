#pragma once

#include <string>
#include <sys/types.h>

struct Limits {
    std::string memory;
    std::string cpu;
    std::string pids;
};

bool parse_limit(const std::string& option, const std::string& value, Limits& limits);

class Cgroup {
public:
    bool create(const Limits& limits);
    bool attach(pid_t pid);
    bool kill_all();
    bool cleanup();
    ~Cgroup();

private:
    std::string path_;
};

// SIGINT/SIGTERM are blocked and read through signalfd, including in PID 1.
int create_signal_fd();
int supervise_command(char** args, char** environment);
int wait_container(pid_t pid, int signal_fd, Cgroup& cgroup);
