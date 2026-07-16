#pragma once

#include "rtsps/logger.h"

#include <cstdint>
#include <string>

namespace rtsps {

enum class StorageState {
    Unconfigured,
    Missing,
    Checking,
    Ready,
    ReadOnly,
    Full,
    Error,
};

std::string to_string(StorageState state);

struct StorageStatus {
    StorageState state = StorageState::Unconfigured;
    std::string message;
    std::uintmax_t available_bytes = 0;
    bool is_mount_point = false;
};

class StorageManager {
public:
    StorageManager(std::string storage_root, std::uintmax_t min_free_bytes, bool require_mount_point, Logger& logger);

    StorageStatus check();
    const std::string& storage_root() const;

private:
    std::string storage_root_;
    std::uintmax_t min_free_bytes_;
    bool require_mount_point_;
    Logger& logger_;
};

}  // namespace rtsps
