#include "rtsps/recording_index.h"

#include <chrono>
#include <ctime>
#include <filesystem>
#include <iomanip>
#include <sstream>
#include <stdexcept>
#include <utility>

namespace rtsps {
namespace {

void check_sqlite(int rc, sqlite3* db, const std::string& action) {
    if (rc != SQLITE_OK && rc != SQLITE_DONE && rc != SQLITE_ROW) {
        throw std::runtime_error(action + ": " + sqlite3_errmsg(db));
    }
}

}  // namespace

RecordingIndex::RecordingIndex(std::string db_path, Logger& logger, int busy_timeout_ms)
    : db_path_(std::move(db_path)), logger_(logger), busy_timeout_ms_(busy_timeout_ms) {
    if (busy_timeout_ms_ < 0) {
        throw std::runtime_error("SQLite busy timeout must not be negative");
    }
}

RecordingIndex::~RecordingIndex() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (db_) {
        sqlite3_close(db_);
    }
}

void RecordingIndex::open() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (db_) {
        return;
    }
    const int rc = sqlite3_open(db_path_.c_str(), &db_);
    if (rc != SQLITE_OK) {
        std::string error = db_ ? sqlite3_errmsg(db_) : "unknown sqlite error";
        if (db_) {
            sqlite3_close(db_);
            db_ = nullptr;
        }
        throw std::runtime_error("sqlite open failed: " + error);
    }
    check_sqlite(sqlite3_busy_timeout(db_, busy_timeout_ms_), db_, "configure busy timeout");
    exec("PRAGMA foreign_keys = ON;");
    exec("PRAGMA journal_mode = WAL;");
    exec("PRAGMA synchronous = NORMAL;");
    logger_.info("[db] opened media index: " + db_path_ +
                 " writer_policy=process_mutex busy_timeout_ms=" + std::to_string(busy_timeout_ms_));
}

void RecordingIndex::initialize_schema() {
    std::lock_guard<std::mutex> lock(mutex_);
    exec(
        "CREATE TABLE IF NOT EXISTS schema_migrations ("
        "version INTEGER PRIMARY KEY,"
        "applied_at_utc TEXT NOT NULL,"
        "description TEXT NOT NULL"
        ");"
        "CREATE TABLE IF NOT EXISTS recording_segments ("
        "id INTEGER PRIMARY KEY AUTOINCREMENT,"
        "channel_id INTEGER NOT NULL,"
        "file_path TEXT NOT NULL UNIQUE,"
        "container TEXT NOT NULL,"
        "codec TEXT NOT NULL,"
        "start_wall_time_utc TEXT NOT NULL,"
        "end_wall_time_utc TEXT,"
        "start_pts_ns INTEGER,"
        "end_pts_ns INTEGER,"
        "duration_ns INTEGER,"
        "size_bytes INTEGER,"
        "complete INTEGER NOT NULL DEFAULT 0,"
        "created_at_utc TEXT NOT NULL"
        ");"
        "CREATE INDEX IF NOT EXISTS idx_recording_segments_channel_time "
        "ON recording_segments(channel_id, start_wall_time_utc, end_wall_time_utc);"
        "CREATE TABLE IF NOT EXISTS recording_policy ("
        "channel_id INTEGER PRIMARY KEY,"
        "mode TEXT NOT NULL DEFAULT 'continuous',"
        "pre_event_seconds INTEGER NOT NULL DEFAULT 5,"
        "post_event_seconds INTEGER NOT NULL DEFAULT 10,"
        "record_profile TEXT NOT NULL DEFAULT 'record',"
        "live_profile TEXT NOT NULL DEFAULT 'default',"
        "updated_at_utc TEXT NOT NULL"
        ");"
        "CREATE TABLE IF NOT EXISTS vms_events ("
        "id INTEGER PRIMARY KEY AUTOINCREMENT,"
        "channel_id INTEGER NOT NULL,"
        "event_type TEXT NOT NULL,"
        "event_state TEXT NOT NULL,"
        "wall_time_utc TEXT NOT NULL,"
        "source TEXT NOT NULL,"
        "source_event_name TEXT,"
        "metadata_json TEXT,"
        "segment_id INTEGER,"
        "created_at_utc TEXT NOT NULL,"
        "FOREIGN KEY(segment_id) REFERENCES recording_segments(id) ON DELETE SET NULL"
        ");"
        "CREATE INDEX IF NOT EXISTS idx_vms_events_channel_time "
        "ON vms_events(channel_id, wall_time_utc);"
        "CREATE INDEX IF NOT EXISTS idx_vms_events_type_time "
        "ON vms_events(event_type, wall_time_utc);"
        "CREATE TABLE IF NOT EXISTS vms_channels ("
        "channel_id INTEGER PRIMARY KEY CHECK(channel_id BETWEEN 1 AND 4),"
        "camera_channel INTEGER NOT NULL UNIQUE CHECK(camera_channel BETWEEN 0 AND 3),"
        "updated_at_utc TEXT NOT NULL"
        ");"
        "INSERT OR IGNORE INTO schema_migrations(version, applied_at_utc, description) "
        "VALUES (1, strftime('%Y-%m-%dT%H:%M:%fZ', 'now'), 'initial media schema');"
        "INSERT OR IGNORE INTO schema_migrations(version, applied_at_utc, description) "
        "VALUES (2, strftime('%Y-%m-%dT%H:%M:%fZ', 'now'), 'add camera to VMS channel mapping');");
}

