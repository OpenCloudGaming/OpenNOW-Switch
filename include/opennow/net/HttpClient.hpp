#pragma once

#include <map>
#include <string>
#include <utility>
#include <vector>

namespace opennow::net {

using Headers = std::map<std::string, std::string>;

enum class HttpMethod {
    Get,
    Post,
    Put,
    Delete,
};

struct HttpRequest {
    HttpMethod method = HttpMethod::Get;
    std::string url;
    Headers headers;
    std::string body;
};

struct HttpResponse {
    int status = 0;
    Headers headers;
    std::string body;
};

class HttpClient {
public:
    virtual ~HttpClient() = default;
    virtual HttpResponse send(const HttpRequest& request) = 0;
};

class MockHttpClient final : public HttpClient {
public:
    void enqueue(HttpResponse response);
    HttpResponse send(const HttpRequest& request) override;

    const std::vector<HttpRequest>& requests() const;

private:
    std::vector<HttpResponse> responses_;
    std::vector<HttpRequest> requests_;
};

const char* toString(HttpMethod method);

} // namespace opennow::net
