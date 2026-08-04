#include "handlers/command_dispatcher.h"
#include "commands/help.h"
#include "commands/ping.h"
#include "commands/sauce.h"
#include "commands/trace.h"
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

        if (command_name == "sauce") {
            commands::handle_sauce_slashcommand(event);
            return;
        }

        if (command_name == "trace") {
            commands::handle_trace_slashcommand(event);
            return;
        }

        event.reply("Unknown command. Try /help.");
    }

    void dispatch_context_menu(const dpp::message_context_menu_t& event) {
        const auto command_name = event.command.get_command_name();
        logger::info("handlers", "Dispatching context command: " + command_name);

        if (command_name == "sauce") {
            commands::handle_sauce_context_menu(event);
            return;
        }

        if (command_name == "trace") {
            commands::handle_trace_context_menu(event);
            return;
        }

        event.reply("Unknown context command.");
    }

    void dispatch_button_click(const dpp::button_click_t& event) {
        logger::info("handlers", "Dispatching button click: " + event.custom_id);

        if (event.custom_id.rfind("trace_", 0) == 0) {
            commands::handle_trace_button_click(event);
            return;
        }
    }
}