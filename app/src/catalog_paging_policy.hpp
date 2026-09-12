#pragma once

#include "models.hpp"

#include <stdexcept>
#include <unordered_set>
#include <utility>

namespace opennow
{
class CatalogListing
{
  public:
    const CatalogPage& page() const { return page_; }

    void Apply(CatalogPage incoming, bool append)
    {
        if (incoming.next_cursor && (incoming.next_cursor->empty() ||
            (append && (incoming.next_cursor == page_.next_cursor ||
                visited_cursors_.count(*incoming.next_cursor) != 0))))
            throw std::runtime_error("Catalog pagination did not advance");
        if (append && !page_.next_cursor)
            throw std::runtime_error("Catalog has no next page");
        if (!append)
        {
            page_ = {};
            visited_cursors_.clear();
        }
        else
            visited_cursors_.insert(*page_.next_cursor);

        std::unordered_set<std::string> ids;
        for (const auto& game : page_.games)
            ids.insert(game.id);
        for (auto& game : incoming.games)
        {
            if (ids.insert(game.id).second)
                page_.games.push_back(std::move(game));
        }
        page_.next_cursor = std::move(incoming.next_cursor);
        page_.total_count = incoming.total_count;
    }

  private:
    CatalogPage page_;
    std::unordered_set<std::string> visited_cursors_;
};
}
