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
