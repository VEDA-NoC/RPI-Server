#include "rtsps/logger.h"

#include <iostream>

namespace rtsps {

Logger::Logger(LogLevel level) : level_(level) {}

bool Logger::enabled(LogLevel level) const { return static_cast<int>(level) <= static_cast<int>(level_); }

void Logger::log(LogLevel level, const std::string& message) const {
    if (!enabled(level)) {
        return;
    }
    std::lock_guard<std::mutex> lock(mutex_);
    std::cerr << "[" << to_string(level) << "] " << message << std::endl;
}

void Logger::error(const std::string& message) const { log(LogLevel::Error, message); }
void Logger::warn(const std::string& message) const { log(LogLevel::Warn, message); }
void Logger::info(const std::string& message) const { log(LogLevel::Info, message); }
void Logger::debug(const std::string& message) const { log(LogLevel::Debug, message); }
void Logger::trace(const std::string& message) const { log(LogLevel::Trace, message); }

}  // namespace rtsps
