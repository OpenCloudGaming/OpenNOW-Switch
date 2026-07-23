#include "internal.hpp"

#include "../community_proxy_policy.hpp"
#include "../play_history.hpp"
#include "../server_location_policy.hpp"
#include "../stream_settings.hpp"

#include <algorithm>
#include <cctype>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace opennow
{
using namespace gfn::detail;

namespace
{
constexpr const char* kServiceUrlsEndpoint = "https://pcs.geforcenow.com/v1/serviceUrls";
constexpr const char* kPublicCatalogEndpoint =
    "https://static.nvidiagrid.net/supported-public-game-list/locales/gfnpc-en-US.json";
constexpr const char* kGraphQlEndpoint     = "https://games.geforce.com/graphql";
constexpr const char* kClientVersion       = "2.0.80.173";
constexpr const char* kLcarsClientId       = "ec7e38d4-03af-4b58-b131-cfb0495903ab";
constexpr const char* kDefaultLocale       = "en_US";
constexpr const char* kPanelsQueryHash = "f8e26265a5db5c20e1334a6872cf04b6e3970507697f6ae55a6ddefa5420daf0";
constexpr const char* kLibraryWithTimeQueryHash = "039e8c0d553972975485fee56e59f2549d2fdb518e247a42ab5022056a74406f";
constexpr const char* kPlayOrigin          = "https://play.geforcenow.com";
constexpr const char* kPlayReferer         = "https://play.geforcenow.com/";

std::vector<std::string> BuildGfnLcarsHeaders(
    const std::string& token,
    const std::string& client_type,
    const std::string& client_streamer,
    bool include_user_agent)
{
    std::vector<std::string> headers;
    headers.push_back("Accept: application/json");
    headers.push_back("nv-client-id: " + std::string(kLcarsClientId));
    headers.push_back("nv-client-type: " + client_type);
    headers.push_back("nv-client-version: " + std::string(kClientVersion));
    headers.push_back("nv-client-streamer: " + client_streamer);
    headers.push_back("nv-device-os: WINDOWS");
    headers.push_back("nv-device-type: DESKTOP");

    if (!token.empty())
        headers.push_back("Authorization: GFNJWT " + token);

    if (include_user_agent)
        headers.push_back("User-Agent: " + std::string(GfnClient::kUserAgent));

    return headers;
}

std::vector<std::string> BuildGraphQlHeaders(const std::string& token)
{
    std::vector<std::string> headers;
    headers.push_back("Accept: application/json, text/plain, */*");
    // NVIDIA's Switch-facing persisted-query endpoint rejects this GET as
    // malformed JSON. application/graphql keeps both library and play-time
    // fields available while preserving the endpoint's expected request type.
    headers.push_back("Content-Type: application/graphql");
    headers.push_back("Origin: " + std::string(kPlayOrigin));
    headers.push_back("Referer: " + std::string(kPlayReferer));
    headers.push_back("nv-client-id: " + std::string(kLcarsClientId));
    headers.push_back("nv-client-type: NATIVE");
    headers.push_back("nv-client-version: " + std::string(kClientVersion));
    headers.push_back("nv-client-streamer: NVIDIA-CLASSIC");
    headers.push_back("nv-device-os: WINDOWS");
    headers.push_back("nv-device-type: DESKTOP");
    headers.push_back("nv-device-make: UNKNOWN");
    headers.push_back("nv-device-model: UNKNOWN");
    headers.push_back("nv-browser-type: CHROME");
    headers.push_back("User-Agent: " + std::string(GfnClient::kUserAgent));

    if (!token.empty())
        headers.push_back("Authorization: GFNJWT " + token);

    return headers;
}

std::string OptimizeImageUrl(const std::string& url)
{
    if (url.find("img.nvidiagrid.net") != std::string::npos)
        return url + ";w=272";

    return url;
}

bool IsOwnedLibraryStatus(const std::string& status)
{
    return status == "MANUAL" || status == "PLATFORM_SYNC" || status == "IN_LIBRARY";
}

bool IsNumericId(const std::string& value)
{
    if (value.empty())
        return false;

    return std::all_of(value.begin(), value.end(), [](unsigned char ch) {
        return std::isdigit(ch) != 0;
    });
}

GameInfo ParseApp(json_t* app)
{
    GameInfo game;
    game.id                 = GetString(app, "id");
    game.uuid               = game.id;
    game.title              = GetString(app, "title");
    game.description        = GetString(app, "description");
    if (game.description.empty())
        game.description = GetString(app, "longDescription");
    if (game.description.empty())
        game.description = GetString(app, "shortDescription");
    game.publisher          = GetString(app, "publisherName");
    json_t* app_gfn = json_object_get(app, "gfn");
    game.membership_tier_label = GetString(app_gfn, "minimumMembershipTierLabel");
    json_t* app_library = app_gfn ? json_object_get(app_gfn, "library") : nullptr;
    game.last_played = GetString(app_library, "lastPlayedDate");
    if (game.last_played.empty())
        game.last_played = GetString(app_library, "lastPlayedAt");

    json_t* images = json_object_get(app, "images");
    for (const char* key : {"KEY_ART", "GAME_BOX_ART", "TV_BANNER", "HERO_IMAGE"})
    {
        const std::string candidate = GetString(images, key);
        if (!candidate.empty())
        {
            game.image_url = OptimizeImageUrl(candidate);
            break;
        }
    }

    json_t* variants = json_object_get(app, "variants");
    if (json_is_array(variants))
    {
        size_t index = 0;
        json_t* entry = nullptr;
        json_array_foreach(variants, index, entry)
        {
            GameVariant variant;
            variant.id    = GetString(entry, "id");
            variant.store = GetString(entry, "appStore");

            json_t* gfn = json_object_get(entry, "gfn");
            variant.gfn_status = GetString(gfn, "status");

            json_t* library = gfn ? json_object_get(gfn, "library") : nullptr;
            variant.library_status   = GetString(library, "status");
            variant.library_selected = GetBool(library, "selected");
            variant.last_played_date = GetString(library, "lastPlayedDate");
            if (variant.last_played_date.empty())
                variant.last_played_date = GetString(library, "lastPlayedAt");
            if (variant.last_played_date.empty())
                variant.last_played_date = GetString(library, "lastPlayed");

            if (variant.library_selected) {
                game.selected_variant_index = index;
                game.launch_app_id = variant.id;
            }

            if (!variant.last_played_date.empty() &&
                (game.last_played.empty() || variant.last_played_date > game.last_played))
                game.last_played = variant.last_played_date;

            if (!variant.store.empty() &&
                std::find(game.available_stores.begin(), game.available_stores.end(), variant.store) ==
                    game.available_stores.end())
            {
                game.available_stores.push_back(variant.store);
            }

            if (IsOwnedLibraryStatus(variant.library_status))
                game.is_in_library = true;

            if (game.launch_app_id.empty() && IsNumericId(variant.id))
                game.launch_app_id = variant.id;

            game.variants.push_back(std::move(variant));
        }
    }

    if (game.launch_app_id.empty() && IsNumericId(game.id))
        game.launch_app_id = game.id;

    return game;
}

void MergeGame(GameInfo& target, const GameInfo& incoming)
{
    if (target.image_url.empty())
        target.image_url = incoming.image_url;

    if (target.description.empty())
        target.description = incoming.description;

    if (target.publisher.empty())
        target.publisher = incoming.publisher;

    if (!incoming.last_played.empty() &&
        (target.last_played.empty() || incoming.last_played > target.last_played))
        target.last_played = incoming.last_played;

    if (target.launch_app_id.empty())
        target.launch_app_id = incoming.launch_app_id;

    if (target.membership_tier_label.empty())
        target.membership_tier_label = incoming.membership_tier_label;

    target.is_in_library = target.is_in_library || incoming.is_in_library;

    for (const auto& store : incoming.available_stores)
    {
        if (std::find(target.available_stores.begin(), target.available_stores.end(), store) ==
            target.available_stores.end())
        {
            target.available_stores.push_back(store);
        }
    }

    for (const auto& variant : incoming.variants)
    {
        const auto it = std::find_if(
            target.variants.begin(),
            target.variants.end(),
            [&variant](const GameVariant& existing) { return existing.id == variant.id; });

        if (it == target.variants.end())
            target.variants.push_back(variant);
    }
}

void ThrowIfGraphQlFailed(json_t* root)
{
    json_t* errors = root ? json_object_get(root, "errors") : nullptr;
    if (!json_is_array(errors) || json_array_size(errors) == 0)
        return;

    std::string message = "GFN GraphQL returned errors";
    json_t* first       = json_array_get(errors, 0);
    const std::string first_message = GetString(first, "message");
    if (!first_message.empty())
        message += ": " + first_message;

    throw std::runtime_error(message);
}

std::string BuildLibraryUrl(const std::string& vpc_id, bool with_library_time)
{
    const std::string variables =
        std::string("{\"vpcId\":\"") + vpc_id + "\",\"locale\":\"" + kDefaultLocale +
        "\",\"panelNames\":[\"LIBRARY\"]}";

    const std::string extensions =
        std::string("{\"persistedQuery\":{\"sha256Hash\":\"") +
        (with_library_time ? kLibraryWithTimeQueryHash : kPanelsQueryHash) + "\"}}";

    std::string url = std::string(kGraphQlEndpoint) + "?";
    url += "requestType=" + UrlEncode("panels/Library");
    url += "&extensions=" + UrlEncode(extensions);
    url += "&huId=" + UrlEncode(HexEncode(GenerateRandomBytes(8).data(), 8));
    url += "&variables=" + UrlEncode(variables);
    return url;
}

std::string ResolveVpcId(
    const HttpClient& http_client,
    const AuthSession& session,
    const std::string& proxy_url)
{
    const StreamSettings settings = LoadStreamSettings();
    const std::string streaming_base_url = server_location::ResolveStreamingBaseUrl(
        settings.region, session.provider.streaming_service_url);
    if (streaming_base_url.empty())
        return "GFN-PC";

    const HttpResponse response = http_client.Get(
        streaming_base_url + "v2/serverInfo",
        GfnClient::kUserAgent,
        BuildGfnLcarsHeaders(ResolveSessionJwt(session), "NATIVE", "NVIDIA-CLASSIC", true),
        proxy_url);

    if (response.status_code != 200)
        return "GFN-PC";

    JsonPtr root = LoadJson(response.body);
    json_t* request_status = json_object_get(root.get(), "requestStatus");
    const std::string server_id = GetString(request_status, "serverId");
    return server_id.empty() ? std::string("GFN-PC") : server_id;
}

std::vector<GameInfo> ParseLibraryGames(JsonPtr& root)
{
    ThrowIfGraphQlFailed(root.get());

    std::unordered_map<std::string, size_t> index_by_id;
    std::vector<GameInfo> games;

    json_t* data   = json_object_get(root.get(), "data");
    json_t* panels = data ? json_object_get(data, "panels") : nullptr;
    if (!json_is_array(panels))
        return {};

    size_t panel_index = 0;
    json_t* panel      = nullptr;
    json_array_foreach(panels, panel_index, panel)
    {
        json_t* sections = json_object_get(panel, "sections");
        if (!json_is_array(sections))
            continue;

        size_t section_index = 0;
        json_t* section      = nullptr;
        json_array_foreach(sections, section_index, section)
        {
            json_t* items = json_object_get(section, "items");
            if (!json_is_array(items))
                continue;

            size_t item_index = 0;
            json_t* item      = nullptr;
            json_array_foreach(items, item_index, item)
            {
                if (GetString(item, "__typename") != "GameItem")
                    continue;

                json_t* app = json_object_get(item, "app");
                if (!json_is_object(app))
                    continue;

                GameInfo game = ParseApp(app);
                if (game.id.empty() || game.title.empty())
                    continue;

                const auto existing = index_by_id.find(game.id);
                if (existing == index_by_id.end())
                {
                    index_by_id.emplace(game.id, games.size());
                    games.push_back(std::move(game));
                }
                else
                {
                    MergeGame(games[existing->second], game);
                }
            }
        }
    }

    return games;
}

PublicGame ToPublicGame(const GameInfo& source)
{
    PublicGame game;
    game.id                    = source.id;
    game.uuid                  = source.uuid;
    game.launch_app_id         = source.launch_app_id;
    game.title                 = source.title;
    game.publisher             = source.publisher;
    game.image_url             = source.image_url;
    game.membership_tier_label = source.membership_tier_label;
    game.is_in_library         = source.is_in_library;
    game.variants              = source.variants;

    if (source.selected_variant_index < source.variants.size())
        game.store = source.variants[source.selected_variant_index].store;
    if (game.store.empty() && !source.available_stores.empty())
        game.store = source.available_stores.front();
    if (game.store.empty())
        game.store = "Unknown";
    return game;
}

std::vector<PublicGame> ParseCatalogPage(
    JsonPtr& root, bool& has_next_page, std::string& end_cursor)
{
    ThrowIfGraphQlFailed(root.get());
    has_next_page = false;
    end_cursor.clear();

    json_t* data = json_object_get(root.get(), "data");
    json_t* apps = data ? json_object_get(data, "apps") : nullptr;
    json_t* items = apps ? json_object_get(apps, "items") : nullptr;
    if (!json_is_array(items))
        return {};

    json_t* page_info = json_object_get(apps, "pageInfo");
    has_next_page = GetBool(page_info, "hasNextPage");
    end_cursor = GetString(page_info, "endCursor");

    std::vector<PublicGame> games;
    size_t index = 0;
    json_t* app = nullptr;
    json_array_foreach(items, index, app)
    {
        GameInfo parsed = ParseApp(app);
        if (!parsed.id.empty() && !parsed.title.empty())
            games.push_back(ToPublicGame(parsed));
    }
    return games;
}

std::string BuildCatalogRequestBody(
    const std::string& vpc_id, const std::string& search_query,
    const std::string& cursor)
{
    static const char* kBrowseQuery = R"GRAPHQL(
query GetFilterBrowseResults($vpcId: String!, $locale: String!, $sortString: String!, $fetchCount: Int!, $cursor: String!, $filters: AppFilterFields!) {
  apps(vpcId: $vpcId, language: $locale, orderBy: $sortString, first: $fetchCount, after: $cursor, filters: $filters) {
    numberReturned numberSupported
    pageInfo { hasNextPage endCursor totalCount }
    items {
      id title longDescription shortDescription publisherName
      images { KEY_ART GAME_BOX_ART TV_BANNER HERO_IMAGE }
      variants { id appStore gfn { status library { status selected } } }
      gfn { playabilityState minimumMembershipTierLabel }
    }
  }
})GRAPHQL";
    static const char* kSearchQuery = R"GRAPHQL(
query GetSearchFilterResults($vpcId: String!, $locale: String!, $sortString: String!, $fetchCount: Int!, $cursor: String!, $searchString: String!, $filters: AppFilterFields!) {
  apps(vpcId: $vpcId, language: $locale, orderBy: $sortString, first: $fetchCount, after: $cursor, searchQuery: $searchString, filters: $filters) {
    numberReturned numberSupported
    pageInfo { hasNextPage endCursor totalCount }
    items {
      id title longDescription shortDescription publisherName
      images { KEY_ART GAME_BOX_ART TV_BANNER HERO_IMAGE }
      variants { id appStore gfn { status library { status selected } } }
      gfn { playabilityState minimumMembershipTierLabel }
    }
  }
})GRAPHQL";

    JsonPtr body(json_object(), &json_decref);
    JsonPtr variables(json_object(), &json_decref);
    json_object_set_new(body.get(), "query", json_string(search_query.empty() ? kBrowseQuery : kSearchQuery));
    json_object_set_new(variables.get(), "vpcId", json_string(vpc_id.c_str()));
    json_object_set_new(variables.get(), "locale", json_string(kDefaultLocale));
    json_object_set_new(
        variables.get(), "sortString",
        json_string("itemMetadata.relevance:DESC,sortName:ASC"));
    json_object_set_new(variables.get(), "fetchCount", json_integer(120));
    json_object_set_new(variables.get(), "cursor", json_string(cursor.c_str()));
    json_object_set_new(variables.get(), "filters", json_object());
    if (!search_query.empty())
        json_object_set_new(variables.get(), "searchString", json_string(search_query.c_str()));
    json_object_set_new(body.get(), "variables", json_incref(variables.get()));
    return DumpJson(body.get());
}

