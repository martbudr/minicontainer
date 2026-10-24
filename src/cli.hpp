#pragma once
#include <string>
#include <vector>
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <string>
#include <cmath>

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

// Parse digits only, detecting overflow.
std::uint64_t parse_integer(const std::string& text);

std::uint64_t parse_memory(std::string text);

double parse_cpus(const std::string& text);

Options parse_arguments(int argc, char* argv[]);