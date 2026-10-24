#include "cli.hpp"

// Parse digits only, detecting overflow.
std::uint64_t parse_integer(const std::string& text) {
    if (text.empty())
        throw std::runtime_error("expected a nonempty integer");

    std::uint64_t result = 0;

    for (char c : text) {
        if (c < '0' || c > '9')
            throw std::runtime_error("invalid integer: " + text);

        unsigned digit = c - '0';

        if (result > (std::numeric_limits<std::uint64_t>::max() - digit) / 10)
            throw std::runtime_error("integer too large: " + text);

        result = result * 10 + digit;
    }

    return result;
}

std::uint64_t parse_memory(std::string text) {
    if (text.empty())
        throw std::runtime_error("--memory requires a value");

    std::uint64_t multiplier = 1;

    switch (text.back()) {
        case 'K': multiplier = 1024ULL; break;
        case 'M': multiplier = 1024ULL * 1024; break;
        case 'G': multiplier = 1024ULL * 1024 * 1024; break;
        default: throw std::runtime_error("--memory must be not more than 1GB");
    }

    if (multiplier != 1)
        text.pop_back();

    auto value = parse_integer(text);

    if (value == 0 ||
        value > std::numeric_limits<std::uint64_t>::max() / multiplier)
        throw std::runtime_error("--memory must be positive and fit in uint64_t");

    return value * multiplier;
}

double parse_cpus(const std::string& text) {
    bool digit_seen = false;
    bool dot_seen = false;

    for (char c : text) {
        if (c >= '0' && c <= '9') {
            digit_seen = true;
        } else if (c == '.' && !dot_seen) {
            dot_seen = true;
        } else {
            throw std::runtime_error("invalid --cpus value: " + text);
        }
    }

    if (!digit_seen)
        throw std::runtime_error("--cpus requires a decimal number");

    double value = std::stod(text);

    if (!std::isfinite(value) || value < 0.01 || value > 1024)
        throw std::runtime_error("--cpus must be between 0.01 and 1024");

    return value;
}

Options parse_arguments(int argc, char* argv[]) {
    if (argc < 2 || std::string(argv[1]) != "run")
        throw std::runtime_error("expected the 'run' subcommand");

    Options options;
    int i = 2;

    while (i < argc) {
        std::string option = argv[i];

        if (option == "--") {
            ++i;
            break;
        }

        if (option.empty() || option[0] != '-')
            break;

        if (option != "--hostname" && option != "--memory" &&
            option != "--cpus" && option != "--pids")
            throw std::runtime_error("unknown option: " + option);

        if (++i >= argc)
            throw std::runtime_error("missing value for " + option);

        std::string value = argv[i++];

        if (option == "--hostname") {
            if (value.empty() || value.size() > 64)
                throw std::runtime_error("--hostname must contain 1–64 bytes");

            options.hostname = value;
        } else if (option == "--memory") {
            options.limits.memory = parse_memory(value);
        } else if (option == "--cpus") {
            options.limits.cpus = parse_cpus(value);
        } else {
            auto count = parse_integer(value);

            if (count < 2 || count > 2147483647ULL)
                throw std::runtime_error("--pids must be between 2 and 2147483647");

            options.limits.pids = static_cast<int>(count);
        }
    }

    if (argc - i < 2)
        throw std::runtime_error("expected <rootfs> and <command>");

    options.rootfs = argv[i++];

    while (i < argc)
        options.command.emplace_back(argv[i++]);

    return options;
}