#pragma once

#include <dpp/dpp.h>

namespace commands {

dpp::slashcommand create_ping_command(dpp::snowflake guild_id = 0);
void handle_ping_command(const dpp::slashcommand_t& event);

} // namespace commands
