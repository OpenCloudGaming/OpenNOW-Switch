#include "library_sort.hpp"

#include <cassert>
#include <vector>

int main()
{
    using opennow::GameInfo;
    using opennow::LibrarySortMode;

    std::vector<GameInfo> games(4);
    games[0].id = "server-first";
    games[0].title = "Zulu";
    games[0].last_played = "2025-01-01T00:00:00Z";
    games[0].available_stores = {"Steam"};
    games[1].id = "newest-played";
    games[1].title = "Alpha";
    games[1].last_played = "2026-06-01T12:00:00Z";
    games[1].available_stores = {"Epic"};
    games[2].id = "never-played";
    games[2].title = "Beta";
    games[2].available_stores = {"Steam"};
    games[3].id = "older-played";
    games[3].title = "Gamma";
    games[3].last_played = "2026-01-01T00:00:00Z";
    games[3].available_stores = {"Epic"};

    std::vector<size_t> indices {0, 1, 2, 3};
    opennow::SortLibraryIndices(indices, games, LibrarySortMode::LastAdded);
    assert((indices == std::vector<size_t> {0, 1, 2, 3}));

    opennow::SortLibraryIndices(indices, games, LibrarySortMode::LastPlayed);
    assert((indices == std::vector<size_t> {1, 3, 0, 2}));

    opennow::SortLibraryIndices(indices, games, LibrarySortMode::Title);
    assert((indices == std::vector<size_t> {1, 2, 3, 0}));

    opennow::SortLibraryIndices(indices, games, LibrarySortMode::Store);
    assert((indices == std::vector<size_t> {1, 3, 2, 0}));
    return 0;
}
