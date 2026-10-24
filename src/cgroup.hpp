#pragma once

#include <string>

#include "cli.hpp"

class Cgroup {
public:
    bool create(const Limits& limits);
    bool attach(pid_t pid) const;
    bool kill_all() const;
    bool cleanup();
    ~Cgroup();

private:
    std::string path_;
};
