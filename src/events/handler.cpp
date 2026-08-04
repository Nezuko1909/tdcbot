#include "events/handler.h"
#include "commands/help.h"
#include "commands/ping.h"
#include "commands/sauce.h"
#include "commands/trace.h"
#include "handlers/command_dispatcher.h"
#include "utils/logger.h"

namespace events {

void register_events(dpp::cluster& bot) {
    bot.on_ready([&bot](const dpp::ready_t& event) {
        (void)event;
        logger::info("events", "Bot is online and ready");

        // Register bot slash commands once the bot is ready
        bot.global_command_create(commands::create_ping_command());
        bot.global_command_create(commands::create_help_command());
        bot.global_command_create(commands::create_sauce_slash_command());
        bot.global_command_create(commands::create_sauce_context_command());
        bot.global_command_create(commands::create_trace_slash_command());
        bot.global_command_create(commands::create_trace_context_command());
    });

    bot.on_slashcommand([](const dpp::slashcommand_t& event) {
        logger::info("events", "Slash command received: " + event.command.get_command_name());
        handlers::dispatch_slash_command(event);
    });

    bot.on_message_context_menu([](const dpp::message_context_menu_t& event) {
        logger::info("events", "Context menu command received: " + event.command.get_command_name());
        handlers::dispatch_context_menu(event);
    });

    bot.on_button_click([](const dpp::button_click_t& event) {
        logger::info("events", "Button click received: " + event.custom_id);
        handlers::dispatch_button_click(event);
    });
}

} // namespace events