std::vector<std::string> BuildGraphQlPostHeaders(const std::string& token)
{
    std::vector<std::string> headers = BuildGraphQlHeaders(token);
    for (std::string& header : headers)
    {
        if (header.find("Content-Type:") == 0)
            header = "Content-Type: application/json";
    }
    return headers;
}

std::string InferStore(json_t* item)
{
    const std::string explicit_store = GetString(item, "store");
    if (!explicit_store.empty())
        return explicit_store;

    const std::string publisher = GetString(item, "publisher");
    if (publisher.find("NCSoft") != std::string::npos ||
        publisher.find("ncsoft") != std::string::npos)
    {
        return "NCSoft";
    }

    return "Unknown";
}

std::string BuildSteamImageUrl(const std::string& steam_url)
{
    const std::string marker = "/app/";
    const auto marker_pos    = steam_url.find(marker);
    if (marker_pos == std::string::npos)
        return "";

    const auto id_begin = marker_pos + marker.size();
    const auto id_end   = steam_url.find('/', id_begin);
    const std::string steam_id =
        steam_url.substr(id_begin, id_end == std::string::npos ? std::string::npos : id_end - id_begin);

    if (steam_id.empty())
        return "";

    return "https://cdn.cloudflare.steamstatic.com/steam/apps/" + steam_id + "/library_600x900.jpg";
}

} // namespace

