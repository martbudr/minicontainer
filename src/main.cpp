#include <filesystem>
#include <iostream>
#include <sched.h>
#include <unistd.h>

#include "cli.hpp"
#include "container.hpp"

namespace fs = std::filesystem;

int usage(const char* program_name) {
    std::cerr << "usage: " << program_name
              << " run [--hostname <name>] [--memory <bytes|K|M|G>]"
              << " [--cpus <count>] [--pids <count>] <rootfs> <command> [args...]\n";
    return 1;
}

int main(int argc, char** argv, char** envp) {
    try {
        Options options = parse_arguments(argc, argv);

        // convert rootfs from string, check if dir and check for /proc and /dev
        std::error_code rootfs_error;
        const auto rootfs = fs::canonical(options.rootfs, rootfs_error);
        if (rootfs_error || !fs::is_directory(rootfs) || rootfs == "/") {
            std::cerr << "rootfs must be an existing directory other than /. Incorrect directory: " << rootfs << "\n";
            return 1;
        }

        for (const char* directory : {"proc", "dev"}) {
            const auto path = rootfs / directory;
            const auto status = fs::symlink_status(path, rootfs_error);
            if (rootfs_error || !fs::is_directory(status)) {
                std::cerr << "rootfs must contain real /proc and /dev directories\n";
                return 1;
            }
        }

        // rootful; might change later
        if (geteuid() != 0) {
            std::cerr << "namespace creation requires root; run with sudo\n";
            return 1;
        }

        return run_container(options, envp);
    } catch (const std::exception& error) {
        std::cerr << "error: " << error.what() << '\n';
        usage(argv[0]);
        return 1;
    }
}
