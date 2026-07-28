#include "utils/logger.h"
#include <chrono>
#include <iomanip>
#include <iostream>

namespace logger {

static std::string timestamp() {
    auto now = std::chrono::system_clock::now();
    auto now_time = std::chrono::system_clock::to_time_t(now);
    std::tm local_time{};
#if defined(_MSC_VER)
    localtime_s(&local_time, &now_time);
#else
    localtime_r(&now_time, &local_time);
#endif
    std::ostringstream formatter;
    formatter << std::put_time(&local_time, "%Y-%m-%d %H:%M:%S");
    return formatter.str();
}

void init() {
    // Placeholder for future logger initialization
}

void info(const std::string& message) {
    std::cout << timestamp() << " [INFO] " << message << std::endl;
}

void info(const std::string& tag, const std::string& message) {
    std::cout << timestamp() << " [INFO] [" << tag << "] " << message << std::endl;
}

void warn(const std::string& message) {
    std::cout << timestamp() << " [WARN] " << message << std::endl;
}

void error(const std::string& message) {
    std::cerr << timestamp() << " [ERROR] " << message << std::endl;
}

} // namespace logger
