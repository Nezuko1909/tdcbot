#include "commands/sauce.h"
#include "utils/logger.h"
#include "utils/http_client.h"
#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <ctime>
#include <dpp/json.h>
#include <memory>
#include <sstream>
#include <vector>
#include <mutex>
#include <future>

namespace commands {

// Shared HttpClient singleton configured specifically for Anime Reverse Search
net::HttpClient& get_sauce_http_client() {
    static net::HttpClient instance;
    static std::once_flag init_flag;

    std::call_once(init_flag, []() {
        // Set compliant User-Agent
        instance.set_default_user_agent(
            "SauceBot/0.1.0 (+https://github.com/Nezuko1909/tdcbot; kny1909.nezuko@gmail.com"
        );

        // Domain Policy 1: Trace.moe Official API
        net::DomainPolicy trace_policy;
        trace_policy.rate_limit_rps = 1.0;          // 1 req/sec limit
        trace_policy.burst_capacity = 3;
        trace_policy.max_concurrent_requests = 2;
        trace_policy.max_retries = 3;
        trace_policy.cache_ttl = std::chrono::seconds(600); // Cache 10 min
        instance.set_domain_policy("api.trace.moe", trace_policy);

        // Domain Policy 2: AniList Official GraphQL API
        net::DomainPolicy anilist_policy;
        anilist_policy.rate_limit_rps = 1.5;         // 90 req/min
        anilist_policy.burst_capacity = 5;
        anilist_policy.max_concurrent_requests = 3;
        anilist_policy.max_retries = 3;
        anilist_policy.cache_ttl = std::chrono::seconds(600);
        instance.set_domain_policy("graphql.anilist.co", anilist_policy);

        // Domain Policy 3: IQDB Search
        net::DomainPolicy iqdb_policy;
        iqdb_policy.rate_limit_rps = 0.5;            // Polite 1 req / 2 sec
        iqdb_policy.burst_capacity = 2;
        iqdb_policy.max_concurrent_requests = 1;
        iqdb_policy.max_retries = 2;
        iqdb_policy.cache_ttl = std::chrono::seconds(300);
        instance.set_domain_policy("iqdb.org", iqdb_policy);

        // Domain Policy 4: Ascii2D Search
        net::DomainPolicy ascii2d_policy;
        ascii2d_policy.rate_limit_rps = 0.5;         // Polite 1 req / 2 sec
        ascii2d_policy.burst_capacity = 2;
        ascii2d_policy.max_concurrent_requests = 1;
        ascii2d_policy.max_retries = 2;
        ascii2d_policy.cache_ttl = std::chrono::seconds(300);
        instance.set_domain_policy("ascii2d.net", ascii2d_policy);

        // Attach Structured Request Logger
        instance.set_log_callback([](const net::RequestLogEvent& ev) {
            std::ostringstream ss;
            ss << "[HTTP LOG] " << ev.method << " " << ev.url
               << " | Status: " << ev.status_code
               << " | Latency: " << ev.latency_ms << "ms"
               << " | Retries: " << ev.retry_count
               << " | RateLimitWait: " << ev.rate_limit_wait_ms << "ms"
               << " | Cached: " << (ev.cache_hit ? "YES" : "NO");
            if (!ev.error.empty()) {
                ss << " | Error: " << ev.error;
            }
            logger::info("SauceHttpClient", ss.str());
        });
    });

    return instance;
}

dpp::slashcommand create_sauce_slash_command() {
    return dpp::slashcommand(
               "sauce",
               "Search image source across multiple anime reverse search engines", 0)
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

// --- Engine 1: Trace.moe Official API ---
static SearchResult search_tracemoe(const std::string &image_url) {
    SearchResult res;
    res.engine_name = "Trace.moe (Anime Scene)";

    net::HttpRequest req;
    req.method = "GET";
    req.url = "https://api.trace.moe/search?url=" + dpp::utility::url_encode(image_url);
    req.timeout = std::chrono::milliseconds(8000);

    net::HttpResponse response = get_sauce_http_client().execute(req);

    if (!response.is_success() || response.body.empty()) {
        return res;
    }

    try {
        auto j = dpp::json::parse(response.body);
        if (j.contains("result") && j["result"].is_array() && !j["result"].empty()) {
            const auto &item = j["result"][0];
            if (item.contains("similarity") && item["similarity"].is_number()) {
                res.similarity = item["similarity"].get<float>() * 100.0f;
            }

            if (item.contains("anilist")) {
                const auto &anilist = item["anilist"];
                uint64_t id = 0;
                if (anilist.contains("id") && anilist["id"].is_number()) {
                    id = anilist["id"].get<uint64_t>();
                    res.source_url = "https://anilist.co/anime/" + std::to_string(id);
                }

                if (anilist.contains("title")) {
                    const auto &title_obj = anilist["title"];
                    if (title_obj.contains("english") && title_obj["english"].is_string() &&
                        !title_obj["english"].get<std::string>().empty()) {
                        res.title = title_obj["english"].get<std::string>();
                    } else if (title_obj.contains("romaji") && title_obj["romaji"].is_string() &&
                               !title_obj["romaji"].get<std::string>().empty()) {
                        res.title = title_obj["romaji"].get<std::string>();
                    } else if (title_obj.contains("native") && title_obj["native"].is_string()) {
                        res.title = title_obj["native"].get<std::string>();
                    }
                }
            }

            if (item.contains("episode") && item["episode"].is_number()) {
                int ep = item["episode"].get<int>();
                res.extra_info = "Episode " + std::to_string(ep);
            }

            if (item.contains("image") && item["image"].is_string()) {
                res.thumbnail_url = item["image"].get<std::string>();
            }
        }
    } catch (const std::exception &ex) {
        logger::warn("commands::sauce", std::string("Trace.moe parsing exception: ") + ex.what());
    }

    return res;
}

// --- Engine 2: IQDB Search ---
static SearchResult search_iqdb(const std::string &image_url) {
    SearchResult res;
    res.engine_name = "IQDB (Booru Search)";

    net::HttpRequest req;
    req.method = "POST";
    req.url = "https://iqdb.org/";
    req.headers["Content-Type"] = "application/x-www-form-urlencoded";
    req.body = "url=" + dpp::utility::url_encode(image_url);
    req.timeout = std::chrono::milliseconds(10000);

    net::HttpResponse response = get_sauce_http_client().execute(req);

    if (!response.is_success() || response.body.empty()) {
        return res;
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
                res.source_url = "https://" + html.substr(href_start, href_end - href_start);
            }
        }

        auto sim_pos = html.find("% similarity", match_pos);
        if (sim_pos != std::string::npos && sim_pos >= 5) {
            auto num_start = html.rfind(" ", sim_pos - 1);
            if (num_start != std::string::npos) {
                try {
                    res.similarity = std::stof(html.substr(num_start + 1, sim_pos - num_start - 1));
                } catch (...) {}
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

    return res;
}

// --- Engine 3: Ascii2D Search ---
static SearchResult search_ascii2d(const std::string &image_url) {
    SearchResult res;
    res.engine_name = "Ascii2D (Pixiv/Twitter)";

    net::HttpRequest req;
    req.method = "GET";
    req.url = "https://ascii2d.net/search/url/" + dpp::utility::url_encode(image_url);
    req.timeout = std::chrono::milliseconds(10000);

    net::HttpResponse response = get_sauce_http_client().execute(req);

    if (!response.is_success() || response.body.empty()) {
        return res;
    }

    const std::string &html = response.body;

    auto link_pos = html.find("https://www.pixiv.net/member_illust.php");
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

    return res;
}

// --- Aggregate Multi-Engine Search ---
static void perform_sauce_search(const std::string &image_url,
                                 std::function<void(dpp::message)> respond) {
    logger::info("commands::sauce", "Starting multi-engine search for image: " + image_url);

    // Launch queries concurrently using async worker threads
    std::thread([image_url, respond]() {
        auto fut_trace = std::async(std::launch::async, search_tracemoe, image_url);
        auto fut_iqdb = std::async(std::launch::async, search_iqdb, image_url);
        auto fut_ascii2d = std::async(std::launch::async, search_ascii2d, image_url);

        std::vector<SearchResult> results;
        results.push_back(fut_trace.get());
        results.push_back(fut_iqdb.get());
        results.push_back(fut_ascii2d.get());

        dpp::embed embed;
        embed.set_timestamp(time(nullptr));

        SearchResult best_match;
        float max_sim = -1.0f;

        for (const auto &r : results) {
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
                "Searched via Trace.moe, IQDB & Ascii2D (Polite HTTP Engine)"));

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
    }).detach();
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
        image_url,
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
                    image_url,
                    [event](dpp::message msg) { event.edit_response(msg); });
            });
    } else if (link.rfind("http://", 0) == 0 ||
               link.rfind("https://", 0) == 0) {
        perform_sauce_search(
            link,
            [event](dpp::message msg) { event.edit_response(msg); });
    } else {
        event.edit_response("Invalid link format. Please provide a valid "
                            "Discord message link or direct image URL.");
    }
}

} // namespace commands
