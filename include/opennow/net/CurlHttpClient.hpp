#pragma once

#include "opennow/net/HttpClient.hpp"

namespace opennow::net {

class CurlHttpClient final : public HttpClient {
public:
    HttpResponse send(const HttpRequest& request) override;
};

} // namespace opennow::net
