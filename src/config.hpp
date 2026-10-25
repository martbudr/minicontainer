#pragma once

#include <string>
#include <vector>

struct Limits {
    std::string memory;
    std::string cpus;
    std::string pids;
};

struct Options {
    std::string hostname = "minicontainer";

    Limits limits;

    std::string rootfs;
    std::vector<std::string> command; // command followed by its arguments
};

