#pragma once

#include <deque>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

namespace boo {

enum class LogLevel : uint8_t {
    Debug = 0,
    Info  = 1,
    Warn  = 2,
    Error = 3
};

struct LogEntry {
    std::string timestampIso;
    LogLevel level = LogLevel::Info;
    std::string category;
    std::string message;
};

const char* toString(LogLevel level) noexcept;
bool parseLogLevel(std::string_view text, LogLevel& out) noexcept;

// Section 11: Redacts user profile/home directories from logged paths to protect local privacy.
std::string redactUserPath(std::string_view rawPath);

class Logger {
public:
    static Logger& instance();

    void setMinLevel(LogLevel level) noexcept;
    LogLevel minLevel() const noexcept;
    void setConsoleEnabled(bool enabled) noexcept;
    bool openLogFile(const std::filesystem::path& filePath, std::string* outError = nullptr);
    void closeLogFile() noexcept;

    void log(LogLevel level, std::string_view category, std::string_view message);

    std::vector<LogEntry> recentEntries(size_t maxCount = 64) const;
    void clearRecentEntries();

private:
    Logger() = default;
    mutable std::mutex mutex_;
    LogLevel minLevel_ = LogLevel::Info;
    bool consoleEnabled_ = true;
    std::ofstream fileStream_;
    std::deque<LogEntry> ringBuffer_;
    static constexpr size_t kMaxRingEntries = 256;
};

#define BOO_LOG_DEBUG(cat, msg) ::boo::Logger::instance().log(::boo::LogLevel::Debug, (cat), (msg))
#define BOO_LOG_INFO(cat, msg)  ::boo::Logger::instance().log(::boo::LogLevel::Info,  (cat), (msg))
#define BOO_LOG_WARN(cat, msg)  ::boo::Logger::instance().log(::boo::LogLevel::Warn,  (cat), (msg))
#define BOO_LOG_ERROR(cat, msg) ::boo::Logger::instance().log(::boo::LogLevel::Error, (cat), (msg))

} // namespace boo