void RecordingIndex::upsert_channel_mapping(int channel_id, int camera_channel) {
    if (channel_id < 1 || channel_id > 4 || camera_channel < 0 || camera_channel > 3) {
        throw std::runtime_error("channel mapping out of range");
    }
    std::lock_guard<std::mutex> lock(mutex_);
    static constexpr const char* sql =
        "INSERT INTO vms_channels(channel_id, camera_channel, updated_at_utc) VALUES (?, ?, ?) "
        "ON CONFLICT(channel_id) DO UPDATE SET "
        "camera_channel=excluded.camera_channel, updated_at_utc=excluded.updated_at_utc;";
    sqlite3_stmt* stmt = nullptr;
    check_sqlite(sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr), db_, "prepare channel mapping");
    sqlite3_bind_int(stmt, 1, channel_id);
    sqlite3_bind_int(stmt, 2, camera_channel);
    bind_text(stmt, 3, now_utc());
    check_sqlite(sqlite3_step(stmt), db_, "upsert channel mapping");
    sqlite3_finalize(stmt);
}

void RecordingIndex::mark_segment_opened(int channel_id, const std::string& location, const std::string& container,
                                         const std::string& codec) {
    std::lock_guard<std::mutex> lock(mutex_);
    static constexpr const char* sql =
        "INSERT INTO recording_segments("
        "channel_id, file_path, container, codec, start_wall_time_utc, complete, created_at_utc"
        ") VALUES (?, ?, ?, ?, ?, 0, ?);";

    sqlite3_stmt* stmt = nullptr;
    check_sqlite(sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr), db_, "prepare segment opened");
    const std::string now = now_utc();
    sqlite3_bind_int(stmt, 1, channel_id);
    bind_text(stmt, 2, location);
    bind_text(stmt, 3, container);
    bind_text(stmt, 4, codec);
    bind_text(stmt, 5, now);
    bind_text(stmt, 6, now);
    check_sqlite(sqlite3_step(stmt), db_, "insert segment opened");
    sqlite3_finalize(stmt);
    logger_.info("[record] opened segment: " + location);
}

void RecordingIndex::mark_segment_closed(const std::string& location) {
    std::lock_guard<std::mutex> lock(mutex_);
    static constexpr const char* sql =
        "UPDATE recording_segments "
        "SET end_wall_time_utc = ?, size_bytes = ?, complete = 1 "
        "WHERE file_path = ?;";

    sqlite3_stmt* stmt = nullptr;
    check_sqlite(sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr), db_, "prepare segment closed");
    bind_text(stmt, 1, now_utc());
    sqlite3_bind_int64(stmt, 2, static_cast<sqlite3_int64>(file_size_or_zero(location)));
    bind_text(stmt, 3, location);
    check_sqlite(sqlite3_step(stmt), db_, "update segment closed");
    sqlite3_finalize(stmt);
    logger_.info("[record] closed segment: " + location);
}

int RecordingIndex::busy_timeout_ms() const { return busy_timeout_ms_; }

void RecordingIndex::exec(const std::string& sql) {
    char* error = nullptr;
    const int rc = sqlite3_exec(db_, sql.c_str(), nullptr, nullptr, &error);
    if (rc != SQLITE_OK) {
        std::string message = error ? error : "unknown sqlite error";
        sqlite3_free(error);
        throw std::runtime_error("sqlite exec failed: " + message);
    }
}

void RecordingIndex::bind_text(sqlite3_stmt* stmt, int index, const std::string& value) {
    check_sqlite(sqlite3_bind_text(stmt, index, value.c_str(), -1, SQLITE_TRANSIENT), db_, "bind text");
}

std::string RecordingIndex::now_utc() const {
    const auto now = std::chrono::system_clock::now();
    const std::time_t t = std::chrono::system_clock::to_time_t(now);
    std::tm tm{};
    gmtime_r(&t, &tm);

    std::ostringstream out;
    out << std::put_time(&tm, "%Y-%m-%dT%H:%M:%SZ");
    return out.str();
}

std::int64_t RecordingIndex::file_size_or_zero(const std::string& path) const {
    std::error_code ec;
    const auto size = std::filesystem::file_size(path, ec);
    if (ec) {
        return 0;
    }
    return static_cast<std::int64_t>(size);
}

}  // namespace rtsps
