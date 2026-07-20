#include "play_history.hpp"
#include "library_sort.hpp"

#include <cassert>
#include <vector>

int main()
{
    using namespace opennow;
    std::vector<GameInfo> games(3);
    games[0].id = "old";
    games[0].title = "Zulu";
    games[0].last_played = "2025-01-01T00:00:00Z";
    games[1].uuid = "local-new";
    games[1].title = "Alpha";
    games[2].id = "never";
    games[2].title = "Beta";

    PlayHistory history;
    history.by_id["local-new"] = "2026-07-14T12:00:00Z";
    ApplyPlayHistory(games, history);
    assert(games[1].last_played == "2026-07-14T12:00:00Z");

    std::vector<size_t> indices {0, 1, 2};
    SortLibraryIndices(indices, games, LibrarySortMode::LastPlayed);
    assert((indices == std::vector<size_t> {1, 0, 2}));
    return 0;
}
