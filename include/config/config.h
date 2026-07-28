#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <unordered_map>

namespace config {

struct BotConfig {
    std::string token;
    std::uint64_t guild_id = 0;
};

std::unordered_map<std::string, std::string> parse_env_file(const std::filesystem::path& path);
BotConfig load_config();

} // namespace config
