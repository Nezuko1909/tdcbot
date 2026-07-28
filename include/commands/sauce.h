#pragma once

#include <dpp/dpp.h>

namespace commands {
    std::string create_sauce_command(dpp::slashcommand_t& event);
    void handle_sauce_command(const dpp::slashcommand_t& event);
}