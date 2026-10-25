#pragma once

#include <string>

std::string validate_rootfs(const std::string& path);
void setup_rootfs(const char* rootfs);
void setup_devices();