#pragma once

#include "models.hpp"

#include <algorithm>
#include <cctype>
#include <string>
#include <vector>

namespace opennow
{

enum class LibrarySortMode
{
    LastPlayed = 0,
    LastAdded = 1,
    Title = 2,
    Store = 3,
};

inline std::string LibrarySortKey(std::string value)
{
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    return value;
}

inline std::string LibrarySortStore(const GameInfo& game)
{
    if (!game.available_stores.empty())
        return game.available_stores.front();
    return game.publisher;
}

inline void SortLibraryIndices(
    std::vector<size_t>& indices,
    const std::vector<GameInfo>& games,
    LibrarySortMode mode)
{
    // NVIDIA's library feed order is the only trustworthy "last added"
    // signal. Keep it untouched, matching desktop OpenNOW behavior.
    if (mode == LibrarySortMode::LastAdded)
        return;

    std::stable_sort(indices.begin(), indices.end(), [&](size_t left_index, size_t right_index) {
        const GameInfo& left = games[left_index];
        const GameInfo& right = games[right_index];
        const std::string left_title = LibrarySortKey(left.title);
        const std::string right_title = LibrarySortKey(right.title);

        if (mode == LibrarySortMode::LastPlayed)
        {
            if (left.last_played.empty() != right.last_played.empty())
                return !left.last_played.empty();
            if (left.last_played != right.last_played)
                return left.last_played > right.last_played;
        }
        else if (mode == LibrarySortMode::Store)
        {
            const std::string left_store = LibrarySortKey(LibrarySortStore(left));
            const std::string right_store = LibrarySortKey(LibrarySortStore(right));
            if (left_store != right_store)
                return left_store < right_store;
        }

        if (left_title != right_title)
            return left_title < right_title;
        return left.id < right.id;
    });
}

} // namespace opennow