std::vector<LoginProvider> GfnClient::FetchLoginProviders() const
{
    const HttpResponse response = http_client_.Get(
        kServiceUrlsEndpoint,
        kUserAgent,
        {"Accept: application/json"});

    if (response.status_code != 200)
    {
        throw std::runtime_error(
            "Provider discovery failed with HTTP " + std::to_string(response.status_code));
    }

    JsonPtr root = LoadJson(response.body);
    json_t* gfn_service_info =
        json_object_get(root.get(), "gfnServiceInfo");
    json_t* endpoints =
        gfn_service_info ? json_object_get(gfn_service_info, "gfnServiceEndpoints") : nullptr;

    std::vector<LoginProvider> providers;
    if (json_is_array(endpoints))
    {
        size_t index = 0;
        json_t* entry = nullptr;
        json_array_foreach(endpoints, index, entry)
        {
            LoginProvider provider;
            provider.idp_id                = GetString(entry, "idpId");
            provider.code                  = GetString(entry, "loginProviderCode");
            provider.display_name          = GetString(entry, "loginProviderDisplayName");
            provider.streaming_service_url = EnsureTrailingSlash(GetString(entry, "streamingServiceUrl"));
            provider.priority              = GetInteger(entry, "loginProviderPriority", 0);

            if (provider.code == "BPC")
                provider.display_name = "bro.game";

            if (!provider.idp_id.empty() && !provider.streaming_service_url.empty())
                providers.push_back(provider);
        }
    }

    if (providers.empty())
        providers.push_back(DefaultProvider());

    std::sort(
        providers.begin(),
        providers.end(),
        [](const LoginProvider& left, const LoginProvider& right) {
            return left.priority < right.priority;
        });

    return providers;
}

