#include "commands/trace.h"
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
#include <regex>
#include <iomanip>
#include <unordered_map>

namespace commands {

// --- Trace Result Item Structure (Captures ALL trace.moe info) ---

struct TraceResultItem {
    uint64_t anilist_id = 0;
    uint64_t mal_id = 0;
    std::string title_english;
    std::string title_romaji;
    std::string title_native;
    std::vector<std::string> synonyms;
    bool is_adult = false;
    std::string filename;
    int episode = 0;
    double from_sec = 0.0;
    double to_sec = 0.0;
    float similarity = 0.0f;
    std::string video_url;
    std::string image_url;
};

// --- Pagination Session Store ---

struct TraceSearchSession {
    std::string session_id;
    std::string query_image_url;
    std::vector<TraceResultItem> results;
    size_t current_page = 0;
    std::chrono::steady_clock::time_point created_at;
};

static std::mutex g_sessions_mutex;
static std::unordered_map<std::string, TraceSearchSession> g_trace_sessions;

// Cleanup sessions older than 30 minutes
static void cleanup_old_sessions() {
    auto now = std::chrono::steady_clock::now();
    for (auto it = g_trace_sessions.begin(); it != g_trace_sessions.end(); ) {
        if (now - it->second.created_at > std::chrono::minutes(30)) {
            it = g_trace_sessions.erase(it);
        } else {
            ++it;
        }
    }
}

// --- Diagnostic Logging Structures & Helpers ---

struct EngineDiagnosticReport {
    std::string engine_name = "Trace.moe (Anime Scene)";
    std::string request_url;
    int http_status = 0;
    int64_t response_time_ms = 0;
    bool http_success = false;
    std::string error_message;
    std::string raw_response_body;
    size_t parsed_result_count = 0;
    std::string engine_rejection_reason;
};

static std::string redact_sensitive_info(const std::string &input) {
    if (input.empty()) return "";
    std::string text = input;

    static const std::vector<std::pair<std::regex, std::string>> redaction_rules = {
        {std::regex("(api_key|apikey|key|token|access_token|secret|auth)=([^&\"\\s]+)", std::regex_constants::icase), "$1=[REDACTED]"},
        {std::regex("(Authorization|Bearer)\\s*:\\s*([^\\r\\n]+)", std::regex_constants::icase), "$1: [REDACTED]"},
        {std::regex("\"([^\"]*token|[^\"]*key|[^\"]*secret|[^\"]*password)\"\\s*:\\s*\"[^\"]*\"", std::regex_constants::icase), "\"$1\": \"[REDACTED]\""}
    };

    for (const auto& [pattern, replacement] : redaction_rules) {
        text = std::regex_replace(text, pattern, replacement);
    }
    return text;
}

static std::string format_and_truncate_body(const std::string &body, size_t max_length = 1000) {
    if (body.empty()) return "(Empty Response Body)";

    std::string formatted_body;
    try {
        auto parsed_json = dpp::json::parse(body);
        formatted_body = parsed_json.dump(2);
    } catch (...) {
        formatted_body = body;
    }

    formatted_body = redact_sensitive_info(formatted_body);

    if (formatted_body.length() > max_length) {
        size_t original_len = formatted_body.length();
        formatted_body = formatted_body.substr(0, max_length);
        formatted_body += "\n... [Truncated " + std::to_string(original_len - max_length) + " bytes]";
    }

    return formatted_body;
}

static void log_no_match_diagnostic(const std::string &query_image_url,
                                    const EngineDiagnosticReport &report) {
    std::ostringstream ss;
    ss << "\n================================================================================\n"
       << "               TRACE ANIME SEARCH NO-MATCH DIAGNOSTIC REPORT                    \n"
       << "================================================================================\n"
       << " Target Image URL : " << redact_sensitive_info(query_image_url) << "\n"
       << " Endpoint URL     : " << redact_sensitive_info(report.request_url) << "\n"
       << " HTTP Status      : " << report.http_status << (report.http_success ? " (OK)" : " (FAILED)") << "\n"
       << " Response Time    : " << report.response_time_ms << " ms\n"
       << " Request Succeeded: " << (report.http_success ? "YES" : "NO") << "\n"
       << " Error Message    : " << (report.error_message.empty() ? "None" : report.error_message) << "\n"
       << " Parsed Results   : " << report.parsed_result_count << " candidate(s)\n"
       << " Engine Status    : " << report.engine_rejection_reason << "\n"
       << "--------------------------------------------------------------------------------\n"
       << " Raw Response Body:\n";

    std::istringstream body_stream(format_and_truncate_body(report.raw_response_body, 1000));
    std::string line;
    while (std::getline(body_stream, line)) {
        ss << "   " << line << "\n";
    }

    ss << "================================================================================\n";

    logger::warn("TraceDiagnostic", ss.str());
}

// --- Time Formatting Helper (seconds to MM:SS or HH:MM:SS) ---

static std::string format_timestamp(double seconds) {
    int total_s = static_cast<int>(seconds);
    int h = total_s / 3600;
    int m = (total_s % 3600) / 60;
    int s = total_s % 60;

    std::ostringstream ss;
    if (h > 0) {
        ss << std::setfill('0') << std::setw(2) << h << ":"
           << std::setfill('0') << std::setw(2) << m << ":"
           << std::setfill('0') << std::setw(2) << s;
    } else {
        ss << std::setfill('0') << std::setw(2) << m << ":"
           << std::setfill('0') << std::setw(2) << s;
    }
    return ss.str();
}

// --- Shared HttpClient Singleton for Trace ---

net::HttpClient& get_trace_http_client() {
    static net::HttpClient instance;
    static std::once_flag init_flag;

    std::call_once(init_flag, []() {
        instance.set_default_user_agent(
            "SauceBot/0.1.0 (+https://github.com/Nezuko1909/tdcbot; kny1909.nezuko@gmail.com)"
        );

        net::DomainPolicy trace_policy;
        trace_policy.rate_limit_rps = 1.0;
        trace_policy.burst_capacity = 3;
        trace_policy.max_concurrent_requests = 2;
        trace_policy.max_retries = 3;
        trace_policy.cache_ttl = std::chrono::seconds(600);
        instance.set_domain_policy("api.trace.moe", trace_policy);

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
            logger::info("TraceHttpClient", ss.str());
        });
    });

    return instance;
}

