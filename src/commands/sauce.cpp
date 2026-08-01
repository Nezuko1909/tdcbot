#include "commands/sauce.h"
#include "utils/logger.h"
#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <ctime>
#include <dpp/json.h>
#include <memory>
#include <sstream>
#include <vector>

namespace commands {

dpp::slashcommand create_sauce_slash_command() {
    return dpp::slashcommand(
               "sauce",
               "Search image source across multiple reverse search engines", 0)
        .add_option(dpp::command_option(
            dpp::co_string, "link", "Discord message link or direct image URL",
            true));
}

dpp::slashcommand create_sauce_context_command() {
    return dpp::slashcommand("sauce", "", 0).set_type(dpp::ctxm_message);
}

struct SearchResult {
    std::string engine_name;
    float similarity = 0.0f;
    std::string title;
    std::string author;
    std::string author_url;
    std::string source_url;
    std::string thumbnail_url;
    std::string extra_info;
};

static bool parse_discord_message_link(const std::string &link,
                                       dpp::snowflake &channel_id,
                                       dpp::snowflake &message_id) {
    auto last_slash = link.find_last_of('/');
    if (last_slash == std::string::npos || last_slash == link.size() - 1) {
        return false;
    }
    auto second_last_slash = link.find_last_of('/', last_slash - 1);
    if (second_last_slash == std::string::npos) {
        return false;
    }

    try {
        message_id = std::stoull(link.substr(last_slash + 1));
        channel_id = std::stoull(link.substr(
            second_last_slash + 1, last_slash - second_last_slash - 1));
        return true;
    } catch (...) {
        return false;
    }
}

static std::string extract_image_url_from_message(const dpp::message &target) {
    if (!target.attachments.empty()) {
        for (const auto &att : target.attachments) {
            if (att.url.find(".png") != std::string::npos ||
                att.url.find(".jpg") != std::string::npos ||
                att.url.find(".jpeg") != std::string::npos ||
                att.url.find(".webp") != std::string::npos ||
                att.url.find(".gif") != std::string::npos ||
                att.content_type.find("image/") != std::string::npos) {
                return att.url;
            }
        }
        return target.attachments[0].url;
    }

    for (const auto &embed : target.embeds) {
        if (embed.image.has_value() && !embed.image->url.empty()) {
            return embed.image->url;
        }
        if (embed.thumbnail.has_value() && !embed.thumbnail->url.empty()) {
            return embed.thumbnail->url;
        }
    }

    std::istringstream iss(target.content);
    std::string token;
    while (iss >> token) {
        if (token.rfind("http://", 0) == 0 || token.rfind("https://", 0) == 0) {
            if (token.find(".png") != std::string::npos ||
                token.find(".jpg") != std::string::npos ||
                token.find(".jpeg") != std::string::npos ||
                token.find(".webp") != std::string::npos ||
                token.find(".gif") != std::string::npos) {
                return token;
            }
        }
    }

    return "";
}

// --- Engine 1: Trace.moe (Anime Scene Search API) ---
static void search_tracemoe(dpp::cluster &bot, const std::string &image_url,
                            std::function<void(SearchResult)> callback) {
    std::string api_url = "https://api.trace.moe/search?url=" +
                          dpp::utility::url_encode(image_url);

    bot.request(
        api_url, dpp::m_get,
        [callback, image_url](const dpp::http_request_completion_t &response) {
            SearchResult res;
            res.engine_name = "Trace.moe (Anime Scene)";

            if (response.status != 200 || response.body.empty()) {
                callback(res);
                return;
            }

            try {
                auto j = dpp::json::parse(response.body);
                if (j.contains("result") && j["result"].is_array() &&
                    !j["result"].empty()) {
                    const auto &item = j["result"][0];
                    if (item.contains("similarity") &&
                        item["similarity"].is_number()) {
                        res.similarity =
                            item["similarity"].get<float>() * 100.0f;
                    }

                    if (item.contains("anilist")) {
                        const auto &anilist = item["anilist"];
                        uint64_t id = 0;
                        if (anilist.contains("id") &&
                            anilist["id"].is_number()) {
                            id = anilist["id"].get<uint64_t>();
                            res.source_url = "https://anilist.co/anime/" +
                                             std::to_string(id);
                        }

                        if (anilist.contains("title")) {
                            const auto &title_obj = anilist["title"];
                            if (title_obj.contains("english") &&
                                title_obj["english"].is_string() &&
                                !title_obj["english"]
                                     .get<std::string>()
                                     .empty()) {
                                res.title =
                                    title_obj["english"].get<std::string>();
                            } else if (title_obj.contains("romaji") &&
                                       title_obj["romaji"].is_string() &&
                                       !title_obj["romaji"]
                                            .get<std::string>()
                                            .empty()) {
                                res.title =
                                    title_obj["romaji"].get<std::string>();
                            } else if (title_obj.contains("native") &&
                                       title_obj["native"].is_string()) {
                                res.title =
                                    title_obj["native"].get<std::string>();
                            }
                        }
                    }

                    if (item.contains("episode") &&
                        item["episode"].is_number()) {
                        int ep = item["episode"].get<int>();
                        res.extra_info = "Episode " + std::to_string(ep);
                    }

                    if (item.contains("image") && item["image"].is_string()) {
                        res.thumbnail_url = item["image"].get<std::string>();
                    }
                }
            } catch (const std::exception &ex) {
                logger::warn("commands::sauce",
                             std::string("Trace.moe parsing exception: ") +
                                 ex.what());
            }

            callback(res);
        });
}

// --- Engine 2: IQDB (Anime & Booru Multi-Service Search) ---
static void search_iqdb(dpp::cluster &bot, const std::string &image_url,
                        std::function<void(SearchResult)> callback) {
    std::string postdata = "url=" + dpp::utility::url_encode(image_url);
    std::multimap<std::string, std::string> headers = {
        {"Content-Type", "application/x-www-form-urlencoded"},
        {"User-Agent", "Mozilla/5.0 (X11; Linux x86_64)"}};

    bot.request(
        "https://iqdb.org/", dpp::m_post,
        [callback](const dpp::http_request_completion_t &response) {
            SearchResult res;
            res.engine_name = "IQDB (Booru Search)";

            if (response.status != 200 || response.body.empty()) {
                callback(res);
                return;
            }

            const std::string &html = response.body;

            auto match_pos = html.find("Best match");
            if (match_pos == std::string::npos) {
                match_pos = html.find("Additional match");
            }

            if (match_pos != std::string::npos) {
                auto href_pos = html.find("href=\"//", match_pos);
                if (href_pos != std::string::npos) {
                    auto href_start = href_pos + 8;
                    auto href_end = html.find("\"", href_start);
                    if (href_end != std::string::npos) {
                        res.source_url =
                            "https://" +
                            html.substr(href_start, href_end - href_start);
                    }
                }

                auto sim_pos = html.find("% similarity", match_pos);
                if (sim_pos != std::string::npos && sim_pos >= 5) {
                    auto num_start = html.rfind(" ", sim_pos - 1);
                    if (num_start != std::string::npos) {
                        try {
                            res.similarity = std::stof(html.substr(
                                num_start + 1, sim_pos - num_start - 1));
                        } catch (...) {
                        }
                    }
                }

                auto alt_pos = html.find("alt=\"", match_pos);
                if (alt_pos != std::string::npos) {
                    auto alt_start = alt_pos + 5;
                    auto alt_end = html.find("\"", alt_start);
                    if (alt_end != std::string::npos) {
                        res.title = html.substr(alt_start, alt_end - alt_start);
                    }
                }
            }

            callback(res);
        },
        postdata, "application/x-www-form-urlencoded", headers);
}

// --- Engine 3: Ascii2D (Pixiv & Twitter Artwork Search) ---
static void search_ascii2d(dpp::cluster &bot, const std::string &image_url,
                           std::function<void(SearchResult)> callback) {
    std::string search_url =
        "https://ascii2d.net/search/url/" + dpp::utility::url_encode(image_url);
    std::multimap<std::string, std::string> headers = {
        {"User-Agent", "Mozilla/5.0 (X11; Linux x86_64)"}};

    bot.request(
        search_url, dpp::m_get,
        [callback](const dpp::http_request_completion_t &response) {
            SearchResult res;
            res.engine_name = "Ascii2D (Pixiv/Twitter)";

            if (response.status != 200 || response.body.empty()) {
                callback(res);
                return;
            }

            const std::string &html = response.body;

            auto link_pos =
                html.find("https://www.pixiv.net/member_illust.php");
            if (link_pos == std::string::npos) {
                link_pos = html.find("https://www.pixiv.net/artworks/");
            }
            if (link_pos == std::string::npos) {
                link_pos = html.find("https://twitter.com/");
            }
            if (link_pos == std::string::npos) {
                link_pos = html.find("https://x.com/");
            }

            if (link_pos != std::string::npos) {
                auto link_end = html.find("\"", link_pos);
                if (link_end != std::string::npos) {
                    res.source_url = html.substr(link_pos, link_end - link_pos);
                    res.similarity = 85.0f;
                    res.title = "Matched Artwork Post";
                }
            }

            callback(res);
        },
        "", "text/plain", headers);
}

// --- Aggregate Multi-Engine Search ---
static void perform_sauce_search(dpp::cluster &bot,
                                 const std::string &image_url,
                                 std::function<void(dpp::message)> respond) {
    logger::info("commands::sauce",
                 "Starting multi-engine search for image: " + image_url);

    struct SearchState {
        std::atomic<int> pending_requests{3};
        std::vector<SearchResult> results;
        std::mutex mtx;
    };

    auto state = std::make_shared<SearchState>();

    auto check_complete = [state, image_url, respond]() {
        if (state->pending_requests.fetch_sub(1) == 1) {
            dpp::embed embed;
            embed.set_timestamp(time(nullptr));

            SearchResult best_match;
            float max_sim = -1.0f;

            for (const auto &r : state->results) {
                if (r.similarity > max_sim && !r.source_url.empty()) {
                    max_sim = r.similarity;
                    best_match = r;
                }
            }

            if (max_sim <= 0.0f || best_match.source_url.empty()) {
                embed.set_title("❓ No Sauce Found")
                    .set_description(
                        "Could not find matching image sources across "
                        "Trace.moe, IQDB, or Ascii2D engines.")
                    .set_color(0xE74C3C)
                    .set_image(image_url);
                respond(dpp::message().add_embed(embed));
                return;
            }

            uint32_t color =
                (best_match.similarity >= 80.0f)
                    ? 0x2ECC71
                    : ((best_match.similarity >= 50.0f) ? 0xF1C40F : 0xE74C3C);

            embed.set_title("Multi-Engine Image Search Results")
                .set_color(color)
                .set_footer(dpp::embed_footer().set_text(
                    "Searched across Trace.moe, IQDB & Ascii2D engines"));

            if (!best_match.thumbnail_url.empty()) {
                embed.set_thumbnail(best_match.thumbnail_url);
            } else {
                embed.set_thumbnail(image_url);
            }

            embed.add_field("Best Engine", best_match.engine_name, true);
            embed.add_field(
                "Similarity",
                std::to_string(static_cast<int>(best_match.similarity)) + "%",
                true);

            if (!best_match.title.empty()) {
                embed.add_field("Title",
                                "[" + best_match.title + "](" +
                                    best_match.source_url + ")",
                                false);
            } else {
                embed.add_field("Title", "Matched Result", false);
            }

            if (!best_match.extra_info.empty()) {
                embed.add_field("Details", best_match.extra_info, true);
            }

            if (!best_match.author.empty()) {
                if (!best_match.author_url.empty()) {
                    embed.add_field("Author",
                                    "[" + best_match.author + "](" +
                                        best_match.author_url + ")",
                                    false);
                } else {
                    embed.add_field("Author", best_match.author, false);
                }
            }

            embed.add_field("Source Link",
                            "[Click here to view original post](" +
                                best_match.source_url + ")",
                            false);
            embed.add_field("Original Image", "[View Image](" + image_url + ")",
                            false);

            respond(dpp::message().add_embed(embed));
        }
    };

    auto on_engine_result = [state, check_complete](SearchResult res) {
        {
            std::lock_guard<std::mutex> lock(state->mtx);
            state->results.push_back(res);
        }
        check_complete();
    };

    search_tracemoe(bot, image_url, on_engine_result);
    search_iqdb(bot, image_url, on_engine_result);
    search_ascii2d(bot, image_url, on_engine_result);
}

void handle_sauce_context_menu(const dpp::message_context_menu_t &event) {
    logger::info("commands::sauce", "Handling sauce context menu command");

    event.thinking();

    const dpp::message &target = event.get_message();
    std::string image_url = extract_image_url_from_message(target);

    if (image_url.empty()) {
        event.edit_response(
            "No image attachment or image link found in the selected message.");
        return;
    }

    perform_sauce_search(
        *event.from()->creator, image_url,
        [event](dpp::message msg) { event.edit_response(msg); });
}

void handle_sauce_slashcommand(const dpp::slashcommand_t &event) {
    logger::info("commands::sauce", "Handling /sauce slash command");

    const auto link = std::get<std::string>(event.get_parameter("link"));

    event.thinking();

    dpp::snowflake channel_id = 0;
    dpp::snowflake message_id = 0;

    if (parse_discord_message_link(link, channel_id, message_id)) {
        event.from()->creator->message_get(
            message_id, channel_id,
            [event](const dpp::confirmation_callback_t &cb) {
                if (cb.is_error()) {
                    event.edit_response("Message not found (invalid link or "
                                        "bot lacks permission).");
                    return;
                }
                dpp::message target = std::get<dpp::message>(cb.value);
                std::string image_url = extract_image_url_from_message(target);

                if (image_url.empty()) {
                    event.edit_response("No image attachment or image link "
                                        "found in the target message.");
                    return;
                }

                perform_sauce_search(
                    *event.from()->creator, image_url,
                    [event](dpp::message msg) { event.edit_response(msg); });
            });
    } else if (link.rfind("http://", 0) == 0 ||
               link.rfind("https://", 0) == 0) {
        perform_sauce_search(
            *event.from()->creator, link,
            [event](dpp::message msg) { event.edit_response(msg); });
    } else {
        event.edit_response("Invalid link format. Please provide a valid "
                            "Discord message link or direct image URL.");
    }
}

} // namespace commands
