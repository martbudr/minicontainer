#include "cgroup.hpp"

#include <iostream>
#include <sys/fcntl.h>
#include <unistd.h>
#include <fstream>
#include <cstring>

namespace {
    bool write_control(const std::string& path, const std::string& value);
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
                                   std::pair{"cpu", !limits.cpus.empty()},
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

    if (!limits.cpus.empty() && !write_control(path_ + "/cpu.max", limits.cpus)) return false;
    if (!limits.pids.empty() && !write_control(path_ + "/pids.max", limits.pids)) return false;

    // Require whole-cgroup killing so shutdown covers descendants in other process groups.
    if (access((path_ + "/cgroup.kill").c_str(), W_OK) < 0) {
        std::cerr << "cgroup.kill is required (Linux 5.14 or newer)\n";
        return false;
    }

    return true;
}

bool Cgroup::attach(pid_t pid) const {
    return write_control(path_ + "/cgroup.procs", std::to_string(pid));
}

bool Cgroup::kill_all() const {
    return path_.empty() || write_control(path_ + "/cgroup.kill", "1");
}

bool Cgroup::cleanup() {
    if (path_.empty()) return true;

    // Setup can fail before cgroup.kill is available; an empty group needs only rmdir
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
}