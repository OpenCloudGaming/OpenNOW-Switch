#pragma once

#include "models.hpp"

#include <string>
#include <unordered_map>
#include <vector>

namespace opennow
{

struct PlayHistory
{
    std::unordered_map<std::string, std::string> by_id;
    std::unordered_map<std::string, std::string> by_title;
};

std::string CurrentUtcIsoTimestamp();
PlayHistory LoadPlayHistory();
bool RecordGamePlayed(const std::string& game_id, const std::string& title,
                      const std::string& timestamp = {});
void ApplyPlayHistory(std::vector<GameInfo>& games, const PlayHistory& history);

} // namespace opennow