dpp::slashcommand create_trace_slash_command() {
    return dpp::slashcommand(
               "trace",
               "Search anime scene and episode source using Trace.moe API", 0)
        .add_option(dpp::command_option(
            dpp::co_string, "link", "Discord message link or direct image URL",
            true));
}

dpp::slashcommand create_trace_context_command() {
    return dpp::slashcommand("trace", "", 0).set_type(dpp::ctxm_message);
}

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

// --- Build Discord Message for Single Result Page ---

static dpp::message build_trace_page_message(const TraceSearchSession &session) {
    dpp::message msg;
    size_t total_pages = session.results.size();
    size_t current_page = session.current_page;

    if (total_pages == 0 || current_page >= total_pages) {
        dpp::embed embed;
        embed.set_title("No Anime Scene Found")
             .set_description("Trace.moe returned no matching anime scene results.")
             .set_color(0xE74C3C)
             .set_image(session.query_image_url);
        msg.add_embed(embed);
        return msg;
    }

    const auto &item = session.results[current_page];

    dpp::embed embed;
    embed.set_timestamp(time(nullptr));

    uint32_t color = (item.similarity >= 80.0f)
                         ? 0x2ECC71
                         : ((item.similarity >= 50.0f) ? 0xF1C40F : 0xE74C3C);
    embed.set_color(color);

    // Exact Title Selection Logic: Priority Romaji -> English -> Native -> Filename
    std::string main_title;
    if (!item.title_romaji.empty()) {
        main_title = item.title_romaji;
    } else if (!item.title_english.empty()) {
        main_title = item.title_english;
    } else if (!item.title_native.empty()) {
        main_title = item.title_native;
    } else if (!item.filename.empty()) {
        main_title = item.filename;
    } else {
        main_title = "Unknown Anime Title";
    }

    embed.set_title(main_title);
    if (item.anilist_id > 0) {
        embed.set_url("https://anilist.co/anime/" + std::to_string(item.anilist_id));
    }

    // Display title variants if available
    if (!item.title_english.empty() && item.title_english != main_title) {
        embed.add_field("English Title", item.title_english, false);
    }
    if (!item.title_romaji.empty() && item.title_romaji != main_title) {
        embed.add_field("Romaji Title", item.title_romaji, false);
    }
    if (!item.title_native.empty() && item.title_native != main_title) {
        embed.add_field("Native Title", item.title_native, false);
    }

    // Similarity Score
    embed.add_field("Similarity Score", std::to_string(static_cast<int>(item.similarity)) + "%", true);

    // Episode & Timestamp
    std::string ep_str = (item.episode > 0) ? ("Episode " + std::to_string(item.episode)) : "N/A / Movie";
    std::string time_str = format_timestamp(item.from_sec) + " - " + format_timestamp(item.to_sec);
    embed.add_field("Episode & Timestamp", ep_str + " (" + time_str + ")", true);

    // Filename
    if (!item.filename.empty()) {
        embed.add_field("File Name", item.filename, false);
    }

    // External Database Links & IDs
    std::ostringstream links_ss;
    if (item.anilist_id > 0) {
        links_ss << "[AniList (ID: " << item.anilist_id << ")](https://anilist.co/anime/" << item.anilist_id << ")  ";
    }
    if (item.mal_id > 0) {
        links_ss << "|  [MyAnimeList (ID: " << item.mal_id << ")](https://myanimelist.net/anime/" << item.mal_id << ")  ";
    }
    if (!links_ss.str().empty()) {
        embed.add_field("Database Links", links_ss.str(), false);
    }

    // Video preview link
    if (!item.video_url.empty()) {
        embed.add_field("Video Preview", "[Click here to watch scene clip](" + item.video_url + ")", false);
    }

    // Synonyms
    if (!item.synonyms.empty()) {
        std::ostringstream syn_ss;
        for (size_t s = 0; s < std::min<size_t>(4, item.synonyms.size()); ++s) {
            syn_ss << (s > 0 ? ", " : "") << item.synonyms[s];
        }
        embed.add_field("Synonyms", syn_ss.str(), false);
    }

    // Adult Content Flag
    embed.add_field("Adult Content (NSFW)", item.is_adult ? "Yes (18+)" : "No", true);

    // Thumbnail
    if (!item.image_url.empty()) {
        embed.set_thumbnail(item.image_url);
    } else {
        embed.set_thumbnail(session.query_image_url);
    }

    embed.set_footer(dpp::embed_footer().set_text(
        "Result " + std::to_string(current_page + 1) + " of " + std::to_string(total_pages) + " | Trace.moe API"
    ));

    msg.add_embed(embed);

    // Discord Interactive Button Action Row
    if (total_pages > 1) {
        dpp::component action_row;
        action_row.set_type(dpp::cot_action_row);

        dpp::component prev_btn;
        prev_btn.set_type(dpp::cot_button);
        prev_btn.set_label("◀ Previous");
        prev_btn.set_style(dpp::cos_primary);
        prev_btn.set_id("trace_prev:" + session.session_id);
        if (current_page == 0) prev_btn.set_disabled(true);

        dpp::component page_btn;
        page_btn.set_type(dpp::cot_button);
        page_btn.set_label(std::to_string(current_page + 1) + " / " + std::to_string(total_pages));
        page_btn.set_style(dpp::cos_secondary);
        page_btn.set_id("trace_page:" + session.session_id);
        page_btn.set_disabled(true);

        dpp::component next_btn;
        next_btn.set_type(dpp::cot_button);
        next_btn.set_label("Next ▶");
        next_btn.set_style(dpp::cos_primary);
        next_btn.set_id("trace_next:" + session.session_id);
        if (current_page + 1 >= total_pages) next_btn.set_disabled(true);

        action_row.add_component(prev_btn);
        action_row.add_component(page_btn);
        action_row.add_component(next_btn);

        msg.add_component(action_row);
    }

    return msg;
}

