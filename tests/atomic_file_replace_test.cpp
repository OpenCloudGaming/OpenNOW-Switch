#include "atomic_file_replace.hpp"

#include <cassert>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>

namespace
{
std::string ReadFile(const std::filesystem::path& path)
{
    std::ifstream stream(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>()};
}
}

int main()
{
    const auto directory = std::filesystem::temp_directory_path() / "opennow-atomic-save-test";
    const auto destination = directory / "stream_settings.json";
    const auto temporary = directory / "stream_settings.json.tmp";
    std::filesystem::create_directories(directory);

    {
        std::ofstream stream(destination, std::ios::binary | std::ios::trunc);
        stream << "old settings";
    }
    {
        std::ofstream stream(temporary, std::ios::binary | std::ios::trunc);
        stream << "new settings";
    }

    assert(opennow::storage::ReplaceWithTemporaryFile(temporary.string(), destination.string()));
    assert(ReadFile(destination) == "new settings");
    assert(!std::filesystem::exists(temporary));
    assert(!std::filesystem::exists(destination.string() + ".bak"));

    std::filesystem::remove_all(directory);
    return 0;
}
