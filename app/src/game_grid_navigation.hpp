#pragma once

#include "grid_navigation_policy.hpp"

#include <borealis.hpp>

#include <cstddef>
#include <vector>

namespace opennow
{

inline void WireVerticalGridNavigation(const std::vector<std::vector<brls::View*>>& rows)
{
    for (size_t row = 0; row < rows.size(); ++row)
    {
        for (size_t column = 0; column < rows[row].size(); ++column)
        {
            brls::View* card = rows[row][column];
            if (!card)
                continue;

            // Persistent toolbar buttons are rewired every time the grid is
            // rebuilt. Point edge routes back to the live view first so no
            // route can keep a pointer to a deleted card from an older grid.
            card->setCustomNavigationRoute(brls::FocusDirection::UP, card);
            card->setCustomNavigationRoute(brls::FocusDirection::DOWN, card);
            if (row > 0 && !rows[row - 1].empty())
                card->setCustomNavigationRoute(
                    brls::FocusDirection::UP,
                    rows[row - 1][GridTargetColumn(column, rows[row - 1].size())]);
            if (row + 1 < rows.size() && !rows[row + 1].empty())
                card->setCustomNavigationRoute(
                    brls::FocusDirection::DOWN,
                    rows[row + 1][GridTargetColumn(column, rows[row + 1].size())]);
        }
    }
}

} // namespace opennow
