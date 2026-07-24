#pragma once

#include <sqlite3.h>

#include <cstdint>
#include <mutex>
#include <string>

#include "rtsps/logger.h"

namespace rtsps {

class RecordingIndex {
public:
    RecordingIndex(std::string db_path, Logger& logger, int busy_timeout_ms = 5000);
    ~RecordingIndex();

    RecordingIndex(const RecordingIndex&) = delete;
    RecordingIndex& operator=(const RecordingIndex&) = delete;

    void open();
    void initialize_schema();
    void upsert_channel_mapping(int channel_id, int camera_channel);
    void mark_segment_opened(int channel_id, const std::string& location, const std::string& container,
                             const std::string& codec);
    void mark_segment_closed(const std::string& location);
    int busy_timeout_ms() const;

private:
    void exec(const std::string& sql);
    void bind_text(sqlite3_stmt* stmt, int index, const std::string& value);
    std::string now_utc() const;
    std::int64_t file_size_or_zero(const std::string& path) const;

    std::string db_path_;
    Logger& logger_;
    int busy_timeout_ms_;
    sqlite3* db_ = nullptr;
    mutable std::mutex mutex_;
};

}  // namespace rtsps
