#include "commands/ping.h"
#include "utils/logger.h"

namespace commands {

dpp::slashcommand create_ping_command(dpp::snowflake guild_id) {
    dpp::slashcommand cmd("ping", "Replies with Pong!", guild_id);
    return cmd;
}

void handle_ping_command(const dpp::slashcommand_t& event) {
    logger::info("commands::ping", "Handling /ping command");
    event.reply("Pong!");
}

} // namespace commands
