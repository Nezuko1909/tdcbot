#include "commands/help.h"
#include "utils/logger.h"

namespace commands {

dpp::slashcommand create_help_command(dpp::snowflake guild_id) {
    dpp::slashcommand cmd("help", "Show available bot commands", guild_id);
    return cmd;
}

void handle_help_command(const dpp::slashcommand_t& event) {
    logger::info("commands::help", "Handling /help command");
    event.reply("Available commands: /ping, /help, /sauce (booru/artwork search), /trace (anime scene search via Trace.moe & AniList)");
}

} // namespace commands