std::vector<PublicGame> GfnClient::FetchPublicGames() const
{
    const std::string proxy_url = community_proxy::EnabledUrl(LoadStreamSettings());
    const HttpResponse response = http_client_.Get(
        kPublicCatalogEndpoint,
        kUserAgent,
        {"Accept: application/json"},
        proxy_url);

    if (response.status_code != 200)
    {
        throw std::runtime_error(
            "Public catalog fetch failed with HTTP " + std::to_string(response.status_code));
    }

    JsonPtr root = LoadJson(response.body);
    if (!json_is_array(root.get()))
        throw std::runtime_error("Public catalog payload is not a JSON array");

    std::vector<PublicGame> games;

    size_t index = 0;
    json_t* item = nullptr;
    json_array_foreach(root.get(), index, item)
    {
        const std::string status = GetString(item, "status");
        const std::string title  = GetString(item, "title");
        if (status != "AVAILABLE" || title.empty())
            continue;

        PublicGame game;
        game.id        = GetString(item, "id");
        game.title     = title;
        game.store     = InferStore(item);
        game.publisher = GetString(item, "publisher");
        game.image_url = BuildSteamImageUrl(GetString(item, "steamUrl"));
        if (game.id.empty())
            game.id = game.title;

        games.push_back(std::move(game));
    }

    std::sort(
        games.begin(),
        games.end(),
        [](const PublicGame& left, const PublicGame& right) {
            return left.title < right.title;
        });

    return games;
}

