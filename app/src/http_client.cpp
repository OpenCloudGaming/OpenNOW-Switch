#include "http_client.hpp"

#include <curl/curl.h>

#include <memory>
#include <stdexcept>
#include <string>

namespace opennow
{
namespace
{

size_t WriteBody(char* ptr, size_t size, size_t nmemb, void* userdata)
{
    const size_t bytes = size * nmemb;
    auto* body         = static_cast<std::string*>(userdata);
    body->append(ptr, bytes);
    return bytes;
}

} // namespace

HttpResponse HttpClient::Request(
    const std::string& method,
    const std::string& url,
    const std::string& user_agent,
    const std::vector<std::string>& headers,
    const std::string& body,
    const std::string& proxy_url) const
{
    CURL* raw = curl_easy_init();
    if (!raw)
        throw std::runtime_error("curl_easy_init failed");

    std::unique_ptr<CURL, decltype(&curl_easy_cleanup)> curl(raw, &curl_easy_cleanup);
    std::string response_body;

    curl_slist* raw_headers = nullptr;
    for (const auto& header : headers)
        raw_headers = curl_slist_append(raw_headers, header.c_str());

    std::unique_ptr<curl_slist, decltype(&curl_slist_free_all)> header_list(
        raw_headers,
        &curl_slist_free_all);

    curl_easy_setopt(curl.get(), CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl.get(), CURLOPT_USERAGENT, user_agent.c_str());
    curl_easy_setopt(curl.get(), CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl.get(), CURLOPT_CONNECTTIMEOUT, 10L);
    curl_easy_setopt(curl.get(), CURLOPT_TIMEOUT, 30L);
    curl_easy_setopt(curl.get(), CURLOPT_WRITEFUNCTION, WriteBody);
    curl_easy_setopt(curl.get(), CURLOPT_WRITEDATA, &response_body);
    curl_easy_setopt(curl.get(), CURLOPT_ACCEPT_ENCODING, "");
    curl_easy_setopt(curl.get(), CURLOPT_SSL_VERIFYPEER, 1L);
    curl_easy_setopt(curl.get(), CURLOPT_SSL_VERIFYHOST, 2L);

    if (!proxy_url.empty())
    {
        curl_easy_setopt(curl.get(), CURLOPT_PROXY, proxy_url.c_str());
        curl_easy_setopt(curl.get(), CURLOPT_PROXYAUTH, CURLAUTH_ANY);
    }

    if (header_list)
        curl_easy_setopt(curl.get(), CURLOPT_HTTPHEADER, header_list.get());

    if (method == "POST")
    {
        curl_easy_setopt(curl.get(), CURLOPT_POST, 1L);
        curl_easy_setopt(curl.get(), CURLOPT_POSTFIELDS, body.c_str());
        curl_easy_setopt(curl.get(), CURLOPT_POSTFIELDSIZE, static_cast<long>(body.size()));
    }
    else if (method != "GET")
    {
        curl_easy_setopt(curl.get(), CURLOPT_CUSTOMREQUEST, method.c_str());
        if (!body.empty())
        {
            curl_easy_setopt(curl.get(), CURLOPT_POSTFIELDS, body.c_str());
            curl_easy_setopt(curl.get(), CURLOPT_POSTFIELDSIZE, static_cast<long>(body.size()));
        }
    }

    const CURLcode result = curl_easy_perform(curl.get());
    if (result != CURLE_OK)
    {
        throw std::runtime_error(
            "HTTP " + method + " failed for " + url + ": " + curl_easy_strerror(result));
    }

    long status_code = 0;
    curl_easy_getinfo(curl.get(), CURLINFO_RESPONSE_CODE, &status_code);

    return HttpResponse{
        .status_code = status_code,
        .body        = std::move(response_body),
    };
}

HttpResponse HttpClient::Get(
    const std::string& url,
    const std::string& user_agent,
    const std::vector<std::string>& headers,
    const std::string& proxy_url) const
{
    return Request("GET", url, user_agent, headers, {}, proxy_url);
}

HttpResponse HttpClient::Post(
    const std::string& url,
    const std::string& user_agent,
    const std::vector<std::string>& headers,
    const std::string& body,
    const std::string& proxy_url) const
{
    return Request("POST", url, user_agent, headers, body, proxy_url);
}

} // namespace opennow
