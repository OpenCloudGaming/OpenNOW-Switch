#include "app_paths.hpp"
#include "stream_settings.hpp"

#include <cassert>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>

namespace
{
std::string home;
}

namespace opennow
{
const std::string& AppHomePath() { return home; }
void PrepareAppStorage() { std::filesystem::create_directories(home); }
}

int main()
{
    std::string pattern = (std::filesystem::temp_directory_path() / "opennow-retired-proxy-XXXXXX").string();
    char* directory = mkdtemp(pattern.data());
    assert(directory);
    home = directory;
    for (const std::string suffix : {"", ".bak"})
    {
        std::filesystem::remove(home + "/stream_settings.json");
        std::ofstream file(home + "/stream_settings.json" + suffix);
        file << R"({"community_proxy_enabled":true,"community_proxy_url":"http://legacy-client:legacy-pass@opennow-proxy-tcp.zortos.me:3128","fps":30,"interface_language":"ru","queue_notify_threshold":20})";
        file.close();
        const auto settings = opennow::LoadStreamSettings();
        assert(!settings.community_proxy_enabled && settings.community_proxy_url.empty());
        assert(settings.fps == 30 && settings.interface_language == "ru" && settings.queue_notify_threshold == 20);
    }
    auto settings = opennow::LoadStreamSettings();
    settings.community_proxy_enabled = true;
    settings.community_proxy_url = "http://legacy-client:legacy-pass@217.76.50.166:3128";
    assert(opennow::SaveStreamSettings(settings));
    const auto saved = opennow::LoadStreamSettings();
    assert(!saved.community_proxy_enabled && saved.community_proxy_url.empty());
    assert(saved.fps == 30 && saved.interface_language == "ru" && saved.queue_notify_threshold == 20);
    std::filesystem::remove_all(home);
}
