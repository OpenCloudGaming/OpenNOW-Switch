#include "app_paths.hpp"

#include <cerrno>
#include <cstdio>
#include <sys/stat.h>

namespace opennow
{
namespace
{

bool DirectoryExists(const std::string& path)
{
    struct stat info {};
    return stat(path.c_str(), &info) == 0 && S_ISDIR(info.st_mode);
}

} // namespace

const std::string& AppHomePath()
{
    static const std::string path = "sdmc:/switch/SwitchNOW";
    return path;
}

const std::string& LegacyAppHomePath()
{
    static const std::string path = "sdmc:/switch/OpenNOWSwitch";
    return path;
}

void PrepareAppStorage()
{
#ifdef __SWITCH__
    mkdir("sdmc:/switch", 0777);

    if (!DirectoryExists(AppHomePath()) && DirectoryExists(LegacyAppHomePath()))
    {
        // Preserve saved sessions, credentials, settings, history and cache
        // when upgrading from pre-1.0 builds.
        std::rename(LegacyAppHomePath().c_str(), AppHomePath().c_str());
    }

    mkdir(AppHomePath().c_str(), 0777);
#endif
}

} // namespace opennow