std::vector<PublicGame> GfnClient::FetchCatalogGames(
    AuthSession& session, const std::string& search_query) const
{
    session = RecoverSavedSession(session);
    std::string jwt_token = ResolveSessionJwt(session);
    const std::string proxy_url = community_proxy::EnabledUrl(LoadStreamSettings());
    const std::string vpc_id = ResolveVpcId(http_client_, session, proxy_url);

    std::vector<PublicGame> games;
    std::unordered_set<std::string> seen_ids;
    std::string cursor;

    for (int page = 0; page < 3; ++page)
    {
        HttpResponse response = http_client_.Post(
            kGraphQlEndpoint,
            kUserAgent,
            BuildGraphQlPostHeaders(jwt_token),
            BuildCatalogRequestBody(vpc_id, search_query, cursor),
            proxy_url);

        if (response.status_code == 401)
        {
            session = RecoverSavedSession(session, true);
            jwt_token = ResolveSessionJwt(session);
            response = http_client_.Post(
                kGraphQlEndpoint,
                kUserAgent,
                BuildGraphQlPostHeaders(jwt_token),
                BuildCatalogRequestBody(vpc_id, search_query, cursor),
                proxy_url);
        }

        if (response.status_code != 200)
        {
            throw std::runtime_error(
                "Catalog browse failed with HTTP " + std::to_string(response.status_code));
        }

        JsonPtr root = LoadJson(response.body);
        bool has_next_page = false;
        std::string end_cursor;
        std::vector<PublicGame> page_games =
            ParseCatalogPage(root, has_next_page, end_cursor);
        for (PublicGame& game : page_games)
        {
            const std::string key = game.id.empty() ? game.title : game.id;
            if (seen_ids.insert(key).second)
                games.push_back(std::move(game));
        }

        if (!has_next_page || end_cursor.empty() || end_cursor == cursor)
            break;
        cursor = std::move(end_cursor);
    }

    return games;
}
std::vector<GameInfo> GfnClient::FetchLibraryGames(AuthSession& session) const
{
    session = RecoverSavedSession(session);
    std::string jwt_token = ResolveSessionJwt(session);
    const std::string proxy_url = community_proxy::EnabledUrl(LoadStreamSettings());
    const std::string vpc_id = ResolveVpcId(http_client_, session, proxy_url);

    HttpResponse response = http_client_.Get(
        BuildLibraryUrl(vpc_id, true),
        kUserAgent,
        BuildGraphQlHeaders(jwt_token),
        proxy_url);

    if (response.status_code == 401)
    {
        session = RecoverSavedSession(session, true);
        jwt_token = ResolveSessionJwt(session);
        response = http_client_.Get(
            BuildLibraryUrl(vpc_id, true),
            kUserAgent,
            BuildGraphQlHeaders(jwt_token),
            proxy_url);
    }

    if (response.status_code != 200)
    {
        response = http_client_.Get(
            BuildLibraryUrl(vpc_id, false),
            kUserAgent,
            BuildGraphQlHeaders(jwt_token),
            proxy_url);
    }

    if (response.status_code != 200)
    {
        throw std::runtime_error(
            "Library fetch failed with HTTP " + std::to_string(response.status_code));
    }

    JsonPtr root = LoadJson(response.body);
    std::vector<GameInfo> games = ParseLibraryGames(root);
    ApplyPlayHistory(games, LoadPlayHistory());
    return games;
}

} // namespace opennow
