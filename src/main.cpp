#include "config/config.h"
#include "events/handler.h"
#include "utils/logger.h"
#include <cstdlib>
#include <iostream>

int main() {
    try {
        logger::init();
        auto bot_config = config::load_config();

        logger::info("main", "Starting bot");
        dpp::cluster bot(bot_config.token);

        events::register_events(bot);
        bot.start(dpp::st_wait);
    }
    catch (const std::exception& ex) {
        std::cerr << "Startup error: " << ex.what() << std::endl;
        return EXIT_FAILURE;
    }

    return EXIT_SUCCESS;
}
