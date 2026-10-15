#include "../app/src/gfn/catalog.cpp"

#include <cassert>

namespace
{
void Reject(const std::string& payload)
{
    auto root = opennow::gfn::detail::LoadJson(payload);
    bool rejected = false;
    try
    {
        opennow::ParseCatalogPage(root, "previous");
    }
    catch (const std::runtime_error&)
    {
        rejected = true;
    }
    assert(rejected);
}
}

int main()
{
    const auto request = opennow::gfn::detail::LoadJson(
        opennow::BuildCatalogRequestBody("GFN-PC", "Example", "cursor-60"));
    json_t* variables = json_object_get(request.get(), "variables");
    assert(json_integer_value(json_object_get(variables, "fetchCount")) == 60);
    assert(opennow::gfn::detail::GetString(variables, "cursor") == "cursor-60");
    assert(opennow::gfn::detail::GetString(variables, "searchString") == "Example");
    const auto library_request = opennow::gfn::detail::LoadJson(
        opennow::BuildLibraryRequestBody("GFN-PC", "cursor-120"));
    json_t* library_variables = json_object_get(library_request.get(), "variables");
    assert(json_integer_value(json_object_get(library_variables, "fetchCount")) == 60);
    assert(opennow::gfn::detail::GetString(library_variables, "cursor") == "cursor-120");
    json_t* library_status = json_object_get(json_object_get(json_object_get(json_object_get(
        json_object_get(library_variables, "filters"), "variants"), "gfn"), "library"), "status");
    assert(opennow::gfn::detail::GetString(library_status, "notEquals") == "NOT_OWNED");
    assert(opennow::gfn::detail::GetString(library_request.get(), "query").find("after: $cursor") !=
           std::string::npos);

    auto library_page = opennow::gfn::detail::LoadJson(R"({"data":{"apps":{
        "items":[{"id":"owned","title":"Owned",
            "variants":[{"id":"456","appStore":"STEAM",
                "gfn":{"library":{"status":"PLATFORM_SYNC","lastPlayedDate":"2026-10-01T10:00:00Z"}}}]}],
        "pageInfo":{"hasNextPage":true,"endCursor":"cursor-180"}}}})");
    const auto library_result = opennow::ParseAppsPage(library_page, "cursor-120");
    assert(library_result.next_cursor == "cursor-180");
    assert(library_result.games.size() == 1);
    assert(library_result.games[0].is_in_library);
    assert(library_result.games[0].last_played == "2026-10-01T10:00:00Z");
    assert(library_result.games[0].launch_app_id == "456");

    Reject(R"({})");
    Reject(R"({"data":{"apps":{"items":[]}}})");
    Reject(R"({"data":{"apps":{"items":{},"pageInfo":{"hasNextPage":false}}}})");
    Reject(R"({"data":{"apps":{"items":[],"pageInfo":{"hasNextPage":"false"}}}})");
    Reject(R"({"data":{"apps":{"items":[],"pageInfo":{"hasNextPage":true}}}})");
    Reject(R"({"data":{"apps":{"items":[],"pageInfo":{"hasNextPage":true,"endCursor":"previous"}}}})");
    Reject(R"({"errors":[{"message":"Catalog unavailable"}]})");
    std::string oversized = R"({"data":{"apps":{"items":[)";
    for (int index = 0; index < 61; ++index)
        oversized += index == 0 ? "{}" : ",{}";
    oversized += R"(],"pageInfo":{"hasNextPage":false}}}})";
    Reject(oversized);

    auto empty = opennow::gfn::detail::LoadJson(
        R"({"data":{"apps":{"items":[],"pageInfo":{"hasNextPage":false}}}})");
    const auto empty_page = opennow::ParseCatalogPage(empty, "old");
    assert(empty_page.games.empty());
    assert(!empty_page.next_cursor && !empty_page.total_count);

    auto page = opennow::gfn::detail::LoadJson(R"({"data":{"apps":{
        "items":[null, {"id":"broken"}, {"id":"game","title":"Example",
            "images":{"KEY_ART":"https://img.nvidiagrid.net/example"},
            "variants":[{"id":"123","appStore":"STEAM"}]}],
        "pageInfo":{"hasNextPage":true,"endCursor":"next","totalCount":500}}}})");
    const auto result = opennow::ParseCatalogPage(page, "");
    assert(result.next_cursor == "next" && result.total_count == 500);
    assert(result.games.size() == 1);
    assert(result.games[0].title == "Example");
    assert(result.games[0].launch_app_id == "123");
    assert(result.games[0].image_url == "https://img.nvidiagrid.net/example;w=272");
}
