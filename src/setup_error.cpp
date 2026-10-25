#pragma once

#include <cerrno>
#include <cstring>
#include <iostream>
#include <unistd.h>

[[noreturn]] inline void setup_error(const char* operation) {
    std::cerr << operation << " failed: "
              << std::strerror(errno) << '\n';
    _exit(1);
}