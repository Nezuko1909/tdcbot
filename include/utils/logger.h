#pragma once

#include <string>

namespace logger {

void init();
void info(const std::string& message);
void info(const std::string& tag, const std::string& message);
void warn(const std::string& message);
void error(const std::string& message);

} // namespace logger
