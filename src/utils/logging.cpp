#include "logging.hpp"
#include <iostream>
#include <chrono>
#include <iomanip>
#include <sstream>
#include <cstdlib>
#include <cstring>
#include <algorithm>
#include <cctype>

namespace mali_wrapper {

namespace {

static uint32_t category_mask(LogCategory category)
{
    return static_cast<uint32_t>(category);
}

static std::string normalize_category_token(const std::string& token)
{
    std::string normalized;
    normalized.reserve(token.size());
    for (char ch : token) {
        if (!std::isspace(static_cast<unsigned char>(ch))) {
            normalized.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(ch))));
        }
    }
    return normalized;
}

static bool append_category_token(const std::string& token, uint32_t* mask)
{
    if (mask == nullptr || token.empty()) {
        return false;
    }

    if (token == "wrapper") {
        *mask |= category_mask(LogCategory::WRAPPER);
        return true;
    }

    if (token == "wsi") {
        *mask |= category_mask(LogCategory::WSI_LAYER);
        return true;
    }

    if (token == "low-address-map" || token == "low_address_map" ||
        token == "low-address" || token == "low_address" ||
        token == "lowaddr" || token == "lowaddressmap") {
        *mask |= category_mask(LogCategory::LOW_ADDRESS_MAP);
        return true;
    }

    return false;
}

} // namespace

Logger::Logger() {
    InitFromEnv();
}

Logger& Logger::Instance() {
    static Logger instance;
    return instance;
}

void Logger::SetLevel(LogLevel level) {
    level_ = level;
}

void Logger::SetCategory(LogCategory category) {
    category_ = category;
}

void Logger::EnableColors(bool enable) {
    colors_enabled_ = enable;
}

void Logger::SetOutputFile(const std::string& path) {
    if (!path.empty()) {
        file_stream_ = std::make_unique<std::ofstream>(path, std::ios::app);
    }
}

void Logger::EnableConsole(bool enable) {
    console_enabled_ = enable;
}

void Logger::Log(LogLevel level, LogCategory category, const std::string& message) {
    if (!ShouldLog(level, category)) {
        return;
    }

    auto now = std::chrono::system_clock::now();
    auto time_t = std::chrono::system_clock::to_time_t(now);
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        now.time_since_epoch()) % 1000;

    std::stringstream prefix;
    prefix << std::put_time(std::localtime(&time_t), "%Y-%m-%d %H:%M:%S");
    prefix << '.' << std::setfill('0') << std::setw(3) << ms.count();

    std::stringstream console_ss;
    console_ss << prefix.str();
    if (console_enabled_ && colors_enabled_) {
        console_ss << " [" << GetColorCode(level) << LevelToString(level) << GetResetCode() << "]["
                   << GetCategoryColor(category) << CategoryToString(category) << GetResetCode() << "] "
                   << message;
    } else {
        console_ss << " [" << LevelToString(level) << "][" << CategoryToString(category) << "] "
                   << message;
    }

    std::stringstream file_ss;
    file_ss << prefix.str()
            << " [" << LevelToString(level) << "][" << CategoryToString(category) << "] "
            << message;

    const std::string console_line = console_ss.str();
    const std::string file_line = file_ss.str();

    if (console_enabled_) {
        std::cout << console_line << std::endl;
    }

    if (file_stream_ && file_stream_->is_open()) {
        *file_stream_ << file_line << std::endl;
        file_stream_->flush();
    }
}

void Logger::LogF(LogLevel level, LogCategory category, const char* format, ...) {
    if (!ShouldLog(level, category)) {
        return;
    }

    va_list args;
    va_start(args, format);

    char buffer[1024];
    vsnprintf(buffer, sizeof(buffer), format, args);

    va_end(args);

    Log(level, category, std::string(buffer));
}

void Logger::Error(const std::string& message) {
    Log(LogLevel::ERROR, LogCategory::WRAPPER, message);
}

void Logger::Warn(const std::string& message) {
    Log(LogLevel::WARN, LogCategory::WRAPPER, message);
}

void Logger::Info(const std::string& message) {
    Log(LogLevel::INFO, LogCategory::WRAPPER, message);
}

void Logger::Debug(const std::string& message) {
    Log(LogLevel::DEBUG, LogCategory::WRAPPER, message);
}

void Logger::WsiError(const std::string& message) {
    Log(LogLevel::ERROR, LogCategory::WSI_LAYER, message);
}

void Logger::WsiWarn(const std::string& message) {
    Log(LogLevel::WARN, LogCategory::WSI_LAYER, message);
}

void Logger::WsiInfo(const std::string& message) {
    Log(LogLevel::INFO, LogCategory::WSI_LAYER, message);
}

void Logger::WsiDebug(const std::string& message) {
    Log(LogLevel::DEBUG, LogCategory::WSI_LAYER, message);
}

void Logger::LowAddressError(const std::string& message) {
    Log(LogLevel::ERROR, LogCategory::LOW_ADDRESS_MAP, message);
}

void Logger::LowAddressWarn(const std::string& message) {
    Log(LogLevel::WARN, LogCategory::LOW_ADDRESS_MAP, message);
}

void Logger::LowAddressInfo(const std::string& message) {
    Log(LogLevel::INFO, LogCategory::LOW_ADDRESS_MAP, message);
}

void Logger::LowAddressDebug(const std::string& message) {
    Log(LogLevel::DEBUG, LogCategory::LOW_ADDRESS_MAP, message);
}

