#pragma once

#include <dpp/dpp.h>
#include "utils/http_client.h"

namespace commands {

// Shared HTTP client instance for reverse search commands
net::HttpClient& get_sauce_http_client();

// Slash and Context commands registration
dpp::slashcommand create_sauce_slash_command();   // "/sauce <link>"
dpp::slashcommand create_sauce_context_command(); // Context Menu "sauce"

// Event Handlers
void handle_sauce_slashcommand(const dpp::slashcommand_t& event);
void handle_sauce_context_menu(const dpp::message_context_menu_t& event);

} // namespace commands