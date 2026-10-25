#pragma once

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

int container_main(void* args);
