#include "cli.hpp"
#include "container.hpp"
#include "rootfs.hpp"

#include <iostream>
#include <unistd.h>

int main(int argc, char** argv, char** envp) {
    try {
        Options options = parse_arguments(argc, argv);

        options.rootfs = validate_rootfs(options.rootfs);

        // rootful; might change later
        if (geteuid() != 0) {
            std::cerr << "namespace creation requires root; run with sudo\n";
            return 1;
        }

        return run_container(options, envp);
    } catch (const std::exception& error) {
        std::cerr << "error: " << error.what() << '\n';
        print_usage(argv[0]);
        return 1;
    }
}
