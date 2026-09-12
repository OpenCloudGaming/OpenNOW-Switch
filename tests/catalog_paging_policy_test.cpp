#include "catalog_paging_policy.hpp"

#include <cassert>

namespace
{
opennow::CatalogPage Page(int start, int count, const std::string& next)
{
    opennow::CatalogPage page;
    for (int id = start; id < start + count; ++id)
    {
        opennow::PublicGame game;
        game.id = std::to_string(id);
        game.title = "Game " + game.id;
        page.games.push_back(std::move(game));
    }
    if (!next.empty())
        page.next_cursor = next;
    page.total_count = 480;
    return page;
}
}

int main()
{
    opennow::CatalogListing catalog;
    catalog.Apply(Page(0, 60, "60"), false);
    for (int start = 60; start < 480; start += 60)
    {
        assert(catalog.page().next_cursor == std::to_string(start));
        catalog.Apply(Page(start, 60, start == 420 ? "" : std::to_string(start + 60)), true);
    }
    assert(catalog.page().games.size() == 480);
    assert(!catalog.page().next_cursor);
    assert(catalog.page().total_count == 480);

    catalog.Apply(Page(0, 60, "first"), false);
    catalog.Apply(Page(50, 60, "second"), true);
    assert(catalog.page().games.size() == 110);
    for (const auto& cursor : {"first", "second", ""})
    {
        auto invalid = Page(110, 60, "");
        invalid.next_cursor = cursor;
        bool rejected = false;
        try
        {
            catalog.Apply(std::move(invalid), true);
        }
        catch (const std::runtime_error&)
        {
            rejected = true;
        }
        assert(rejected);
        assert(catalog.page().games.size() == 110);
        assert(catalog.page().next_cursor == "second");
    }
    catalog.Apply(Page(800, 1, "first"), false);
    assert(catalog.page().games.size() == 1);
    assert(catalog.page().games.front().id == "800");
}
