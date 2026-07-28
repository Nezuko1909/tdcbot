#include "config/config.h"
#include <algorithm>
#include <cctype>
#include <fstream>
#include <sstream>
#include <stdexcept>

namespace config {

static std::string trim(const std::string& value) {
    auto start = value.find_first_not_of(" \t\r\n");
    auto end = value.find_last_not_of(" \t\r\n");
    if (start == std::string::npos || end == std::string::npos) {
        return {};
    }
    return value.substr(start, end - start + 1);
}

std::unordered_map<std::string, std::string> parse_env_file(const std::filesystem::path& path) {
    std::unordered_map<std::string, std::string> values;
    if (!std::filesystem::exists(path)) {
        return values;
    }

    std::ifstream file(path);
    if (!file.is_open()) {
        return values;
    }

    std::string line;
    while (std::getline(file, line)) {
        auto comment_pos = line.find('#');
        if (comment_pos != std::string::npos) {
            line.erase(comment_pos);
        }

        auto separator = line.find('=');
        if (separator == std::string::npos) {
            continue;
        }

        std::string key = trim(line.substr(0, separator));
        std::string value = trim(line.substr(separator + 1));

        if (!value.empty() && value.front() == '"' && value.back() == '"' && value.size() > 1) {
            value = value.substr(1, value.size() - 2);
        }

        if (!key.empty()) {
            values[key] = value;
        }
    }

    return values;
}

static std::string extract_json_value(const std::string& content, const std::string& key) {
    auto quoted_key = '"' + key + '"';
    auto key_pos = content.find(quoted_key);
    if (key_pos == std::string::npos) {
        return {};
    }

    auto colon_pos = content.find(':', key_pos + quoted_key.size());
    if (colon_pos == std::string::npos) {
        return {};
    }

    auto value_start = content.find_first_not_of(" \t\r\n", colon_pos + 1);
    if (value_start == std::string::npos) {
        return {};
    }

    if (content[value_start] == '"') {
        auto value_end = content.find('"', value_start + 1);
        if (value_end == std::string::npos) {
            return {};
        }
        return content.substr(value_start + 1, value_end - value_start - 1);
    }

    auto value_end = content.find_first_of(",}\n", value_start);
    if (value_end == std::string::npos) {
        return trim(content.substr(value_start));
    }
    return trim(content.substr(value_start, value_end - value_start));
}

static std::unordered_map<std::string, std::string> parse_json_config(const std::filesystem::path& path) {
    std::unordered_map<std::string, std::string> values;
    if (!std::filesystem::exists(path)) {
        return values;
    }

    std::ifstream file(path);
    if (!file.is_open()) {
        return values;
    }

    std::ostringstream buffer;
    buffer << file.rdbuf();
    std::string content = buffer.str();

    auto token = extract_json_value(content, "DISCORD_TOKEN");
    if (token.empty()) {
        token = extract_json_value(content, "token");
    }
    if (!token.empty()) {
        values["DISCORD_TOKEN"] = token;
    }

    auto guild_id = extract_json_value(content, "DISCORD_GUILD_ID");
    if (guild_id.empty()) {
        guild_id = extract_json_value(content, "guild_id");
    }
    if (!guild_id.empty()) {
        values["DISCORD_GUILD_ID"] = guild_id;
    }

    return values;
}

BotConfig load_config() {
    BotConfig bot_config;
    auto project_root = std::filesystem::current_path();
    auto local_env = project_root / ".env";
    auto config_env = project_root / "config" / ".env";
    auto config_json = project_root / "config" / "config.json";

    auto env_vars = parse_env_file(local_env);
    if (env_vars.empty()) {
        env_vars = parse_env_file(config_env);
    }

    if (const char* env_token = std::getenv("DISCORD_TOKEN")) {
        env_vars["DISCORD_TOKEN"] = env_token;
    }

    if (env_vars.find("DISCORD_TOKEN") == env_vars.end() && std::filesystem::exists(config_json)) {
        auto json_vars = parse_json_config(config_json);
        env_vars.insert(json_vars.begin(), json_vars.end());
    }

    if (env_vars.find("DISCORD_TOKEN") == env_vars.end() || env_vars["DISCORD_TOKEN"].empty()) {
        throw std::runtime_error("DISCORD_TOKEN is required in environment or config file");
    }

    bot_config.token = env_vars["DISCORD_TOKEN"];

    if (env_vars.find("DISCORD_GUILD_ID") != env_vars.end() && !env_vars["DISCORD_GUILD_ID"].empty()) {
        bot_config.guild_id = static_cast<std::uint64_t>(std::stoull(env_vars["DISCORD_GUILD_ID"]));
    }

    return bot_config;
}

} // namespace config
