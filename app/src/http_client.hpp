#pragma once

#include <string>
#include <vector>

namespace opennow
{

struct HttpResponse
{
    long status_code = 0;
    std::string body;
};

class HttpClient
{
  public:
    HttpResponse Request(
        const std::string& method,
        const std::string& url,
        const std::string& user_agent,
        const std::vector<std::string>& headers = {},
        const std::string& body = {}) const;

    HttpResponse Get(
        const std::string& url,
        const std::string& user_agent,
        const std::vector<std::string>& headers = {}) const;

    HttpResponse Post(
        const std::string& url,
        const std::string& user_agent,
        const std::vector<std::string>& headers,
        const std::string& body) const;
};

} // namespace opennow
