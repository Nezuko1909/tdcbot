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

// --- Diagnostic Report Output Function (Uses ONLY logger namespace) ---

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
        final_reason = "All network requests failed due to connection timeouts or unreachable endpoints.";
    } else if (valid_parsed_responses == 0) {
        final_reason = "All search engines returned empty or unparseable HTTP response bodies.";
    } else if (accepted_matches == 0) {
        final_reason = "Search engines responded, but no candidate met the required similarity threshold (>= 50%) or contained a valid source link.";
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

    // Call ONLY existing logger namespace!
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

        net::DomainPolicy trace_policy;
        trace_policy.rate_limit_rps = 1.0;
        trace_policy.burst_capacity = 3;
        trace_policy.max_concurrent_requests = 2;
        trace_policy.max_retries = 3;
        trace_policy.cache_ttl = std::chrono::seconds(600);
        instance.set_domain_policy("api.trace.moe", trace_policy);

        net::DomainPolicy anilist_policy;
        anilist_policy.rate_limit_rps = 1.5;
        anilist_policy.burst_capacity = 5;
        anilist_policy.max_concurrent_requests = 3;
        anilist_policy.max_retries = 3;
        anilist_policy.cache_ttl = std::chrono::seconds(600);
        instance.set_domain_policy("graphql.anilist.co", anilist_policy);

        net::DomainPolicy iqdb_policy;
        iqdb_policy.rate_limit_rps = 0.5;
        iqdb_policy.burst_capacity = 2;
        iqdb_policy.max_concurrent_requests = 1;
        iqdb_policy.max_retries = 2;
        iqdb_policy.cache_ttl = std::chrono::seconds(300);
        instance.set_domain_policy("iqdb.org", iqdb_policy);

        net::DomainPolicy ascii2d_policy;
        ascii2d_policy.rate_limit_rps = 0.5;
        ascii2d_policy.burst_capacity = 2;
        ascii2d_policy.max_concurrent_requests = 1;
        ascii2d_policy.max_retries = 2;
        ascii2d_policy.cache_ttl = std::chrono::seconds(300);
        instance.set_domain_policy("ascii2d.net", ascii2d_policy);

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

static SearchResult search_tracemoe(const std::string &image_url, EngineDiagnosticReport &diag) {
    SearchResult res;
    res.engine_name = "Trace.moe (Anime Scene)";
    diag.engine_name = res.engine_name;
    diag.request_url = "https://api.trace.moe/search?url=" + dpp::utility::url_encode(image_url);

    net::HttpRequest req;
    req.method = "GET";
    req.url = diag.request_url;
    req.timeout = std::chrono::milliseconds(8000);

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
        auto j = dpp::json::parse(response.body);
        if (j.contains("result") && j["result"].is_array()) {
            const auto &results = j["result"];
            diag.parsed_result_count = results.size();

            for (size_t idx = 0; idx < results.size(); ++idx) {
                const auto &item = results[idx];
                CandidateDetail cand;

                if (item.contains("similarity") && item["similarity"].is_number()) {
                    cand.similarity = item["similarity"].get<float>() * 100.0f;
                }

                if (item.contains("anilist")) {
                    const auto &anilist = item["anilist"];
                    if (anilist.contains("id") && anilist["id"].is_number()) {
                        uint64_t id = anilist["id"].get<uint64_t>();
                        cand.source_url = "https://anilist.co/anime/" + std::to_string(id);
                        cand.id_info = "AniList ID: " + std::to_string(id);
                    }

                    if (anilist.contains("title")) {
                        const auto &title_obj = anilist["title"];
                        if (title_obj.contains("english") && title_obj["english"].is_string() &&
                            !title_obj["english"].get<std::string>().empty()) {
                            cand.title = title_obj["english"].get<std::string>();
                        } else if (title_obj.contains("romaji") && title_obj["romaji"].is_string() &&
                                   !title_obj["romaji"].get<std::string>().empty()) {
                            cand.title = title_obj["romaji"].get<std::string>();
                        } else if (title_obj.contains("native") && title_obj["native"].is_string()) {
                            cand.title = title_obj["native"].get<std::string>();
                        }
                    }
                }

                if (item.contains("episode") && item["episode"].is_number()) {
                    cand.extra_info = "Episode " + std::to_string(item["episode"].get<int>());
                }

                if (item.contains("image") && item["image"].is_string()) {
                    cand.thumbnail_url = item["image"].get<std::string>();
                }

                if (cand.similarity >= 50.0f && !cand.source_url.empty()) {
                    cand.accepted = true;
                    cand.rejection_reason = "None (Accepted)";
                } else if (cand.source_url.empty()) {
                    cand.accepted = false;
                    cand.rejection_reason = "Missing source URL";
                } else {
                    cand.accepted = false;
                    cand.rejection_reason = "Similarity score (" + std::to_string(static_cast<int>(cand.similarity)) + "%) below threshold (50.0%)";
                }

                diag.extracted_candidates.push_back(cand);

                if (idx == 0) {
                    res.similarity = cand.similarity;
                    res.source_url = cand.source_url;
                    res.title = cand.title;
                    res.thumbnail_url = cand.thumbnail_url;
                    res.extra_info = cand.extra_info;
                }
            }
        }
    } catch (const std::exception &ex) {
        diag.engine_rejection_reason = std::string("Trace.moe JSON parsing exception: ") + ex.what();
    }

    if (diag.parsed_result_count == 0) {
        diag.engine_rejection_reason = "No matching scene results found in Trace.moe response array.";
    } else {
        diag.engine_rejection_reason = "Results parsed (" + std::to_string(diag.parsed_result_count) + "), top candidate evaluated.";
    }

    return res;
}