void Logger::WsiLogF(LogLevel level, const char* format, ...) {
    if (!ShouldLog(level, LogCategory::WSI_LAYER)) {
        return;
    }

    va_list args;
    va_start(args, format);

    char buffer[1024];
    vsnprintf(buffer, sizeof(buffer), format, args);

    va_end(args);

    Log(level, LogCategory::WSI_LAYER, std::string(buffer));
}

void Logger::LowAddressLogF(LogLevel level, const char* format, ...) {
    if (!ShouldLog(level, LogCategory::LOW_ADDRESS_MAP)) {
        return;
    }

    va_list args;
    va_start(args, format);

    char buffer[1024];
    vsnprintf(buffer, sizeof(buffer), format, args);

    va_end(args);

    Log(level, LogCategory::LOW_ADDRESS_MAP, std::string(buffer));
}

void Logger::InitFromEnv() {
    const char* log_level = std::getenv("MALI_WRAPPER_LOG_LEVEL");
    if (log_level) {
        int level = std::atoi(log_level);
        if (level >= 0 && level <= 3) {
            level_ = static_cast<LogLevel>(level);
        }
    }

    const char* log_category = std::getenv("MALI_WRAPPER_LOG_CATEGORY");
    if (log_category) {
        const LogCategory parsed_category = ParseCategory(log_category);
        if (parsed_category == LogCategory::NONE) {
            LogCategoryWarning(log_category);
            category_ = LogCategory::NONE; // Disable logging for invalid category
        } else {
            category_ = parsed_category;
        }
    }

    const char* console = std::getenv("MALI_WRAPPER_LOG_CONSOLE");
    if (console && std::strcmp(console, "0") == 0) {
        console_enabled_ = false;
    }

    const char* colors = std::getenv("MALI_WRAPPER_LOG_COLORS");
    if (colors && std::strcmp(colors, "0") == 0) {
        colors_enabled_ = false;
    }

    const char* log_file = std::getenv("MALI_WRAPPER_LOG_FILE");
    if (log_file) {
        SetOutputFile(log_file);
    }
}

bool Logger::ShouldLog(LogLevel level, LogCategory category) const {
    if (level > level_ || category_ == LogCategory::NONE) {
        return false;
    }

    return (category_mask(category_) & category_mask(category)) != 0;
}

const char* Logger::LevelToString(LogLevel level) {
    switch (level) {
        case LogLevel::ERROR: return "ERROR";
        case LogLevel::WARN:  return "WARN";
        case LogLevel::INFO:  return "INFO";
        case LogLevel::DEBUG: return "DEBUG";
        default: return "UNKNOWN";
    }
}

LogCategory Logger::ParseCategory(const char* category_str) {
    if (!category_str) {
        return LogCategory::ALL;
    }

    std::string value(category_str);
    if (value.empty()) {
        return LogCategory::ALL;
    }

    uint32_t mask = 0;
    std::string token;
    bool saw_token = false;

    auto flush_token = [&]() -> bool {
        const std::string normalized = normalize_category_token(token);
        token.clear();
        if (normalized.empty()) {
            return false;
        }

        saw_token = true;
        return append_category_token(normalized, &mask);
    };

    for (char ch : value) {
        if (ch == '+' || ch == ',') {
            if (!flush_token()) {
                return LogCategory::NONE;
            }
            continue;
        }

        token.push_back(ch);
    }

    if (!token.empty() && !flush_token()) {
        return LogCategory::NONE;
    }

    if (!saw_token || mask == 0) {
        return LogCategory::NONE;
    }

    return static_cast<LogCategory>(mask);
}

std::string Logger::GetColorCode(LogLevel level) const {
    if (!colors_enabled_) return "";

    switch (level) {
        case LogLevel::ERROR: return "\033[1;31m"; // Bold Red
        case LogLevel::WARN:  return "\033[1;33m"; // Bold Yellow
        case LogLevel::INFO:  return "\033[1;36m"; // Bold Cyan
        case LogLevel::DEBUG: return "\033[1;35m"; // Bold Magenta
        default: return "";
    }
}

std::string Logger::GetResetCode() const {
    return colors_enabled_ ? "\033[0m" : "";
}

std::string Logger::GetCategoryColor(LogCategory category) const {
    if (!colors_enabled_) return "";

    switch (category) {
        case LogCategory::WRAPPER: return "\033[1;32m"; // Bold Green
        case LogCategory::WSI_LAYER: return "\033[1;34m"; // Bold Blue
        case LogCategory::LOW_ADDRESS_MAP: return "\033[1;35m"; // Bold Magenta
        case LogCategory::WRAPPER_WSI: return "\033[1;37m"; // Bold White
        case LogCategory::NONE: return "\033[1;31m"; // Bold Red
        default: return "";
    }
}

std::string Logger::CategoryToString(LogCategory category) const {
    if (category == LogCategory::NONE) {
        return "NONE";
    }

    std::string label;
    auto append = [&](LogCategory bit, const char* name) {
        if ((category_mask(category) & category_mask(bit)) == 0) {
            return;
        }

        if (!label.empty()) {
            label += '+';
        }
        label += name;
    };

    append(LogCategory::WRAPPER, "WRAPPER");
    append(LogCategory::WSI_LAYER, "WSI");
    append(LogCategory::LOW_ADDRESS_MAP, "LOW_ADDRESS_MAP");

    return label.empty() ? "UNKNOWN" : label;
}

void Logger::LogCategoryWarning(const char* invalid_category) {
    fprintf(stderr,
            "\033[1;31m[WARNING]\033[0m Unknown log category '%s'. Valid options: wrapper, wsi, low-address-map, or combinations joined with '+'. Logging disabled.\n",
            invalid_category);
}

} // namespace mali_wrapper
