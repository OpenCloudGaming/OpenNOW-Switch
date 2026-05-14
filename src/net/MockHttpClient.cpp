#include "opennow/net/HttpClient.hpp"

#include <utility>

namespace opennow::net {

void MockHttpClient::enqueue(HttpResponse response) {
    responses_.push_back(std::move(response));
}

HttpResponse MockHttpClient::send(const HttpRequest& request) {
    requests_.push_back(request);
    if (responses_.empty()) {
        return {599, {}, "mock response queue empty"};
    }

    auto response = responses_.front();
    responses_.erase(responses_.begin());
    return response;
}

const std::vector<HttpRequest>& MockHttpClient::requests() const {
    return requests_;
}

const char* toString(HttpMethod method) {
    switch (method) {
    case HttpMethod::Get:
        return "GET";
    case HttpMethod::Post:
        return "POST";
    case HttpMethod::Put:
        return "PUT";
    case HttpMethod::Delete:
        return "DELETE";
    }
    return "UNKNOWN";
}

} // namespace opennow::net
