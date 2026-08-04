#pragma once

#include <dpp/dpp.h>
#include "utils/http_client.h"

namespace commands {

// Shared HTTP client instance for trace command (Trace.moe API)
net::HttpClient& get_trace_http_client();

// Slash and Context commands registration
dpp::slashcommand create_trace_slash_command();   // "/trace <link>"
dpp::slashcommand create_trace_context_command(); // Context Menu "trace"

// Event Handlers
void handle_trace_slashcommand(const dpp::slashcommand_t& event);
void handle_trace_context_menu(const dpp::message_context_menu_t& event);
void handle_trace_button_click(const dpp::button_click_t& event);

} // namespace commands
