#include "opennow/gfn/Catalog.hpp"

#include "opennow/util/Json.hpp"

#include <algorithm>
#include <cctype>
#include <sstream>

namespace opennow::gfn {
namespace {

constexpr const char* kCloudMatchServerInfo = "https://prod.cloudmatchbeta.nvidiagrid.net/v2/serverInfo";
constexpr const char* kLcarsPublicGraphql = "https://public.games.geforce.com/graphql";
constexpr const char* kLcarsUserGraphql = "https://games.geforce.com/graphql";
constexpr const char* kLcarsClientId = "ec7e38d4-03af-4b58-b131-cfb0495903ab";
constexpr const char* kLcarsClientVersion = "2.0.80.173";
constexpr const char* kSteamDeckUserAgent =
    "Mozilla/5.0 (X11; Linux x86_64; Steam Deck) AppleWebKit/537.36 "
    "(KHTML, like Gecko) Chrome/128.0.0.0 Safari/537.36";

std::string graphQlString(const std::string& value) {
    std::string out;
    out.reserve(value.size() + 2);
    out.push_back('"');
    for (const char c : value) {
        switch (c) {
        case '\\': out += "\\\\"; break;
        case '"': out += "\\\""; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        default: out.push_back(c); break;
        }
    }
    out.push_back('"');
    return out;
}

std::uint32_t jsonU32(const util::JsonValue* value) {
    if (!value || !value->isNumber()) {
        return 0;
    }
    const auto number = value->asNumber(0);
    return number <= 0 ? 0U : static_cast<std::uint32_t>(number);
}

std::string objectString(const util::JsonValue* object, const char* key) {
    if (!object) {
        return {};
    }
    if (const auto* value = object->get(key)) {
        return value->asString();
    }
    return {};
}

std::string firstImageString(const util::JsonValue* images, const char* key) {
    const auto* value = images ? images->get(key) : nullptr;
    if (!value) {
        return {};
    }
    if (value->isString()) {
        return value->asString();
    }
    if (value->isArray()) {
        for (const auto& item : value->asArray()) {
            if (item.isString() && !item.asString().empty()) {
                return item.asString();
            }
            if (item.isObject()) {
                auto url = objectString(&item, "url");
                if (url.empty()) url = objectString(&item, "URL");
                if (url.empty()) url = objectString(&item, "src");
                if (!url.empty()) {
                    return url;
                }
            }
        }
    }
    if (value->isObject()) {
        auto url = objectString(value, "url");
        if (url.empty()) url = objectString(value, "URL");
        if (url.empty()) url = objectString(value, "src");
        return url;
    }
    return {};
}

bool objectBool(const util::JsonValue* object, const char* key) {
    if (!object) {
        return false;
    }
    if (const auto* value = object->get(key)) {
        return value->asBool(false);
    }
    return false;
}

std::string firstVariantStores(const CatalogGame& game) {
    std::string out;
    for (const auto& variant : game.variants) {
        if (variant.store.empty()) {
            continue;
        }
        if (out.find(variant.store) != std::string::npos) {
            continue;
        }
        if (!out.empty()) {
            out += ", ";
        }
        out += variant.store;
    }
    return out;
}

CatalogResult catalogError(std::string error, std::string vpcId = {}) {
    CatalogResult result;
    result.ok = false;
    result.error = std::move(error);
    result.vpcId = std::move(vpcId);
    return result;
}

std::string catalogAuthToken(const AuthSession* session) {
    if (!session) {
        return {};
    }
    // OpenNOW desktop sends the ID token as GFNJWT for games/graphql and only
    // falls back to the OAuth access token when the ID token is unavailable.
    if (!session->tokens.idToken.empty()) {
        return session->tokens.idToken;
    }
    return session->tokens.accessToken;
}

} // namespace

CatalogService::CatalogService(net::HttpClient& http)
    : http_(http) {}

CatalogResult CatalogService::fetchCatalog(const AuthSession* session, const CatalogFetchOptions& options) {
    std::string error;
    const auto vpcId = fetchVpcId(error);
    if (vpcId.empty()) {
        return catalogError("Could not resolve GFN region id: " + error);
    }

    auto request = buildCatalogRequest(session, vpcId, options);
    const auto response = http_.send(request);
    if (response.status < 200 || response.status >= 300) {
        std::ostringstream message;
        message << "Catalog request failed with HTTP " << response.status;
        if (!response.body.empty()) {
            message << ": " << response.body.substr(0, 300);
        }
        return catalogError(message.str(), vpcId);
    }

    return parseCatalogResponse(response.body, vpcId);
}

CatalogResult CatalogService::fetchLibrary(const AuthSession& session, const CatalogFetchOptions& options) {
    auto libraryOptions = options;
    libraryOptions.libraryOnly = true;
    return fetchCatalog(&session, libraryOptions);
}

std::string CatalogService::fetchVpcId(std::string& error) {
    const auto response = http_.send({
        net::HttpMethod::Get,
        kCloudMatchServerInfo,
        {
            {"Accept", "application/json, text/plain, */*"},
            {"User-Agent", kSteamDeckUserAgent},
        },
        "",
    });
    if (response.status < 200 || response.status >= 300) {
        std::ostringstream out;
        out << "serverInfo HTTP " << response.status;
        error = out.str();
        return {};
    }

    const auto parsed = util::parseJson(response.body);
    if (!parsed.ok) {
        error = parsed.error;
        return {};
    }
    const auto* requestStatus = parsed.value.get("requestStatus");
    auto serverId = objectString(requestStatus, "serverId");
    if (serverId.empty()) {
        error = "serverInfo response did not include requestStatus.serverId";
        return {};
    }
    std::transform(serverId.begin(), serverId.end(), serverId.begin(), [](unsigned char c) {
        return static_cast<char>(std::toupper(c));
    });
    return serverId;
}

net::HttpRequest CatalogService::buildCatalogRequest(const AuthSession* session, const std::string& vpcId, const CatalogFetchOptions& options) const {
    const auto first = std::max<std::uint32_t>(1, std::min<std::uint32_t>(options.first, 100));

    std::ostringstream query;
    query
        << "query OpenNowCatalog {"
        << " apps("
        << "vpcId:" << graphQlString(vpcId)
        << ", language:" << graphQlString(options.language.empty() ? "en_US" : options.language)
        << ", first:" << first
        << ", after:" << graphQlString(options.cursor);
    if (options.libraryOnly) {
        query << ", filters:{variants:{gfn:{library:{status:{notEquals:\"NOT_OWNED\"}}}}}";
    }
    query
        << ") {"
        << " numberReturned numberSupported"
        << " pageInfo { totalCount hasNextPage endCursor }"
        << " items {"
        << " id title"
        << " images { TV_BANNER HERO_IMAGE KEY_ART GAME_BOX_ART }"
        << " variants { id appStore supportedControls gfn { status library { status selected installed } } }"
        << " gfn { playabilityState minimumMembershipTierLabel playType catalogSkuStrings { SKU_BASED_TAG } }"
        << " }"
        << " }"
        << "}";

    net::Headers headers{
        {"Accept", "application/json, text/plain, */*"},
        {"Content-Type", "application/graphql"},
        {"Origin", "https://play.geforcenow.com"},
        {"Referer", "https://play.geforcenow.com/"},
        {"nv-client-id", kLcarsClientId},
        {"nv-client-type", "NATIVE"},
        {"nv-client-version", kLcarsClientVersion},
        {"nv-client-streamer", "NVIDIA-CLASSIC"},
        {"nv-device-os", "LINUX"},
        {"nv-device-type", "DESKTOP"},
        {"nv-device-make", "Valve"},
        {"nv-device-model", "Steam Deck"},
        {"nv-browser-type", "CHROME"},
        {"User-Agent", kSteamDeckUserAgent},
    };
    const auto token = catalogAuthToken(session);
    if (!token.empty()) {
        headers["Authorization"] = "GFNJWT " + token;
    }

    const auto authenticated = !token.empty();
    return {
        net::HttpMethod::Post,
        authenticated ? kLcarsUserGraphql : kLcarsPublicGraphql,
        std::move(headers),
        query.str(),
    };
}

CatalogResult CatalogService::parseCatalogResponse(const std::string& body, const std::string& vpcId) const {
    const auto parsed = util::parseJson(body);
    if (!parsed.ok) {
        return catalogError("Catalog JSON parse failed: " + parsed.error, vpcId);
    }
    if (const auto* errors = parsed.value.get("errors"); errors && errors->isArray() && !errors->asArray().empty()) {
        const auto* first = errors->at(0);
        const auto message = objectString(first, "message");
        return catalogError(message.empty() ? "Catalog GraphQL returned errors" : message, vpcId);
    }

    const auto* data = parsed.value.get("data");
    const auto* apps = data ? data->get("apps") : nullptr;
    if (!apps) {
        return catalogError("Catalog response did not include data.apps", vpcId);
    }

    CatalogResult result;
    result.ok = true;
    result.vpcId = vpcId;
    if (const auto* pageInfo = apps->get("pageInfo")) {
        result.totalCount = jsonU32(pageInfo->get("totalCount"));
        result.hasNextPage = objectBool(pageInfo, "hasNextPage");
        result.endCursor = objectString(pageInfo, "endCursor");
    }

    const auto* items = apps->get("items");
    if (!items || !items->isArray()) {
        return result;
    }

    for (const auto& item : items->asArray()) {
        CatalogGame game;
        game.id = objectString(&item, "id");
        game.title = objectString(&item, "title");
        if (const auto* images = item.get("images")) {
            game.heroImageUrl = firstImageString(images, "HERO_IMAGE");
            game.bannerImageUrl = firstImageString(images, "TV_BANNER");
            game.boxArtUrl = firstImageString(images, "GAME_BOX_ART");
            game.keyArtUrl = firstImageString(images, "KEY_ART");
        }
        if (const auto* gfn = item.get("gfn")) {
            game.playabilityState = objectString(gfn, "playabilityState");
            game.playType = objectString(gfn, "playType");
            game.minimumTier = objectString(gfn, "minimumMembershipTierLabel");
        }
        if (const auto* variants = item.get("variants"); variants && variants->isArray()) {
            for (const auto& value : variants->asArray()) {
                CatalogGameVariant variant;
                variant.id = objectString(&value, "id");
                variant.store = objectString(&value, "appStore");
                if (const auto* gfn = value.get("gfn")) {
                    variant.status = objectString(gfn, "status");
                    if (const auto* library = gfn->get("library")) {
                        variant.libraryStatus = objectString(library, "status");
                        variant.selected = objectBool(library, "selected");
                    }
                }
                game.variants.push_back(std::move(variant));
            }
        }
        if (game.title.empty()) {
            continue;
        }
        if (game.playType.empty()) {
            game.playType = firstVariantStores(game);
        }
        result.games.push_back(std::move(game));
    }

    return result;
}

} // namespace opennow::gfn
