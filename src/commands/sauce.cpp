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
#include <regex>
#include <iomanip>

namespace commands {

// --- Diagnostic Logging Structures ---

struct CandidateDetail {
    std::string title;
    float similarity = 0.0f;
    std::string source_url;
    std::string thumbnail_url;
    std::string extra_info;
    std::string id_info;
    bool accepted = false;
    std::string rejection_reason;
};

struct EngineDiagnosticReport {
    std::string engine_name;
    std::string request_url;
    int http_status = 0;
    int64_t response_time_ms = 0;
    bool http_success = false;
    std::string error_message;
    std::string raw_response_body;
    size_t parsed_result_count = 0;
    std::vector<CandidateDetail> extracted_candidates;
    std::string engine_rejection_reason;
};

// --- Sensitive Data Redaction & Pretty Printing Helpers ---

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
        formatted_body = body; // Fallback to raw HTML / plain text
    }

    formatted_body = redact_sensitive_info(formatted_body);

    if (formatted_body.length() > max_length) {
        size_t original_len = formatted_body.length();
        formatted_body = formatted_body.substr(0, max_length);
        formatted_body += "\n... [Truncated " + std::to_string(original_len - max_length) + " bytes]";
    }

    return formatted_body;
}

// --- Diagnostic Report Output Function ---

static void log_no_match_diagnostic(const std::string &query_image_url,
                                    const std::vector<EngineDiagnosticReport> &reports) {
    int total_queried = static_cast<int>(reports.size());
    int http_successful = 0;
    int valid_parsed_responses = 0;
    int accepted_matches = 0;

    std::ostringstream ss;
    ss << "\n================================================================================\n"
       << "              SAUCE REVERSE SEARCH NO-MATCH DIAGNOSTIC REPORT                   \n"
       << "================================================================================\n"
       << " Target Image URL : " << redact_sensitive_info(query_image_url) << "\n"
       << " Engines Queried  : " << total_queried << "\n"
       << "--------------------------------------------------------------------------------";

    for (size_t i = 0; i < reports.size(); ++i) {
        const auto &r = reports[i];
        if (r.http_success) http_successful++;
        if (r.parsed_result_count > 0) valid_parsed_responses++;
        
        bool has_accepted = false;
        for (const auto &cand : r.extracted_candidates) {
            if (cand.accepted) {
                has_accepted = true;
                break;
            }
        }
        if (has_accepted) accepted_matches++;

        ss << "\n[ENGINE " << (i + 1) << "/" << total_queried << "] " << r.engine_name << "\n"
           << "  Endpoint URL     : " << redact_sensitive_info(r.request_url) << "\n"
           << "  HTTP Status      : " << r.http_status << (r.http_success ? " (OK)" : " (FAILED)") << "\n"
           << "  Response Time    : " << r.response_time_ms << " ms\n"
           << "  Request Succeeded: " << (r.http_success ? "YES" : "NO") << "\n"
           << "  Error Message    : " << (r.error_message.empty() ? "None" : r.error_message) << "\n"
           << "  Parsed Results   : " << r.parsed_result_count << " candidate(s)\n"
           << "  Engine Status    : " << r.engine_rejection_reason << "\n";

        if (!r.extracted_candidates.empty()) {
            ss << "  Extracted Candidates:\n";
            for (size_t c_idx = 0; c_idx < r.extracted_candidates.size(); ++c_idx) {
                const auto &cand = r.extracted_candidates[c_idx];
                ss << "    Candidate #" << (c_idx + 1) << ":\n"
                   << "      Anime/Manga Title : " << (cand.title.empty() ? "(N/A)" : cand.title) << "\n"
                   << "      Similarity Score  : " << std::fixed << std::setprecision(1) << cand.similarity << "%\n"
                   << "      Source URL        : " << (cand.source_url.empty() ? "(None)" : redact_sensitive_info(cand.source_url)) << "\n"
                   << "      Thumbnail URL     : " << (cand.thumbnail_url.empty() ? "(None)" : redact_sensitive_info(cand.thumbnail_url)) << "\n"
                   << "      Target Image URL  : " << redact_sensitive_info(query_image_url) << "\n"
                   << "      Service Returned IDs: " << (cand.id_info.empty() ? "(None)" : cand.id_info) << "\n"
                   << "      Extra Details     : " << (cand.extra_info.empty() ? "(None)" : cand.extra_info) << "\n"
                   << "      Candidate Status  : " << (cand.accepted ? "ACCEPTED" : ("REJECTED - " + cand.rejection_reason)) << "\n";
            }
        } else {
            ss << "  Extracted Candidates: None\n";
        }

        ss << "  Raw Response Body:\n";
        std::istringstream body_stream(format_and_truncate_body(r.raw_response_body, 1000));
        std::string line;
        while (std::getline(body_stream, line)) {
            ss << "    " << line << "\n";
        }

        ss << "--------------------------------------------------------------------------------";
    }

    std::string final_reason;
    if (http_successful == 0) {
        final_reason = "Network request failed due to connection timeouts or unreachable endpoint.";
    } else if (valid_parsed_responses == 0) {
        final_reason = "SauceNAO API returned empty or unparseable response body.";
    } else if (accepted_matches == 0) {
        final_reason = "SauceNAO responded, but no candidate met the required similarity threshold (>= 50%) or contained a valid source link.";
    } else {
        final_reason = "No candidate accepted based on evaluation rules.";
    }

    ss << "\n================================================================================\n"
       << "                           DIAGNOSTIC SUMMARY LOG                              \n"
       << "================================================================================\n"
       << "  Total Engines Queried     : " << total_queried << "\n"
       << "  Successful HTTP Requests : " << http_successful << " / " << total_queried << "\n"
       << "  Valid Parsed Responses   : " << valid_parsed_responses << " / " << total_queried << "\n"
       << "  Accepted Matches         : " << accepted_matches << " / " << total_queried << "\n"
       << "  Final Reason             : " << final_reason << "\n"
       << "================================================================================\n";

    logger::warn("SauceDiagnostic", ss.str());
}