// --- Query Trace.moe API & Robust JSON Title Field Parsing ---

static void perform_trace_search(const std::string &image_url,
                                 std::function<void(dpp::message)> respond) {
    logger::info("commands::trace", "Starting Trace.moe search for image: " + image_url);

    std::thread([image_url, respond]() {
        EngineDiagnosticReport diag;
        diag.request_url = "https://api.trace.moe/search?url=" + dpp::utility::url_encode(image_url);

        net::HttpRequest req;
        req.method = "GET";
        req.url = diag.request_url;
        req.timeout = std::chrono::milliseconds(10000);

        net::HttpResponse response = get_trace_http_client().execute(req);

        diag.http_status = response.status_code;
        diag.response_time_ms = response.latency.count();
        diag.http_success = response.is_success();
        diag.error_message = response.error_message;
        diag.raw_response_body = response.body;

        std::vector<TraceResultItem> items;

        if (response.is_success() && !response.body.empty()) {
            try {
                auto j = dpp::json::parse(response.body);
                if (j.contains("result") && j["result"].is_array()) {
                    const auto &res_array = j["result"];
                    diag.parsed_result_count = res_array.size();

                    for (const auto &item : res_array) {
                        TraceResultItem result_item;

                        if (item.contains("similarity") && item["similarity"].is_number()) {
                            result_item.similarity = item["similarity"].get<float>() * 100.0f;
                        }
                        if (item.contains("filename") && item["filename"].is_string()) {
                            result_item.filename = item["filename"].get<std::string>();
                        }
                        if (item.contains("episode")) {
                            if (item["episode"].is_number()) {
                                result_item.episode = item["episode"].get<int>();
                            }
                        }
                        if (item.contains("from") && item["from"].is_number()) {
                            result_item.from_sec = item["from"].get<double>();
                        }
                        if (item.contains("to") && item["to"].is_number()) {
                            result_item.to_sec = item["to"].get<double>();
                        }
                        if (item.contains("video") && item["video"].is_string()) {
                            result_item.video_url = item["video"].get<std::string>();
                        }
                        if (item.contains("image") && item["image"].is_string()) {
                            result_item.image_url = item["image"].get<std::string>();
                        }

                        // Direct "title" field at result root level (if present)
                        if (item.contains("title")) {
                            const auto &t = item["title"];
                            if (t.is_string() && !t.is_null()) {
                                result_item.title_english = t.get<std::string>();
                            } else if (t.is_object()) {
                                if (t.contains("english") && t["english"].is_string() && !t["english"].is_null()) {
                                    result_item.title_english = t["english"].get<std::string>();
                                }
                                if (t.contains("romaji") && t["romaji"].is_string() && !t["romaji"].is_null()) {
                                    result_item.title_romaji = t["romaji"].get<std::string>();
                                }
                                if (t.contains("native") && t["native"].is_string() && !t["native"].is_null()) {
                                    result_item.title_native = t["native"].get<std::string>();
                                }
                            }
                        }

                        // Embedded "anilist" object / ID field
                        if (item.contains("anilist")) {
                            const auto &anilist = item["anilist"];
                            if (anilist.is_number()) {
                                result_item.anilist_id = anilist.get<uint64_t>();
                            } else if (anilist.is_object()) {
                                if (anilist.contains("id") && anilist["id"].is_number()) {
                                    result_item.anilist_id = anilist["id"].get<uint64_t>();
                                }
                                if (anilist.contains("idMal") && anilist["idMal"].is_number()) {
                                    result_item.mal_id = anilist["idMal"].get<uint64_t>();
                                }
                                if (anilist.contains("isAdult") && anilist["isAdult"].is_boolean()) {
                                    result_item.is_adult = anilist["isAdult"].get<bool>();
                                }

                                if (anilist.contains("title")) {
                                    const auto &t = anilist["title"];
                                    if (t.is_string() && !t.is_null()) {
                                        if (result_item.title_english.empty()) {
                                            result_item.title_english = t.get<std::string>();
                                        }
                                    } else if (t.is_object()) {
                                        if (t.contains("english") && t["english"].is_string() && !t["english"].is_null()) {
                                            result_item.title_english = t["english"].get<std::string>();
                                        }
                                        if (t.contains("romaji") && t["romaji"].is_string() && !t["romaji"].is_null()) {
                                            result_item.title_romaji = t["romaji"].get<std::string>();
                                        }
                                        if (t.contains("native") && t["native"].is_string() && !t["native"].is_null()) {
                                            result_item.title_native = t["native"].get<std::string>();
                                        }
                                    }
                                }

                                if (anilist.contains("synonyms") && anilist["synonyms"].is_array()) {
                                    for (const auto &syn : anilist["synonyms"]) {
                                        if (syn.is_string() && !syn.is_null()) {
                                            result_item.synonyms.push_back(syn.get<std::string>());
                                        }
                                    }
                                }
                            }
                        }

                        items.push_back(result_item);
                    }
                }
            } catch (const std::exception &ex) {
                diag.engine_rejection_reason = std::string("Trace.moe JSON parsing exception: ") + ex.what();
            }
        } else {
            diag.engine_rejection_reason = "HTTP request failed or returned empty response body.";
        }

        if (items.empty()) {
            if (diag.engine_rejection_reason.empty()) {
                diag.engine_rejection_reason = "No matching scene results found in Trace.moe response array.";
            }
            log_no_match_diagnostic(image_url, diag);

            dpp::embed embed;
            embed.set_timestamp(time(nullptr));
            embed.set_title("No Anime Scene Found")
                 .set_description("Could not find matching anime scene results on Trace.moe.")
                 .set_color(0xE74C3C)
                 .set_image(image_url);
            respond(dpp::message().add_embed(embed));
            return;
        }

        // Store Pagination Session
        std::string session_id = std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + "_" + std::to_string(rand() % 10000);
        
        TraceSearchSession session;
        session.session_id = session_id;
        session.query_image_url = image_url;
        session.results = items;
        session.current_page = 0;
        session.created_at = std::chrono::steady_clock::now();

        {
            std::lock_guard<std::mutex> lock(g_sessions_mutex);
            cleanup_old_sessions();
            g_trace_sessions[session_id] = session;
        }

        dpp::message msg = build_trace_page_message(session);
        respond(msg);
    }).detach();
}

