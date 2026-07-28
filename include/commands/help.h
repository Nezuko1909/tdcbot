#pragma once

#include <dpp/dpp.h>

namespace commands {

dpp::slashcommand create_help_command(dpp::snowflake guild_id = 0);
void handle_help_command(const dpp::slashcommand_t& event);

} // namespace commands
