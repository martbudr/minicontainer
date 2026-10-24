#pragma once

#include "cgroup.hpp"
#include "cli.hpp"

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

int supervise_command(char** args, char** environment);
int container_main(void* args);
int run_container(const Options& options, char** envp);
int wait_container(pid_t pid, int signal_fd, Cgroup& cgroup);
