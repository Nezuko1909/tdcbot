#pragma once

#include <string>
#include <vector>
#include <map>
#include <unordered_map>
#include <memory>
#include <mutex>
#include <chrono>
#include <future>
#include <functional>
#include <atomic>
#include <curl/curl.h>

namespace net {

// --- HTTP Request and Response Structures ---

struct HttpRequest {
    std::string method = "GET";
    std::string url;
    std::map<std::string, std::string> headers;
    std::string body;
    std::chrono::milliseconds timeout{10000};
    bool allow_cache = true;
};

struct HttpResponse {
    int status_code = 0;
    std::string body;
    std::map<std::string, std::string> headers;
    std::chrono::milliseconds latency{0};
    int retry_count = 0;
    bool cached = false;
    std::string error_message;

    bool is_success() const {
        return status_code >= 200 && status_code < 300;
    }
};

// --- Per-Domain Policy Configuration ---

struct DomainPolicy {
    double rate_limit_rps = 2.0;            // Max requests per second
    size_t burst_capacity = 5;              // Token bucket capacity
    size_t max_concurrent_requests = 4;     // Max concurrent TCP connections per host
    int max_retries = 3;                    // Maximum retry attempts
    std::chrono::milliseconds base_backoff{500};  // Exponential backoff base
    std::chrono::milliseconds max_backoff{8000};  // Exponential backoff ceiling
    std::chrono::seconds cache_ttl{300};    // Response cache TTL (5 minutes)
    std::string custom_user_agent;          // Domain-specific User-Agent override
    bool respect_retry_after = true;        // Respect Retry-After headers
};

// --- Token Bucket Rate Limiter ---

class TokenBucketRateLimiter {
public:
    TokenBucketRateLimiter(double rps, size_t capacity);

    // Blocks until a token is available or returns false if timed out
    bool acquire(std::chrono::milliseconds timeout = std::chrono::milliseconds(10000));

    void configure(double rps, size_t capacity);

private:
    std::mutex mutex_;
    double rps_;
    size_t capacity_;
    double tokens_;
    std::chrono::steady_clock::time_point last_refill_;
};

// --- Thread-Safe Response Cache & Single-Flight Deduplication ---

class ResponseCache {
public:
    explicit ResponseCache(size_t max_entries = 1000);

    bool get(const std::string& key, HttpResponse& out_response);
    void put(const std::string& key, const HttpResponse& response, std::chrono::seconds ttl);
    void clear();

    static std::string make_cache_key(const HttpRequest& req);

private:
    struct CacheItem {
        HttpResponse response;
        std::chrono::steady_clock::time_point expiry;
    };

    std::mutex mutex_;
    size_t max_entries_;
    std::unordered_map<std::string, CacheItem> cache_;
};

// --- Structured Request Log Event ---

struct RequestLogEvent {
    std::string timestamp;
    std::string host;
    std::string url;
    std::string method;
    int status_code;
    int64_t latency_ms;
    int retry_count;
    int64_t rate_limit_wait_ms;
    bool cache_hit;
    std::string error;
};

using LogCallback = std::function<void(const RequestLogEvent&)>;

// --- Production-Grade Libcurl Thread-Safe HTTP Client ---

class HttpClient {
public:
    HttpClient();
    ~HttpClient();

    // Disable copy
    HttpClient(const HttpClient&) = delete;
    HttpClient& operator=(const HttpClient&) = delete;

    // Set global user agent format
    void set_default_user_agent(const std::string& user_agent);
    
    // Set domain policy
    void set_domain_policy(const std::string& host, const DomainPolicy& policy);
    DomainPolicy get_domain_policy(const std::string& host) const;

    // Synchronous execution
    HttpResponse execute(const HttpRequest& request);

    // Asynchronous execution
    std::future<HttpResponse> execute_async(const HttpRequest& request);

    // Custom logger hook
    void set_log_callback(LogCallback callback);

private:
    HttpResponse execute_internal(const HttpRequest& request);

    // Helper functions
    std::string extract_host(const std::string& url) const;
    std::chrono::milliseconds parse_retry_after(const std::string& header_val) const;
    std::chrono::milliseconds compute_jitter_backoff(int attempt, std::chrono::milliseconds base, std::chrono::milliseconds max_val) const;
    
    // Member data
    std::string default_user_agent_;
    DomainPolicy default_policy_;

    mutable std::mutex policies_mutex_;
    std::unordered_map<std::string, DomainPolicy> domain_policies_;

    mutable std::mutex rate_limiters_mutex_;
    std::unordered_map<std::string, std::shared_ptr<TokenBucketRateLimiter>> rate_limiters_;

    mutable std::mutex concurrency_mutex_;
    std::unordered_map<std::string, size_t> active_host_requests_;

    ResponseCache response_cache_;
    LogCallback log_callback_;

    // Single-flight in-flight request map for deduplication
    std::mutex singleflight_mutex_;
    std::unordered_map<std::string, std::shared_future<HttpResponse>> in_flight_requests_;

    std::shared_ptr<TokenBucketRateLimiter> get_rate_limiter(const std::string& host, const DomainPolicy& policy);
};

} // namespace net