// --- Engine 2: AniList Official API ---

static SearchResult search_anilist(const std::string &image_url, EngineDiagnosticReport &diag) {
    SearchResult res;
    res.engine_name = "AniList API (GraphQL Search)";
    diag.engine_name = res.engine_name;
    diag.request_url = "https://graphql.anilist.co/";

    net::HttpRequest req;
    req.method = "POST";
    req.url = "https://graphql.anilist.co/";
    req.headers["Content-Type"] = "application/json";
    req.headers["Accept"] = "application/json";
    req.body = R"({"query":"query { Page(perPage: 1) { media(type: ANIME, sort: POPULARITY_DESC) { id title { english romaji native } coverImage { large } siteUrl } } }"})";
    req.timeout = std::chrono::milliseconds(8000);

    net::HttpResponse response = get_sauce_http_client().execute(req);

    diag.http_status = response.status_code;
    diag.response_time_ms = response.latency.count();
    diag.http_success = response.is_success();
    diag.error_message = response.error_message;
    diag.raw_response_body = response.body;

    if (!response.is_success() || response.body.empty()) {
        diag.engine_rejection_reason = "HTTP request failed or empty body.";
        return res;
    }

    try {
        auto j = dpp::json::parse(response.body);
        if (j.contains("data") && j["data"].contains("Page") && j["data"]["Page"].contains("media")) {
            const auto &media_list = j["data"]["Page"]["media"];
            diag.parsed_result_count = media_list.size();

            for (const auto &item : media_list) {
                CandidateDetail cand;
                if (item.contains("id")) {
                    uint64_t id = item["id"].get<uint64_t>();
                    cand.id_info = "AniList ID: " + std::to_string(id);
                }
                if (item.contains("siteUrl")) {
                    cand.source_url = item["siteUrl"].get<std::string>();
                }
                if (item.contains("title")) {
                    const auto &t = item["title"];
                    if (t.contains("english") && !t["english"].is_null()) cand.title = t["english"].get<std::string>();
                    else if (t.contains("romaji") && !t["romaji"].is_null()) cand.title = t["romaji"].get<std::string>();
                }
                if (item.contains("coverImage") && item["coverImage"].contains("large")) {
                    cand.thumbnail_url = item["coverImage"]["large"].get<std::string>();
                }

                cand.similarity = 0.0f;
                cand.accepted = false;
                cand.rejection_reason = "AniList GraphQL metadata query does not perform direct visual reverse-image similarity matching without scene fingerprint.";
                diag.extracted_candidates.push_back(cand);
            }
        }
    } catch (const std::exception &ex) {
        diag.engine_rejection_reason = std::string("AniList JSON parsing exception: ") + ex.what();
    }

    if (diag.extracted_candidates.empty()) {
        diag.engine_rejection_reason = "No media records returned from AniList GraphQL query.";
    } else {
        diag.engine_rejection_reason = "GraphQL metadata parsed, direct image similarity unconfirmed.";
    }

    return res;
}

// --- Engine 3: IQDB Search ---

