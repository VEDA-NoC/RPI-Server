#pragma once

#include <mutex>
#include <string>

#include "rtsps/app_config.h"

namespace rtsps {

class Logger {
public:
    explicit Logger(LogLevel level);

    bool enabled(LogLevel level) const;
    void log(LogLevel level, const std::string& message) const;

    void error(const std::string& message) const;
    void warn(const std::string& message) const;
    void info(const std::string& message) const;
    void debug(const std::string& message) const;
    void trace(const std::string& message) const;

private:
    LogLevel level_;
    mutable std::mutex mutex_;
};

}  // namespace rtsps
