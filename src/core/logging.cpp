#include "boo/logging.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <iostream>

namespace boo {

namespace {
std::string currentIsoTimestamp() {
    using namespace std::chrono;
    const auto now = system_clock::now();
    const auto ms  = duration_cast<milliseconds>(now.time_since_epoch()) % 1000;
    const std::time_t tt = system_clock::to_time_t(now);
    std::tm localTm{};
#if defined(_WIN32)
    localtime_s(&localTm, &tt);
#else
    localtime_r(&tt, &localTm);
#endif
    char buf[40]{};
    std::snprintf(
        buf,
        sizeof(buf),
        "%04d-%02d-%02dT%02d:%02d:%02d.%03lld",
        localTm.tm_year + 1900,
        localTm.tm_mon + 1,
        localTm.tm_mday,
        localTm.tm_hour,
        localTm.tm_min,
        localTm.tm_sec,
        static_cast<long long>(ms.count()));
    return std::string(buf);
}

std::string toLowerCopy(std::string_view s) {
    std::string out(s);
    std::transform(out.begin(), out.end(), out.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return out;
}
} // namespace

const char* toString(LogLevel level) noexcept {
    switch (level) {
        case LogLevel::Debug: return "DEBUG";
        case LogLevel::Info:  return "INFO";
        case LogLevel::Warn:  return "WARN";
        case LogLevel::Error: return "ERROR";
    }
    return "INFO";
}

bool parseLogLevel(std::string_view text, LogLevel& out) noexcept {
    const std::string k = toLowerCopy(text);
    if (k == "debug") { out = LogLevel::Debug; return true; }
    if (k == "info")  { out = LogLevel::Info;  return true; }
    if (k == "warn" || k == "warning") { out = LogLevel::Warn; return true; }
    if (k == "error" || k == "err")    { out = LogLevel::Error; return true; }
    return false;
}

std::string redactUserPath(std::string_view rawPath) {
    std::string normalized(rawPath);
    for (char& c : normalized) {
        if (c == '\\') c = '/';
    }

    // Redact Windows /Users/<username>/ or C:/Users/<username>/ segments
    const std::string lower = toLowerCopy(normalized);
    const std::string marker = "/users/";
    const size_t pos = lower.find(marker);
    if (pos != std::string::npos) {
        const size_t userStart = pos + marker.size();
        const size_t nextSlash = normalized.find('/', userStart);
        const size_t prefixStart = (pos >= 2 && normalized[pos - 1] == ':') ? (pos - 2) : pos;
        if (nextSlash != std::string::npos) {
            return normalized.substr(0, prefixStart) + "<USER_HOME>" + normalized.substr(nextSlash);
        }
        return normalized.substr(0, prefixStart) + "<USER_HOME>";
    }
    return normalized;
}

Logger& Logger::instance() {
    static Logger sInstance;
    return sInstance;
}

void Logger::setMinLevel(LogLevel level) noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    minLevel_ = level;
}

LogLevel Logger::minLevel() const noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    return minLevel_;
}

void Logger::setConsoleEnabled(bool enabled) noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    consoleEnabled_ = enabled;
}

bool Logger::openLogFile(const std::filesystem::path& filePath, std::string* outError) {
    std::lock_guard<std::mutex> lock(mutex_);
    std::error_code ec;
    if (filePath.has_parent_path()) {
        std::filesystem::create_directories(filePath.parent_path(), ec);
    }
    fileStream_.close();
    fileStream_.open(filePath, std::ios::out | std::ios::trunc);
    if (!fileStream_.is_open()) {
        if (outError) *outError = "Failed to open log file: " + redactUserPath(filePath.string());
        return false;
    }
    return true;
}

void Logger::closeLogFile() noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    if (fileStream_.is_open()) {
        fileStream_.flush();
        fileStream_.close();
    }
}

void Logger::log(LogLevel level, std::string_view category, std::string_view message) {
    const std::string ts = currentIsoTimestamp();
    std::lock_guard<std::mutex> lock(mutex_);
    if (static_cast<uint8_t>(level) < static_cast<uint8_t>(minLevel_)) {
        return;
    }

    LogEntry entry{ts, level, std::string(category), std::string(message)};
    if (ringBuffer_.size() >= kMaxRingEntries) {
        ringBuffer_.pop_front();
    }
    ringBuffer_.push_back(entry);

    const std::string formatted =
        "[" + entry.timestampIso + "] [" + toString(level) + "] [" + entry.category + "] " + entry.message;

    if (consoleEnabled_) {
        if (level == LogLevel::Error || level == LogLevel::Warn) {
            std::cerr << formatted << '\n';
        } else {
            std::cout << formatted << '\n';
        }
    }
    if (fileStream_.is_open()) {
        fileStream_ << formatted << '\n';
        fileStream_.flush();
    }
}

std::vector<LogEntry> Logger::recentEntries(size_t maxCount) const {
    std::lock_guard<std::mutex> lock(mutex_);
    if (ringBuffer_.size() <= maxCount) {
        return std::vector<LogEntry>(ringBuffer_.begin(), ringBuffer_.end());
    }
    return std::vector<LogEntry>(ringBuffer_.end() - static_cast<ptrdiff_t>(maxCount), ringBuffer_.end());
}

void Logger::clearRecentEntries() {
    std::lock_guard<std::mutex> lock(mutex_);
    ringBuffer_.clear();
}

} // namespace boo