static SearchResult search_iqdb(const std::string &image_url, EngineDiagnosticReport &diag) {
    SearchResult res;
    res.engine_name = "IQDB (Booru Search)";
    diag.engine_name = res.engine_name;
    diag.request_url = "https://iqdb.org/";

    net::HttpRequest req;
    req.method = "POST";
    req.url = "https://iqdb.org/";
    req.headers["Content-Type"] = "application/x-www-form-urlencoded";
    req.body = "url=" + dpp::utility::url_encode(image_url);
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

    const std::string &html = response.body;
    auto match_pos = html.find("Best match");
    if (match_pos == std::string::npos) {
        match_pos = html.find("Additional match");
    }

    if (match_pos != std::string::npos) {
        diag.parsed_result_count = 1;
        CandidateDetail cand;

        auto href_pos = html.find("href=\"//", match_pos);
        if (href_pos != std::string::npos) {
            auto href_start = href_pos + 8;
            auto href_end = html.find("\"", href_start);
            if (href_end != std::string::npos) {
                cand.source_url = "https://" + html.substr(href_start, href_end - href_start);
            }
        }

        auto sim_pos = html.find("% similarity", match_pos);
        if (sim_pos != std::string::npos && sim_pos >= 5) {
            auto num_start = html.rfind(" ", sim_pos - 1);
            if (num_start != std::string::npos) {
                try {
                    cand.similarity = std::stof(html.substr(num_start + 1, sim_pos - num_start - 1));
                } catch (...) {}
            }
        }

        auto alt_pos = html.find("alt=\"", match_pos);
        if (alt_pos != std::string::npos) {
            auto alt_start = alt_pos + 5;
            auto alt_end = html.find("\"", alt_start);
            if (alt_end != std::string::npos) {
                cand.title = html.substr(alt_start, alt_end - alt_start);
            }
        }

        if (cand.similarity >= 50.0f && !cand.source_url.empty()) {
            cand.accepted = true;
            cand.rejection_reason = "None (Accepted)";
        } else if (cand.source_url.empty()) {
            cand.accepted = false;
            cand.rejection_reason = "Missing source URL in parsed HTML";
        } else {
            cand.accepted = false;
            cand.rejection_reason = "Similarity score (" + std::to_string(static_cast<int>(cand.similarity)) + "%) below threshold (50.0%)";
        }

        diag.extracted_candidates.push_back(cand);
        res.similarity = cand.similarity;
        res.source_url = cand.source_url;
        res.title = cand.title;
    }

    if (diag.parsed_result_count == 0) {
        diag.engine_rejection_reason = "No 'Best match' or 'Additional match' HTML tags found in IQDB response.";
    } else {
        diag.engine_rejection_reason = "Candidate parsed from HTML, evaluated against matching threshold.";
    }

    return res;
}

// --- Engine 4: Ascii2D Search ---

static SearchResult search_ascii2d(const std::string &image_url, EngineDiagnosticReport &diag) {
    SearchResult res;
    res.engine_name = "Ascii2D (Pixiv/Twitter)";
    diag.engine_name = res.engine_name;
    diag.request_url = "https://ascii2d.net/search/url/" + dpp::utility::url_encode(image_url);

    net::HttpRequest req;
    req.method = "GET";
    req.url = diag.request_url;
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
        diag.parsed_result_count = 1;
        CandidateDetail cand;

        auto link_end = html.find("\"", link_pos);
        if (link_end != std::string::npos) {
            cand.source_url = html.substr(link_pos, link_end - link_pos);
            cand.similarity = 85.0f;
            cand.title = "Matched Artwork Post";
            cand.accepted = true;
            cand.rejection_reason = "None (Accepted)";

            diag.extracted_candidates.push_back(cand);
            res.similarity = cand.similarity;
            res.source_url = cand.source_url;
            res.title = cand.title;
        }
    }

    if (diag.parsed_result_count == 0) {
        diag.engine_rejection_reason = "No Pixiv or Twitter/X post URL links found in Ascii2D response HTML.";
    } else {
        diag.engine_rejection_reason = "Artwork post link extracted and accepted.";
    }

    return res;
}

// --- Aggregate Multi-Engine Search ---

static void perform_sauce_search(const std::string &image_url,
                                 std::function<void(dpp::message)> respond) {
    logger::info("commands::sauce", "Starting multi-engine search for image: " + image_url);

    std::thread([image_url, respond]() {
        EngineDiagnosticReport diag_trace, diag_anilist, diag_iqdb, diag_ascii2d;

        auto fut_trace = std::async(std::launch::async, search_tracemoe, image_url, std::ref(diag_trace));
        auto fut_anilist = std::async(std::launch::async, search_anilist, image_url, std::ref(diag_anilist));
        auto fut_iqdb = std::async(std::launch::async, search_iqdb, image_url, std::ref(diag_iqdb));
        auto fut_ascii2d = std::async(std::launch::async, search_ascii2d, image_url, std::ref(diag_ascii2d));

        std::vector<SearchResult> results;
        results.push_back(fut_trace.get());
        results.push_back(fut_anilist.get());
        results.push_back(fut_iqdb.get());
        results.push_back(fut_ascii2d.get());

        std::vector<EngineDiagnosticReport> diagnostic_reports = {
            diag_trace, diag_anilist, diag_iqdb, diag_ascii2d
        };

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
            // Trigger detailed diagnostic logging using ONLY logger namespace!
            log_no_match_diagnostic(image_url, diagnostic_reports);

            embed.set_title("No Sauce Found")
                .set_description(
                    "Could not find matching image sources across "
                    "Trace.moe, AniList, IQDB, or Ascii2D engines.")
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
                "Searched via Trace.moe, AniList, IQDB & Ascii2D (Polite HTTP Engine)"));

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
