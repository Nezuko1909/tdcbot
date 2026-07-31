#include "commands/sauce.h"
#include "utils/logger.h"
#include <curl/curl.h>

namespace commands {
    dpp::slashcommand create_sauce_slash_command() {
        return dpp::slashcommand("sauce", "", 0).add_option(dpp::command_option(dpp::co_string, "link", "link", true));
    }

    dpp::slashcommand create_sauce_context_command() {
        return dpp::slashcommand("sauce", "", 0).set_type(dpp::ctxm_message);
    }

    std::vector<std::string> find_saucenao(const std::string& image_url) {
        // TODO : Implement the logic to call the SauceNAO API and return the result.
        std::vector<std::string> results;

        
    }

    static void process_sauce(dpp::cluster& bot, const dpp::message& target, std::function<void(const std::string&)> respond) {
        std::string mes_content = target.content;

        if (!target.attachments.empty()) {
            mes_content += "\n\n # Image: \n" + target.attachments[0].url;
        }

        for (const auto& attachment : target.attachments) {
            std::vector<std::string> results = find_saucenao(attachment.url);
        }

        for (auto &res : results) {
            mes_content += "\n\n # SauceNAO Result: \n" + res;
        }

        respond(mes_content);
    }

    // --- Handler cho context menu (right-click -> Apps -> Sauce) ---

    void handle_sauce_context_menu(const dpp::message_context_menu_t& event) {
        logger::info("commands::sauce", "Handling sauce context menu command");

        const dpp::message& target = event.get_message();
        process_sauce(*event.from()->creator, target, [&event](const std::string& text) {
            event.reply(text);
        });
    }

    void handle_sauce_slashcommand(const dpp::slashcommand_t& event) {
        logger::info("commands::sauce", "Handling /sauce slash command");

        const auto link = std::get<std::string>(event.get_parameter("link"));

        auto last_slash = link.find_last_of('/');
        auto second_last_slash = link.find_last_of('/', last_slash - 1);
        if (last_slash == std::string::npos || second_last_slash == std::string::npos) {
            event.reply(dpp::message("Link not valid.").set_flags(dpp::m_ephemeral));
            return;
        }

        dpp::snowflake channel_id = std::stoull(link.substr(second_last_slash + 1, last_slash - second_last_slash - 1));
        dpp::snowflake message_id = std::stoull(link.substr(last_slash + 1));

        event.thinking(); // message_get is async -> defer 

        event.from()->creator->message_get(message_id, channel_id, [event](const dpp::confirmation_callback_t& cb) {
            if (cb.is_error()) {
                event.edit_response("Message not found (invalid link or bot lacks permission).");
                return;
            }
            dpp::message target = std::get<dpp::message>(cb.value);
            process_sauce(*event.from()->creator, target, [&event](const std::string& text) {
                event.edit_response(text);
            });
        });
    }
}


