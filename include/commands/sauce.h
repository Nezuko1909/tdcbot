#pragma once

#include <dpp/dpp.h>

namespace commands {
    dpp::slashcommand create_sauce_slash_command();   // "/sauce"
    dpp::slashcommand create_sauce_context_command();  // right-click "Sauce"

    void handle_sauce_slashcommand(const dpp::slashcommand_t& event);
    void handle_sauce_context_menu(const dpp::message_context_menu_t& event);
}