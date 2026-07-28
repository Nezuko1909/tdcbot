#include "handlers/command_dispatcher.h"
#include "commands/help.h"
#include "commands/ping.h"
#include "utils/logger.h"

namespace handlers {

void dispatch_slash_command(const dpp::slashcommand_t& event) {
    const auto command_name = event.command.get_command_name();
    logger::info("handlers", "Dispatching slash command: " + command_name);

    if (command_name == "ping") {
        commands::handle_ping_command(event);
        return;
    }

    if (command_name == "help") {
        commands::handle_help_command(event);
        return;
    }

    event.reply("Unknown command. Try /help.");
}

} // namespace handlers
