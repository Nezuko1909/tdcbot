#pragma once

#include <dpp/dpp.h>

namespace handlers {

void dispatch_slash_command(const dpp::slashcommand_t& event);
void dispatch_context_menu(const dpp::message_context_menu_t& event);
void dispatch_button_click(const dpp::button_click_t& event);

} // namespace handlers
