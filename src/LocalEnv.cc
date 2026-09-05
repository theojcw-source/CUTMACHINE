#include "LocalEnv.h"

#include <cstdlib>
#include <fstream>
#include <string>

namespace local_env {
namespace {

std::string Trim(const std::string& value) {
    const size_t begin = value.find_first_not_of(" \t\r\n");
    if (begin == std::string::npos) return std::string();
    const size_t end = value.find_last_not_of(" \t\r\n");
    return value.substr(begin, end - begin + 1);
}

void LoadDotEnvFile(const std::string& path) {
    std::ifstream input(path);
    if (!input) return;
    std::string line;
    while (std::getline(input, line)) {
        std::string trimmed = Trim(line);
        if (trimmed.empty() || trimmed[0] == '#') continue;
        if (trimmed.rfind("export ", 0) == 0) trimmed = Trim(trimmed.substr(7));
        const size_t equals = trimmed.find('=');
        if (equals == std::string::npos) continue;
        std::string key = Trim(trimmed.substr(0, equals));
        std::string value = Trim(trimmed.substr(equals + 1));
        if (key.empty()) continue;
        if (value.size() >= 2 && value.front() == value.back() &&
            (value.front() == '\'' || value.front() == '"')) {
            value = value.substr(1, value.size() - 2);
        }
        // overwrite=0: a variable already in the environment wins over the
        // file, so a one-off override in front of a command works.
        ::setenv(key.c_str(), value.c_str(), /*overwrite=*/0);
    }
}

}  // namespace

std::string LocalEnvFilePath() {
    if (const char* explicitPath = std::getenv("CUTMACHINE_ENV_FILE");
        explicitPath && *explicitPath)
        return explicitPath;
    const char* home = std::getenv("HOME");
    if (home && *home) return std::string(home) + "/.config/cutmachine/.env";
    return std::string();
}

void LoadLocalEnvFileIfPresent() {
    const std::string path = LocalEnvFilePath();
    if (!path.empty()) LoadDotEnvFile(path);
}

std::string Value(const char* name) {
    LoadLocalEnvFileIfPresent();
    const char* value = std::getenv(name);
    return value ? std::string(value) : std::string();
}

}  // namespace local_env
