#include "rtsps/storage_manager.h"

#include <sys/stat.h>

#include <filesystem>
#include <fstream>
#include <utility>

namespace rtsps {
namespace {

bool is_mount_point(const std::filesystem::path& path) {
    std::error_code ec;
    const std::filesystem::path canonical_path = std::filesystem::canonical(path, ec);
    if (ec) {
        return false;
    }
    const std::filesystem::path parent_path = canonical_path.parent_path();
    if (parent_path.empty() || parent_path == canonical_path) {
        return true;
    }

    struct stat path_stat{};
    struct stat parent_stat{};
    if (::stat(canonical_path.c_str(), &path_stat) != 0) {
        return false;
    }
    if (::stat(parent_path.c_str(), &parent_stat) != 0) {
        return false;
    }
    return path_stat.st_dev != parent_stat.st_dev || path_stat.st_ino == parent_stat.st_ino;
}

}  // namespace

std::string to_string(StorageState state) {
    switch (state) {
        case StorageState::Unconfigured:
            return "unconfigured";
        case StorageState::Missing:
            return "missing";
        case StorageState::Checking:
            return "checking";
        case StorageState::Ready:
            return "ready";
        case StorageState::ReadOnly:
            return "read_only";
        case StorageState::Full:
            return "full";
        case StorageState::Error:
            return "error";
    }
    return "unknown";
}

StorageManager::StorageManager(std::string storage_root, std::uintmax_t min_free_bytes, bool require_mount_point,
                               Logger& logger)
    : storage_root_(std::move(storage_root)),
      min_free_bytes_(min_free_bytes),
      require_mount_point_(require_mount_point),
      logger_(logger) {}

StorageStatus StorageManager::check() {
    StorageStatus status;
    if (storage_root_.empty()) {
        status.state = StorageState::Unconfigured;
        status.message = "storage root is empty";
        return status;
    }

    std::error_code ec;
    if (!std::filesystem::exists(storage_root_, ec)) {
        status.state = StorageState::Missing;
        status.message = "storage root does not exist: " + storage_root_;
        return status;
    }
    if (!std::filesystem::is_directory(storage_root_, ec)) {
        status.state = StorageState::Error;
        status.message = "storage root is not a directory: " + storage_root_;
        return status;
    }

    status.state = StorageState::Checking;
    status.is_mount_point = is_mount_point(storage_root_);
    if (!status.is_mount_point) {
        const std::string warning = "storage root is not a mount point; writes may go to the OS filesystem";
        if (require_mount_point_) {
            status.state = StorageState::Error;
            status.message = warning;
            return status;
        }
        logger_.warn("[storage] " + warning + ": " + storage_root_);
    }

    std::filesystem::create_directories(std::filesystem::path(storage_root_) / "recordings" / "ch0", ec);
    if (ec) {
        status.state = StorageState::ReadOnly;
        status.message = "failed to create recordings directory: " + ec.message();
        return status;
    }
    std::filesystem::create_directories(std::filesystem::path(storage_root_) / "index", ec);
    if (ec) {
        status.state = StorageState::ReadOnly;
        status.message = "failed to create index directory: " + ec.message();
        return status;
    }

    const auto probe_path = std::filesystem::path(storage_root_) / ".rpi-vms-write-test";
    {
        std::ofstream probe(probe_path);
        if (!probe) {
            status.state = StorageState::ReadOnly;
            status.message = "storage root is not writable";
            return status;
        }
        probe << "ok\n";
    }
    std::filesystem::remove(probe_path, ec);

    const auto space = std::filesystem::space(storage_root_, ec);
    if (ec) {
        status.state = StorageState::Error;
        status.message = "failed to query free space: " + ec.message();
        return status;
    }
    status.available_bytes = space.available;
    if (space.available < min_free_bytes_) {
        status.state = StorageState::Full;
        status.message = "storage free space is below threshold";
        return status;
    }

    status.state = StorageState::Ready;
    status.message = "storage ready";
    logger_.info("[storage] ready: " + storage_root_ +
                 (status.is_mount_point ? " mount_point=yes" : " mount_point=no"));
    return status;
}

const std::string& StorageManager::storage_root() const { return storage_root_; }

}  // namespace rtsps
