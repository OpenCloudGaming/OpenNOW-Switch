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
    std::string pattern =
        (std::filesystem::temp_directory_path() / "opennow-queue-settings-XXXXXX").string();
    char* directory = mkdtemp(pattern.data());
    assert(directory);
    home = directory;
    assert(opennow::LoadStreamSettings().queue_notify_threshold == 10);
    for (int threshold : {5, 10, 20, 50}) {
        auto settings = opennow::LoadStreamSettings();
        settings.queue_notify_threshold = threshold;
        assert(opennow::SaveStreamSettings(settings));
        assert(opennow::LoadStreamSettings().queue_notify_threshold == threshold);
    }
    for (const std::string value : {"0", "-1", "15", "2147483648", "null", "\"5\""}) {
        {
            std::ofstream file(home + "/stream_settings.json");
            file << "{\"queue_notify_threshold\":" << value << "}";
            file.close();
            assert(file.good());
        }
        assert(opennow::LoadStreamSettings().queue_notify_threshold == 10);
    }
    std::filesystem::remove_all(home);
}
