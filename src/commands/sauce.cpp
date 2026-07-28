#include "commands/ping.h"
#include "utils/logger.h"

namespace commands {
    std::string create_sauce_command(const dpp::slashcommand_t& event) {
        const auto sauce = std::get<std::string>(event.get_parameter("link"));
    }

    void handle_sauce_command(const dpp::slashcommand_t& event) {
        logger::info("commands::sauce", "Handling /sauce command");
        std::string response = create_sauce_command(event);
        event.reply(response);
    }
}