void handle_trace_button_click(const dpp::button_click_t &event) {
    std::string custom_id = event.custom_id;
    logger::info("commands::trace", "Handling button click: " + custom_id);

    auto colon_pos = custom_id.find(':');
    if (colon_pos == std::string::npos) return;

    std::string action = custom_id.substr(0, colon_pos);
    std::string session_id = custom_id.substr(colon_pos + 1);

    std::lock_guard<std::mutex> lock(g_sessions_mutex);
    auto it = g_trace_sessions.find(session_id);
    if (it == g_trace_sessions.end()) {
        event.reply(dpp::ir_channel_message_with_source, dpp::message("This search session has expired. Please run `/trace` again.").set_flags(dpp::m_ephemeral));
        return;
    }

    auto &session = it->second;
    if (action == "trace_prev") {
        if (session.current_page > 0) {
            session.current_page--;
        }
    } else if (action == "trace_next") {
        if (session.current_page + 1 < session.results.size()) {
            session.current_page++;
        }
    }

    dpp::message updated_msg = build_trace_page_message(session);
    event.reply(dpp::ir_update_message, updated_msg);
}

void handle_trace_context_menu(const dpp::message_context_menu_t &event) {
    logger::info("commands::trace", "Handling trace context menu command");

    event.thinking();

    const dpp::message &target = event.get_message();
    std::string image_url = extract_image_url_from_message(target);

    if (image_url.empty()) {
        event.edit_response(
            "No image attachment or image link found in the selected message.");
        return;
    }

    perform_trace_search(
        image_url,
        [event](dpp::message msg) { event.edit_response(msg); });
}

void handle_trace_slashcommand(const dpp::slashcommand_t &event) {
    logger::info("commands::trace", "Handling /trace slash command");

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

                perform_trace_search(
                    image_url,
                    [event](dpp::message msg) { event.edit_response(msg); });
            });
    } else if (link.rfind("http://", 0) == 0 ||
               link.rfind("https://", 0) == 0) {
        perform_trace_search(
            link,
            [event](dpp::message msg) { event.edit_response(msg); });
    } else {
        event.edit_response("Invalid link format. Please provide a valid "
                            "Discord message link or direct image URL.");
    }
}

} // namespace commands
