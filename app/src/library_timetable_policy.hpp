#pragma once

#include <array>
#include <chrono>
#include <optional>
#include <string>
#include <string_view>

namespace opennow::library
{

enum class DateBucket { Today, ThisWeek, Earlier, Unplayed, Unknown };

struct TimestampPresentation
{
    DateBucket bucket = DateBucket::Unknown;
    std::string label = "?";
};

inline TimestampPresentation PresentTimestamp(
    std::string_view value, std::chrono::system_clock::time_point now)
{
    using namespace std::chrono;
    if (value.empty())
        return {DateBucket::Unplayed, "--"};
    if (value.size() < 20 || value[4] != '-' || value[7] != '-' ||
        value[10] != 'T' || value[13] != ':' || value[16] != ':' || value.back() != 'Z')
        return {};
    auto number = [value](size_t offset, size_t count) -> std::optional<unsigned> {
        unsigned result = 0;
        for (size_t i = offset; i < offset + count; ++i)
        {
            if (value[i] < '0' || value[i] > '9')
                return std::nullopt;
            result = result * 10 + static_cast<unsigned>(value[i] - '0');
        }
        return result;
    };
    const auto y = number(0, 4);
    const auto m = number(5, 2);
    const auto d = number(8, 2);
    const auto h = number(11, 2);
    const auto min = number(14, 2);
    const auto sec = number(17, 2);
    if (!y || *y == 0 || !m || !d || !h || !min || !sec || *h > 23 || *min > 59 || *sec > 59)
        return {};
    const year_month_day date{year{static_cast<int>(*y)}, month{*m}, day{*d}};
    if (!date.ok())
        return {};
    unsigned fraction = 0;
    if (value.size() != 20)
    {
        if (value.size() < 22 || value.size() > 30 || value[19] != '.')
            return {};
        const size_t digits = value.size() - 21;
        const auto parsed = number(20, digits);
        if (!parsed)
            return {};
        fraction = *parsed;
        for (size_t i = digits; i < 9; ++i)
            fraction *= 10;
    }
    const sys_days played_day{date};
    const auto played = played_day + hours{*h} + minutes{*min} + seconds{*sec};
    const auto now_seconds = floor<seconds>(now);
    if (played > now_seconds || (played == now_seconds &&
            nanoseconds{fraction} > now - now_seconds))
        return {};
    const auto today = floor<days>(now);
    if (played_day == today)
        return {DateBucket::Today, std::string(value.substr(11, 5))};
    const auto week_start = today - days{weekday{today}.iso_encoding() - 1};
    if (played_day >= week_start)
    {
        constexpr std::array<const char*, 7> days_of_week {
            "Mon", "Tue", "Wed", "Thu", "Fri", "Sat", "Sun"};
        return {DateBucket::ThisWeek, days_of_week[weekday{played_day}.iso_encoding() - 1]};
    }
    return {DateBucket::Earlier, std::string(value.substr(0, 10))};
}

inline const char* BucketLabel(DateBucket bucket)
{
    switch (bucket)
    {
        case DateBucket::Today: return "Today (UTC)";
        case DateBucket::ThisWeek: return "This week (UTC)";
        case DateBucket::Earlier: return "Earlier (UTC)";
        case DateBucket::Unplayed: return "Unplayed";
        case DateBucket::Unknown: return "Unknown play time";
    }
    return "Unknown play time";
}

}
