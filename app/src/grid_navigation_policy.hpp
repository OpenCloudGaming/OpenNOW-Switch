#pragma once

#include <algorithm>
#include <cstddef>

namespace opennow
{

constexpr size_t GridTargetColumn(size_t source_column, size_t target_row_size)
{
    return target_row_size == 0 ? 0 : std::min(source_column, target_row_size - 1);
}

} // namespace opennow
