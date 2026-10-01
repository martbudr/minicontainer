#include <cerrno>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <sys/wait.h>
#include <unistd.h>

int main(int argc, char** argv) {
    if (argc != 2) return 1;
    const char* mode = argv[1];
    if (std::strcmp(mode, "cpu") == 0) {
        std::puts("ready");
        std::fflush(stdout);
        volatile unsigned long counter = 0;
        for (;;) counter = counter + 1;
    }
    if (std::strcmp(mode, "memory") == 0) {
        std::puts("ready");
        std::fflush(stdout);
        usleep(500000);
        for (;;) {
            auto* block = static_cast<volatile char*>(std::malloc(1024 * 1024));
            if (!block) return 2;
            for (int offset = 0; offset < 1024 * 1024; offset += 4096) block[offset] = 1;
        }
    }
    if (std::strcmp(mode, "pids") == 0) {
        pid_t children[32];
        int count = 0;
        while (count < 32) {
            const pid_t child = fork();
            if (child < 0) break;
            if (child == 0) for (;;) pause();
            children[count++] = child;
        }
        const bool limited = count < 32 && errno == EAGAIN;
        for (int index = 0; index < count; ++index) kill(children[index], SIGKILL);
        while (waitpid(-1, nullptr, 0) > 0) {}
        std::printf("children=%d\n", count);
        return limited && count == 6 ? 0 : 1;
    }
    if (std::strcmp(mode, "zombies") == 0) {
        for (int index = 0; index < 20; ++index) {
            const pid_t child = fork();
            if (child < 0) return 1;
            if (child == 0) {
                const pid_t orphan = fork();
                if (orphan < 0) _exit(1);
                if (orphan == 0) {
                    usleep(50000);
                    _exit(0);
                }
                _exit(0);
            }
            int status;
            if (waitpid(child, &status, 0) != child || status != 0) return 1;
        }
        usleep(300000);
        DIR* directory = opendir("/proc");
        if (!directory) return 1;
        int processes = 0;
        bool zombie = false;
        while (const auto* entry = readdir(directory)) {
            if (entry->d_name[0] < '0' || entry->d_name[0] > '9') continue;
            char path[512];
            std::snprintf(path, sizeof(path), "/proc/%s/stat", entry->d_name);
            FILE* file = std::fopen(path, "r");
            if (!file) continue;
            char line[4096];
            if (std::fgets(line, sizeof(line), file)) {
                const char* end = std::strrchr(line, ')');
                if (end && end[1] == ' ') {
                    ++processes;
                    zombie = zombie || end[2] == 'Z';
                }
            }
            std::fclose(file);
        }
        closedir(directory);
        return !zombie && processes == 2 ? 0 : 1;
    }
    if (std::strcmp(mode, "signal") == 0 || std::strcmp(mode, "ignore") == 0 ||
        std::strcmp(mode, "descendant") == 0) {
        if (std::strcmp(mode, "signal") != 0) {
            signal(SIGINT, SIG_IGN);
            signal(SIGTERM, SIG_IGN);
        }
        const pid_t child = fork();
        if (child < 0) return 1;
        if (child == 0) {
            setsid();
            for (;;) pause();
        }
        std::puts("ready");
        std::fflush(stdout);
        if (std::strcmp(mode, "descendant") == 0) return 0;
        for (;;) pause();
    }
    if (std::strcmp(mode, "self_signal") == 0) {
        raise(SIGTERM);
        return 1;
    }
    return 1;
}
