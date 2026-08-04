#include "utils/http_client.h"
#include "utils/logger.h"
#include <iostream>
#include <sstream>
#include <iomanip>
#include <algorithm>
#include <random>
#include <thread>
#include <cctype>

namespace net {

namespace {

// Helper to lower-case strings
std::string to_lowercase(std::string str) {
    std::transform(str.begin(), str.end(), str.begin(), [](unsigned char c) {
        return std::tolower(c);
    });
    return str;
}

// Curl write callback for response body
size_t write_callback(void* contents, size_t size, size_t nmemb, void* userp) {
    size_t total_size = size * nmemb;
    auto* str = static_cast<std::string*>(userp);
    str->append(static_cast<char*>(contents), total_size);
    return total_size;
}

// Curl header callback for parsing HTTP response headers
size_t header_callback(char* buffer, size_t size, size_t nitems, void* userdata) {
    size_t total_size = size * nitems;
    auto* headers = static_cast<std::map<std::string, std::string>*>(userdata);
    
    std::string line(buffer, total_size);
    auto colon_pos = line.find(':');
    if (colon_pos != std::string::npos) {
        std::string key = line.substr(0, colon_pos);
        std::string value = line.substr(colon_pos + 1);

        // Trim whitespace and trailing \r\n
        key.erase(0, key.find_first_not_of(" \t"));
        key.erase(key.find_last_not_of(" \t\r\n") + 1);
        value.erase(0, value.find_first_not_of(" \t"));
        value.erase(value.find_last_not_of(" \t\r\n") + 1);

        (*headers)[to_lowercase(key)] = value;
    }
    return total_size;
}

} // anonymous namespace

// --- TokenBucketRateLimiter Implementation ---

TokenBucketRateLimiter::TokenBucketRateLimiter(double rps, size_t capacity)
    : rps_(rps), capacity_(capacity), tokens_(static_cast<double>(capacity)),
      last_refill_(std::chrono::steady_clock::now()) {}

void TokenBucketRateLimiter::configure(double rps, size_t capacity) {
    std::lock_guard<std::mutex> lock(mutex_);
    rps_ = rps;
    capacity_ = capacity;
    tokens_ = std::min(tokens_, static_cast<double>(capacity));
}

bool TokenBucketRateLimiter::acquire(std::chrono::milliseconds timeout) {
    auto start = std::chrono::steady_clock::now();
    while (true) {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            auto now = std::chrono::steady_clock::now();
            double elapsed = std::chrono::duration<double>(now - last_refill_).count();
            last_refill_ = now;

            tokens_ = std::min(static_cast<double>(capacity_), tokens_ + elapsed * rps_);

            if (tokens_ >= 1.0) {
                tokens_ -= 1.0;
                return true;
            }
        }

        if (std::chrono::steady_clock::now() - start >= timeout) {
            return false;
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
}

// --- ResponseCache Implementation ---

ResponseCache::ResponseCache(size_t max_entries) : max_entries_(max_entries) {}

std::string ResponseCache::make_cache_key(const HttpRequest& req) {
    std::string key = req.method + ":" + req.url;
    if (!req.body.empty()) {
        key += ":" + std::to_string(std::hash<std::string>{}(req.body));
    }
    return key;
}

bool ResponseCache::get(const std::string& key, HttpResponse& out_response) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = cache_.find(key);
    if (it == cache_.end()) {
        return false;
    }

    if (std::chrono::steady_clock::now() > it->second.expiry) {
        cache_.erase(it);
        return false;
    }

    out_response = it->second.response;
    out_response.cached = true;
    return true;
}

void ResponseCache::put(const std::string& key, const HttpResponse& response, std::chrono::seconds ttl) {
    if (ttl.count() <= 0) return;

    std::lock_guard<std::mutex> lock(mutex_);
    if (cache_.size() >= max_entries_) {
        // Evict oldest item
        cache_.erase(cache_.begin());
    }

    CacheItem item;
    item.response = response;
    item.response.cached = false; // Store original
    item.expiry = std::chrono::steady_clock::now() + ttl;
    cache_[key] = item;
}

void ResponseCache::clear() {
    std::lock_guard<std::mutex> lock(mutex_);
    cache_.clear();
}

// --- HttpClient Implementation ---

HttpClient::HttpClient() {
    curl_global_init(CURL_GLOBAL_ALL);
    default_user_agent_ = "AntigravitySauceBot/1.0 (+https://github.com/Nezuko1909/tdcbot; contact@example.com)";
}

HttpClient::~HttpClient() {
    curl_global_cleanup();
}

void HttpClient::set_default_user_agent(const std::string& user_agent) {
    default_user_agent_ = user_agent;
}

void HttpClient::set_domain_policy(const std::string& host, const DomainPolicy& policy) {
    std::lock_guard<std::mutex> lock(policies_mutex_);
    domain_policies_[host] = policy;

    std::lock_guard<std::mutex> r_lock(rate_limiters_mutex_);
    auto it = rate_limiters_.find(host);
    if (it != rate_limiters_.end()) {
        it->second->configure(policy.rate_limit_rps, policy.burst_capacity);
    }
}

DomainPolicy HttpClient::get_domain_policy(const std::string& host) const {
    std::lock_guard<std::mutex> lock(policies_mutex_);
    auto it = domain_policies_.find(host);
    if (it != domain_policies_.end()) {
        return it->second;
    }
    return default_policy_;
}

std::shared_ptr<TokenBucketRateLimiter> HttpClient::get_rate_limiter(const std::string& host, const DomainPolicy& policy) {
    std::lock_guard<std::mutex> lock(rate_limiters_mutex_);
    auto it = rate_limiters_.find(host);
    if (it != rate_limiters_.end()) {
        return it->second;
    }
    auto limiter = std::make_shared<TokenBucketRateLimiter>(policy.rate_limit_rps, policy.burst_capacity);
    rate_limiters_[host] = limiter;
    return limiter;
}

std::string HttpClient::extract_host(const std::string& url) const {
    std::string host = url;
    auto pos = host.find("://");
    if (pos != std::string::npos) {
        host = host.substr(pos + 3);
    }
    pos = host.find('/');
    if (pos != std::string::npos) {
        host = host.substr(0, pos);
    }
    pos = host.find(':');
    if (pos != std::string::npos) {
        host = host.substr(0, pos);
    }
    return to_lowercase(host);
}

std::chrono::milliseconds HttpClient::parse_retry_after(const std::string& header_val) const {
    if (header_val.empty()) return std::chrono::milliseconds(0);

    // Try integer seconds first
    try {
        size_t idx = 0;
        long seconds = std::stol(header_val, &idx);
        if (idx == header_val.length() && seconds > 0) {
            return std::chrono::milliseconds(seconds * 1000);
        }
    } catch (...) {}

    return std::chrono::milliseconds(0);
}

std::chrono::milliseconds HttpClient::compute_jitter_backoff(int attempt, std::chrono::milliseconds base, std::chrono::milliseconds max_val) const {
    double exp_backoff = static_cast<double>(base.count()) * std::pow(2.0, attempt);
    double capped_backoff = std::min(static_cast<double>(max_val.count()), exp_backoff);

    // Full jitter: uniform random in [0, capped_backoff]
    static thread_local std::mt19937 generator(std::random_device{}());
    std::uniform_real_distribution<double> distribution(0.0, capped_backoff);
    
    return std::chrono::milliseconds(static_cast<int64_t>(distribution(generator)));
}

void HttpClient::set_log_callback(LogCallback callback) {
    log_callback_ = callback;
}

HttpResponse HttpClient::execute(const HttpRequest& request) {
    if (request.method == "GET" && request.allow_cache) {
        std::string cache_key = ResponseCache::make_cache_key(request);
        HttpResponse cached_resp;
        if (response_cache_.get(cache_key, cached_resp)) {
            if (log_callback_) {
                RequestLogEvent ev;
                ev.timestamp = "NOW";
                ev.host = extract_host(request.url);
                ev.url = request.url;
                ev.method = request.method;
                ev.status_code = cached_resp.status_code;
                ev.latency_ms = 0;
                ev.retry_count = 0;
                ev.rate_limit_wait_ms = 0;
                ev.cache_hit = true;
                log_callback_(ev);
            }
            return cached_resp;
        }
    }

    return execute_internal(request);
}

std::future<HttpResponse> HttpClient::execute_async(const HttpRequest& request) {
    return std::async(std::launch::async, [this, request]() {
        return this->execute(request);
    });
}

HttpResponse HttpClient::execute_internal(const HttpRequest& request) {
    std::string host = extract_host(request.url);
    DomainPolicy policy = get_domain_policy(host);
    auto rate_limiter = get_rate_limiter(host, policy);

    HttpResponse response;
    int attempt = 0;
    int64_t total_rate_limit_wait_ms = 0;

    auto start_time = std::chrono::steady_clock::now();

    while (attempt <= policy.max_retries) {
        // Concurrency Control
        {
            std::unique_lock<std::mutex> lock(concurrency_mutex_);
            while (active_host_requests_[host] >= policy.max_concurrent_requests) {
                lock.unlock();
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
                lock.lock();
            }
            active_host_requests_[host]++;
        }

        // Rate Limiter Acquire
        auto rl_start = std::chrono::steady_clock::now();
        bool acquired = rate_limiter->acquire(request.timeout);
        auto rl_wait = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - rl_start).count();
        total_rate_limit_wait_ms += rl_wait;

        if (!acquired) {
            {
                std::lock_guard<std::mutex> lock(concurrency_mutex_);
                active_host_requests_[host]--;
            }
            response.status_code = 429;
            response.error_message = "Rate limit acquire timeout";
            break;
        }

        // Initialize LibCURL Easy Handle
        CURL* curl = curl_easy_init();
        if (!curl) {
            {
                std::lock_guard<std::mutex> lock(concurrency_mutex_);
                active_host_requests_[host]--;
            }
            response.status_code = 500;
            response.error_message = "Failed to initialize libcurl handle";
            break;
        }

        std::string response_body;
        std::map<std::string, std::string> response_headers;
        struct curl_slist* chunk = nullptr;

        // User Agent
        std::string ua = policy.custom_user_agent.empty() ? default_user_agent_ : policy.custom_user_agent;
        curl_easy_setopt(curl, CURLOPT_USERAGENT, ua.c_str());

        // Target URL
        curl_easy_setopt(curl, CURLOPT_URL, request.url.c_str());

        // HTTP Method & Body
        if (request.method == "POST") {
            curl_easy_setopt(curl, CURLOPT_POST, 1L);
            curl_easy_setopt(curl, CURLOPT_POSTFIELDS, request.body.c_str());
            curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, static_cast<long>(request.body.size()));
        } else if (request.method != "GET") {
            curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, request.method.c_str());
            if (!request.body.empty()) {
                curl_easy_setopt(curl, CURLOPT_POSTFIELDS, request.body.c_str());
            }
        }

        // Headers
        for (const auto& [k, v] : request.headers) {
            std::string h = k + ": " + v;
            chunk = curl_slist_append(chunk, h.c_str());
        }
        if (chunk) {
            curl_easy_setopt(curl, CURLOPT_HTTPHEADER, chunk);
        }

        // Callbacks
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_callback);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response_body);
        curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, header_callback);
        curl_easy_setopt(curl, CURLOPT_HEADERDATA, &response_headers);

        // Protocols & Options
        curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, static_cast<long>(request.timeout.count()));
        curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
        curl_easy_setopt(curl, CURLOPT_MAXREDIRS, 5L);
        curl_easy_setopt(curl, CURLOPT_ACCEPT_ENCODING, "gzip, deflate, br");
        curl_easy_setopt(curl, CURLOPT_HTTP_VERSION, CURL_HTTP_VERSION_2TLS);
        curl_easy_setopt(curl, CURLOPT_TCP_KEEPALIVE, 1L);
        curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 1L);
        curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 2L);

        // Perform HTTP Request
        CURLcode res = curl_easy_perform(curl);

        long http_code = 0;
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);

        if (chunk) {
            curl_slist_free_all(chunk);
        }
        curl_easy_cleanup(curl);

        // Decrement concurrency
        {
            std::lock_guard<std::mutex> lock(concurrency_mutex_);
            active_host_requests_[host]--;
        }

        response.status_code = static_cast<int>(http_code);
        response.body = response_body;
        response.headers = response_headers;
        response.retry_count = attempt;

        if (res != CURLE_OK) {
            response.error_message = curl_easy_strerror(res);
        }

        // Check if retry is needed (429 or 503 or network failure)
        bool is_transient_error = (res != CURLE_OK) || http_code == 429 || http_code == 503;

        if (is_transient_error && attempt < policy.max_retries) {
            attempt++;
            std::chrono::milliseconds backoff_delay(0);

            if (policy.respect_retry_after) {
                auto it = response_headers.find("retry-after");
                if (it != response_headers.end()) {
                    backoff_delay = parse_retry_after(it->second);
                }
            }

            if (backoff_delay.count() <= 0) {
                backoff_delay = compute_jitter_backoff(attempt, policy.base_backoff, policy.max_backoff);
            }

            logger::warn("HttpClient", "Host " + host + " returned status " + std::to_string(http_code) +
                                      " (attempt " + std::to_string(attempt) + "/" + std::to_string(policy.max_retries) +
                                      "). Backing off for " + std::to_string(backoff_delay.count()) + "ms.");
            
            std::this_thread::sleep_for(backoff_delay);
            continue;
        }

        break;
    }

    auto end_time = std::chrono::steady_clock::now();
    response.latency = std::chrono::duration_cast<std::chrono::milliseconds>(end_time - start_time);

    // Cache successful GET requests
    if (request.method == "GET" && request.allow_cache && response.is_success()) {
        std::string cache_key = ResponseCache::make_cache_key(request);
        response_cache_.put(cache_key, response, policy.cache_ttl);
    }

    // Structured Log Output
    if (log_callback_) {
        RequestLogEvent ev;
        ev.timestamp = "NOW";
        ev.host = host;
        ev.url = request.url;
        ev.method = request.method;
        ev.status_code = response.status_code;
        ev.latency_ms = response.latency.count();
        ev.retry_count = response.retry_count;
        ev.rate_limit_wait_ms = total_rate_limit_wait_ms;
        ev.cache_hit = false;
        ev.error = response.error_message;
        log_callback_(ev);
    }

    return response;
}

} // namespace net
