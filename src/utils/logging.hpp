#pragma once

#include <string>
#include <fstream>
#include <memory>
#include <cstdarg>
#include <cstdint>

namespace mali_wrapper {

enum class LogLevel {
    ERROR = 0,
    WARN = 1,
    INFO = 2,
    DEBUG = 3
};

enum class LogCategory : uint32_t {
    NONE = 0,
    WRAPPER = 1u << 0,
    WSI_LAYER = 1u << 1,
    LOW_ADDRESS_MAP = 1u << 2,
    WRAPPER_WSI = (1u << 0) | (1u << 1),
    ALL = (1u << 0) | (1u << 1) | (1u << 2)
};

class Logger {
public:
    static Logger& Instance();

    void SetLevel(LogLevel level);
    void SetCategory(LogCategory category);
    void SetOutputFile(const std::string& path);
    void EnableConsole(bool enable);
    void EnableColors(bool enable);

    void Log(LogLevel level, LogCategory category, const std::string& message);
    void LogF(LogLevel level, LogCategory category, const char* format, ...);

    void Error(const std::string& message);
    void Warn(const std::string& message);
    void Info(const std::string& message);
    void Debug(const std::string& message);

    void WsiError(const std::string& message);
    void WsiWarn(const std::string& message);
    void WsiInfo(const std::string& message);
    void WsiDebug(const std::string& message);
    void LowAddressError(const std::string& message);
    void LowAddressWarn(const std::string& message);
    void LowAddressInfo(const std::string& message);
    void LowAddressDebug(const std::string& message);

    void WsiLogF(LogLevel level, const char* format, ...);
    void LowAddressLogF(LogLevel level, const char* format, ...);
    
private:
    Logger();
    ~Logger() = default;
    Logger(const Logger&) = delete;
    Logger& operator=(const Logger&) = delete;

    void InitFromEnv();
    bool ShouldLog(LogLevel level, LogCategory category) const;
    LogCategory ParseCategory(const char* category_str);
    std::string GetColorCode(LogLevel level) const;
    std::string GetCategoryColor(LogCategory category) const;
    std::string GetResetCode() const;

    LogLevel level_ = LogLevel::ERROR;
    LogCategory category_ = LogCategory::ALL;
    std::unique_ptr<std::ofstream> file_stream_;
    bool console_enabled_ = true;
    bool colors_enabled_ = true;

    const char* LevelToString(LogLevel level);
    std::string CategoryToString(LogCategory category) const;
    void LogCategoryWarning(const char* invalid_category);
};

} // namespace mali_wrapper

#define LOG_ERROR(msg) mali_wrapper::Logger::Instance().Error(msg)
#define LOG_WARN(msg) mali_wrapper::Logger::Instance().Warn(msg)
#define LOG_INFO(msg) mali_wrapper::Logger::Instance().Info(msg)
#define LOG_DEBUG(msg) mali_wrapper::Logger::Instance().Debug(msg)

#define WSI_LOG_ERROR(format, ...) mali_wrapper::Logger::Instance().WsiLogF(mali_wrapper::LogLevel::ERROR, format, ##__VA_ARGS__)
#define WSI_LOG_WARNING(format, ...) mali_wrapper::Logger::Instance().WsiLogF(mali_wrapper::LogLevel::WARN, format, ##__VA_ARGS__)
#define WSI_LOG_INFO(format, ...) mali_wrapper::Logger::Instance().WsiLogF(mali_wrapper::LogLevel::INFO, format, ##__VA_ARGS__)
#define WSI_LOG_DEBUG(format, ...) mali_wrapper::Logger::Instance().WsiLogF(mali_wrapper::LogLevel::DEBUG, format, ##__VA_ARGS__)

#define LOW_ADDRESS_LOG_ERROR(msg) mali_wrapper::Logger::Instance().LowAddressError(msg)
#define LOW_ADDRESS_LOG_WARN(msg) mali_wrapper::Logger::Instance().LowAddressWarn(msg)
#define LOW_ADDRESS_LOG_INFO(msg) mali_wrapper::Logger::Instance().LowAddressInfo(msg)
#define LOW_ADDRESS_LOG_DEBUG(msg) mali_wrapper::Logger::Instance().LowAddressDebug(msg)

#define WSI_LOG(level, format, ...) \
    do { \
        if (level == 1) WSI_LOG_ERROR(format, ##__VA_ARGS__); \
        else if (level == 2) WSI_LOG_WARNING(format, ##__VA_ARGS__); \
        else if (level == 3) WSI_LOG_INFO(format, ##__VA_ARGS__); \
    } while(0)