// --- Shared HttpClient Singleton ---

net::HttpClient& get_sauce_http_client() {
    static net::HttpClient instance;
    static std::once_flag init_flag;

    std::call_once(init_flag, []() {
        instance.set_default_user_agent(
            "SauceBot/0.1.0 (+https://github.com/Nezuko1909/tdcbot; kny1909.nezuko@gmail.com)"
        );

        net::DomainPolicy saucenao_policy;
        saucenao_policy.rate_limit_rps = 0.5;
        saucenao_policy.burst_capacity = 2;
        saucenao_policy.max_concurrent_requests = 1;
        saucenao_policy.max_retries = 2;
        saucenao_policy.cache_ttl = std::chrono::seconds(300);
        instance.set_domain_policy("saucenao.com", saucenao_policy);

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
               "Search image source using SauceNAO reverse search engine", 0)
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

// --- Exclusive Engine: SauceNAO (Official Anime & Artwork Search API) ---

static SearchResult search_saucenao(const std::string &image_url, EngineDiagnosticReport &diag) {
    SearchResult res;
    res.engine_name = "SauceNAO API";
    diag.engine_name = res.engine_name;

    std::string api_url = "https://saucenao.com/search.php?db=999&output_type=2&url=" + dpp::utility::url_encode(image_url);
    if (const char *env_key = std::getenv("SAUCENAO_API_KEY")) {
        if (std::strlen(env_key) > 0) {
            api_url += "&api_key=" + std::string(env_key);
        }
    }
    diag.request_url = api_url;

    net::HttpRequest req;
    req.method = "GET";
    req.url = api_url;
    req.timeout = std::chrono::milliseconds(10000);

    net::HttpResponse response = get_sauce_http_client().execute(req);

    diag.http_status = response.status_code;
    diag.response_time_ms = response.latency.count();
    diag.http_success = response.is_success();
    diag.error_message = response.error_message;
    diag.raw_response_body = response.body;

    if (!response.is_success() || response.body.empty()) {
        diag.engine_rejection_reason = "HTTP request failed (status " + std::to_string(response.status_code) + ") or empty body.";
        return res;
    }

    try {
        auto root = dpp::json::parse(response.body);
        if (root.contains("results") && root["results"].is_array() && !root["results"].empty()) {
            diag.parsed_result_count = root["results"].size();

            for (const auto &item : root["results"]) {
                CandidateDetail cand;
                if (item.contains("header")) {
                    const auto &header = item["header"];
                    if (header.contains("similarity")) {
                        try {
                            cand.similarity = std::stof(header["similarity"].get<std::string>());
                        } catch (...) {
                            if (header["similarity"].is_number()) {
                                cand.similarity = header["similarity"].get<float>();
                            }
                        }
                    }
                    cand.thumbnail_url = header.value("thumbnail", "");
                }

                if (item.contains("data")) {
                    const auto &data = item["data"];
                    if (data.contains("ext_urls") && data["ext_urls"].is_array() && !data["ext_urls"].empty()) {
                        cand.source_url = data["ext_urls"][0].get<std::string>();
                    } else if (data.contains("source") && !data["source"].is_null()) {
                        cand.source_url = data["source"].get<std::string>();
                    }

                    if (data.contains("title") && !data["title"].is_null()) {
                        cand.title = data["title"].get<std::string>();
                    } else if (data.contains("jp_name") && !data["jp_name"].is_null()) {
                        cand.title = data["jp_name"].get<std::string>();
                    } else if (data.contains("source") && !data["source"].is_null()) {
                        cand.title = data["source"].get<std::string>();
                    }

                    if (data.contains("member_name") && !data["member_name"].is_null()) {
                        res.author = data["member_name"].get<std::string>();
                    } else if (data.contains("author_name") && !data["author_name"].is_null()) {
                        res.author = data["author_name"].get<std::string>();
                    }
                }

                if (cand.similarity >= 50.0f && !cand.source_url.empty()) {
                    cand.accepted = true;
                    cand.rejection_reason = "None (Accepted)";
                } else if (cand.source_url.empty()) {
                    cand.accepted = false;
                    cand.rejection_reason = "Missing source URL in SauceNAO result data";
                } else {
                    cand.accepted = false;
                    cand.rejection_reason = "Similarity score (" + std::to_string(static_cast<int>(cand.similarity)) + "%) below threshold (50%)";
                }

                diag.extracted_candidates.push_back(cand);

                if (cand.accepted && (res.similarity < cand.similarity)) {
                    res.similarity = cand.similarity;
                    res.source_url = cand.source_url;
                    res.title = cand.title;
                    res.thumbnail_url = cand.thumbnail_url;
                }
            }
        } else {
            diag.engine_rejection_reason = "No matching results array found in SauceNAO response JSON.";
        }
    } catch (const std::exception &ex) {
        diag.engine_rejection_reason = std::string("JSON parsing error: ") + ex.what();
    }

    if (diag.parsed_result_count > 0 && res.source_url.empty()) {
        diag.engine_rejection_reason = "Candidates parsed, but none met requirements (>= 50% similarity with valid URL).";
    }

    return res;
}

// --- Search Handler using SauceNAO ---

static void perform_sauce_search(const std::string &image_url,
                                 std::function<void(dpp::message)> respond) {
    logger::info("commands::sauce", "Starting SauceNAO API search for image: " + image_url);

    std::thread([image_url, respond]() {
        EngineDiagnosticReport diag_sauce;
        SearchResult result = search_saucenao(image_url, diag_sauce);

        std::vector<EngineDiagnosticReport> diagnostic_reports = { diag_sauce };

        dpp::embed embed;
        embed.set_timestamp(time(nullptr));

        if (result.similarity <= 0.0f || result.source_url.empty()) {
            log_no_match_diagnostic(image_url, diagnostic_reports);

            embed.set_title("No Sauce Found")
                .set_description("Could not find matching image sources on SauceNAO.")
                .set_color(0xE74C3C)
                .set_image(image_url);
            respond(dpp::message().add_embed(embed));
            return;
        }

        uint32_t color =
            (result.similarity >= 80.0f)
                ? 0x2ECC71
                : ((result.similarity >= 50.0f) ? 0xF1C40F : 0xE74C3C);

        embed.set_title("SauceNAO Image Search Results")
            .set_color(color)
            .set_footer(dpp::embed_footer().set_text("Searched via SauceNAO API"));

        if (!result.thumbnail_url.empty()) {
            embed.set_thumbnail(result.thumbnail_url);
        } else {
            embed.set_thumbnail(image_url);
        }

        embed.add_field("Engine", result.engine_name, true);
        embed.add_field(
            "Similarity",
            std::to_string(static_cast<int>(result.similarity)) + "%",
            true);

        if (!result.title.empty()) {
            embed.add_field("Title",
                            "[" + result.title + "](" +
                                result.source_url + ")",
                            false);
        } else {
            embed.add_field("Title", "Matched Result", false);
        }

        if (!result.extra_info.empty()) {
            embed.add_field("Details", result.extra_info, true);
        }

        if (!result.author.empty()) {
            if (!result.author_url.empty()) {
                embed.add_field("Author",
                                "[" + result.author + "](" +
                                    result.author_url + ")",
                                false);
            } else {
                embed.add_field("Author", result.author, false);
            }
        }

        embed.add_field("Source Link",
                        "[Click here to view original post](" +
                            result.source_url + ")",
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
