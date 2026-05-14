#include "opennow/net/CurlHttpClient.hpp"

#if defined(OPENNOW_HAS_CURL)
#include <curl/curl.h>
#endif

#include <sstream>
#include <mutex>
#include <cstdio>
#include <fstream>

namespace opennow::net {

#if defined(OPENNOW_HAS_CURL)
namespace {

size_t writeBody(char* ptr, size_t size, size_t nmemb, void* userdata) {
    auto* body = static_cast<std::string*>(userdata);
    body->append(ptr, size * nmemb);
    return size * nmemb;
}

size_t writeHeader(char* ptr, size_t size, size_t nmemb, void* userdata) {
    auto* headers = static_cast<Headers*>(userdata);
    const std::string line(ptr, size * nmemb);
    const auto colon = line.find(':');
    if (colon == std::string::npos) {
        return size * nmemb;
    }
    auto key = line.substr(0, colon);
    auto value = line.substr(colon + 1);
    while (!value.empty() && (value.front() == ' ' || value.front() == '\t')) {
        value.erase(value.begin());
    }
    while (!value.empty() && (value.back() == '\r' || value.back() == '\n' || value.back() == ' ' || value.back() == '\t')) {
        value.pop_back();
    }
    (*headers)[key] = value;
    return size * nmemb;
}

const char* curlMethod(HttpMethod method) {
    switch (method) {
    case HttpMethod::Get: return "GET";
    case HttpMethod::Post: return "POST";
    case HttpMethod::Put: return "PUT";
    case HttpMethod::Delete: return "DELETE";
    }
    return "GET";
}

void ensureCurlGlobalInit() {
    static std::once_flag flag;
    std::call_once(flag, [] {
        curl_global_init(CURL_GLOBAL_DEFAULT);
    });
}

}
#endif

HttpResponse CurlHttpClient::send(const HttpRequest& request) {
#if defined(OPENNOW_HAS_CURL)
    ensureCurlGlobalInit();
#if defined(OPENNOW_PLATFORM_SWITCH)
    std::printf("[INFO][NETWORK] HTTP %s %s\n", curlMethod(request.method), request.url.c_str());
    std::fflush(stdout);
#endif
    CURL* curl = curl_easy_init();
    if (!curl) {
#if defined(OPENNOW_PLATFORM_SWITCH)
        std::printf("[ERROR][NETWORK] curl_easy_init failed\n");
        std::fflush(stdout);
#endif
        return {599, {}, "curl_easy_init failed"};
    }

    std::string body;
    Headers responseHeaders;
    curl_easy_setopt(curl, CURLOPT_URL, request.url.c_str());
    curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, curlMethod(request.method));
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, writeBody);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &body);
    curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, writeHeader);
    curl_easy_setopt(curl, CURLOPT_HEADERDATA, &responseHeaders);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "");
    curl_easy_setopt(curl, CURLOPT_ACCEPT_ENCODING, "");
#if defined(OPENNOW_PLATFORM_SWITCH)
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 8L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 20L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 1L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 2L);
#if defined(OPENNOW_SWITCH_CA_BUNDLE)
    {
        std::ifstream caBundle(OPENNOW_SWITCH_CA_BUNDLE, std::ios::binary);
        if (caBundle.good()) {
            curl_easy_setopt(curl, CURLOPT_CAINFO, OPENNOW_SWITCH_CA_BUNDLE);
        } else {
            std::printf("[WARN][NETWORK] CA bundle not available at %s; TLS verification may fail\n", OPENNOW_SWITCH_CA_BUNDLE);
            std::fflush(stdout);
        }
    }
#endif
#else
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 15L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 45L);
#endif

    struct curl_slist* headers = nullptr;
    for (const auto& [key, value] : request.headers) {
        const auto header = key + ": " + value;
        headers = curl_slist_append(headers, header.c_str());
    }
    if (headers) {
        curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    }

    if (!request.body.empty() || request.method == HttpMethod::Post || request.method == HttpMethod::Put) {
        curl_easy_setopt(curl, CURLOPT_POSTFIELDS, request.body.c_str());
        curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, static_cast<long>(request.body.size()));
    }

    const auto code = curl_easy_perform(curl);
    long status = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);

    if (headers) {
        curl_slist_free_all(headers);
    }
    curl_easy_cleanup(curl);

    if (code != CURLE_OK) {
#if defined(OPENNOW_PLATFORM_SWITCH)
        std::printf("[ERROR][NETWORK] HTTP failed status=%ld curl=%d error=%s\n", status, static_cast<int>(code), curl_easy_strerror(code));
        std::fflush(stdout);
#endif
        return {599, responseHeaders, curl_easy_strerror(code)};
    }
#if defined(OPENNOW_PLATFORM_SWITCH)
    std::printf("[INFO][NETWORK] HTTP completed status=%ld bytes=%zu\n", status, body.size());
    std::fflush(stdout);
#endif
    return {static_cast<int>(status), responseHeaders, body};
#else
    (void)request;
    return {599, {}, "CurlHttpClient was built without libcurl"};
#endif
}

} // namespace opennow::net
