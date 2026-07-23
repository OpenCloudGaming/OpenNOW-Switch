#pragma once

#include "home_shortcut_policy.hpp"

#include <optional>
#include <string>

namespace opennow::shortcut
{

struct CreateResult
{
    bool success = false;
    bool used_game_cover = false;
    std::string title;
    std::string nro_path;
    std::string manifest_path;
    std::string error;
};

void SetExecutablePath(std::string path);
const std::string& ExecutablePath();
std::optional<LaunchRequest> ReadLaunchRequest(int argc, char* argv[]);
CreateResult CreateGameShortcut(LaunchRequest request);
bool StartForwarderInstaller(
    const std::string& nro_path, const std::string& title,
    std::string& error);

} // namespace opennow::shortcut
