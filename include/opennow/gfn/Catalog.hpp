#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "opennow/gfn/GfnTypes.hpp"
#include "opennow/net/HttpClient.hpp"

namespace opennow::gfn {

struct CatalogGameVariant {
    std::string id;
    std::string store;
    std::string status;
    std::string libraryStatus;
    bool selected = false;
};

struct CatalogGame {
    std::string id;
    std::string title;
    std::string heroImageUrl;
    std::string bannerImageUrl;
    std::string boxArtUrl;
    std::string keyArtUrl;
    std::string playabilityState;
    std::string playType;
    std::string minimumTier;
    std::vector<CatalogGameVariant> variants;
};

struct CatalogResult {
    bool ok = false;
    std::string error;
    std::string vpcId;
    std::uint32_t totalCount = 0;
    bool hasNextPage = false;
    std::string endCursor;
    std::vector<CatalogGame> games;
};

struct CatalogFetchOptions {
    bool libraryOnly = false;
    std::uint32_t first = 50;
    std::string cursor;
    std::string language = "en_US";
};

class CatalogService {
public:
    explicit CatalogService(net::HttpClient& http);

    CatalogResult fetchCatalog(const AuthSession* session, const CatalogFetchOptions& options = {});
    CatalogResult fetchLibrary(const AuthSession& session, const CatalogFetchOptions& options = {});

private:
    std::string fetchVpcId(std::string& error);
    net::HttpRequest buildCatalogRequest(const AuthSession* session, const std::string& vpcId, const CatalogFetchOptions& options) const;
    CatalogResult parseCatalogResponse(const std::string& body, const std::string& vpcId) const;

    net::HttpClient& http_;
};

} // namespace opennow::gfn
