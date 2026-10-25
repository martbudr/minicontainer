#include "rootfs.hpp"
#include "setup_error.cpp"

#include <cerrno>
#include <cstring>
#include <iostream>
#include <filesystem>
#include <stdexcept>
#include <unistd.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>

std::string validate_rootfs(const std::string& path) {
    namespace fs = std::filesystem;
    std::error_code error;

    const auto root = fs::canonical(path, error);

    if (error || !fs::is_directory(root) || root == "/")
        throw std::runtime_error("rootfs must be an existing directory other than /");

    for (const char* directory : {"proc", "dev"}) {
        const auto status = fs::symlink_status(root / directory, error);
        if (error || !fs::is_directory(status))
            throw std::runtime_error("rootfs must contain real /proc and /dev directories");
    }

    return root.string();
}

void setup_rootfs(const char* rootfs) {
    // Prevent the new /proc mount from propagating back to the host
    if (mount(nullptr, "/", nullptr, MS_REC | MS_PRIVATE, nullptr) < 0) { // propagation settings
        setup_error("making mounts private");
    }

    if (mount(rootfs, rootfs, nullptr, MS_BIND, nullptr) < 0) { // self-mount
        setup_error("bind mounting rootfs");
    }

    if (chroot(rootfs) < 0 || chdir("/") < 0) {
        setup_error("entering rootfs");
    }

    if (mount("proc", "/proc", "proc", MS_NOSUID | MS_NODEV | MS_NOEXEC, nullptr) < 0) {
        setup_error("mounting /proc");
    }
}

void setup_devices() {
    if (mount("tmpfs", "/dev", "tmpfs", MS_NOSUID | MS_NOEXEC, "mode=755,size=4m") < 0) {
        setup_error("mounting /dev");
    }

    // defining for the kernel
    struct Device { const char* name; unsigned int major; unsigned int minor; };
    const Device devices[] = {
        {"/dev/null", 1, 3},
        {"/dev/zero", 1, 5},
        {"/dev/random", 1, 8},
        {"/dev/urandom", 1, 9},
        {"/dev/tty", 5, 0}};

    for (const auto& device : devices) {
        if (mknod(device.name, S_IFCHR | 0666, makedev(device.major, device.minor)) < 0 ||
            chmod(device.name, 0666) < 0) {
            setup_error("creating device");
        }
    }

    // pseudo-terminals & I/O
    if (mkdir("/dev/pts", 0755) < 0 ||
        mount("devpts", "/dev/pts", "devpts", MS_NOSUID | MS_NOEXEC, // pseudo-terminals
              "newinstance,ptmxmode=0666,mode=0620") < 0 ||
        symlink("pts/ptmx", "/dev/ptmx") < 0 || // path for PTY allocation
        symlink("/proc/self/fd", "/dev/fd") < 0 || // paths to current process's fds
        symlink("/proc/self/fd/0", "/dev/stdin") < 0 ||
        symlink("/proc/self/fd/1", "/dev/stdout") < 0 ||
        symlink("/proc/self/fd/2", "/dev/stderr") < 0)
    {
        setup_error("setting up /dev links and terminals");
    }
}
