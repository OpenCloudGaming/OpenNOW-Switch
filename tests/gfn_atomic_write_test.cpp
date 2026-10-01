#include "gfn/internal.hpp"

#include <cassert>
#include <csignal>
#include <cstdlib>
#include <filesystem>
#include <stdexcept>
#include <sys/resource.h>

namespace opennow
{
bool StreamDiagnosticsEnabled() { return false; }
}

int main()
{
    using namespace opennow::gfn::detail;
    const auto original_directory = std::filesystem::current_path();
    std::string pattern = (std::filesystem::temp_directory_path() / "opennow-write-XXXXXX").string();
    const char* directory = mkdtemp(pattern.data());
    assert(directory);
    const std::filesystem::path test_directory(directory);
    std::filesystem::current_path(test_directory);
    std::filesystem::create_directories(GetAppHome());
    const std::string path = GetAppHome() + "/synthetic-store";
    WriteTextFileAtomically(path, "known-good");
    WriteTextFileAtomically(path, "latest-good");
    assert(ReadTextFile(path + ".bak") == "known-good");
    rlimit original_limit {};
    assert(getrlimit(RLIMIT_FSIZE, &original_limit) == 0);
    rlimit write_limit = original_limit;
    write_limit.rlim_cur = 0;
    const auto original_handler = std::signal(SIGXFSZ, SIG_IGN);
    assert(original_handler != SIG_ERR);
    assert(setrlimit(RLIMIT_FSIZE, &write_limit) == 0);
    bool failed = false;
    try
    {
        WriteTextFileAtomically(path, "incomplete");
    }
    catch (const std::runtime_error&)
    {
        failed = true;
    }
    assert(setrlimit(RLIMIT_FSIZE, &original_limit) == 0);
    assert(std::signal(SIGXFSZ, original_handler) != SIG_ERR);
    assert(failed);
    assert(ReadTextFile(path) == "latest-good");
    assert(ReadTextFile(path + ".bak") == "known-good");
    assert(!std::filesystem::exists(path + ".tmp"));
    WriteTextFileAtomically(path, "recovered");
    assert(ReadTextFile(path) == "recovered");
    assert(ReadTextFile(path + ".bak") == "latest-good");
    std::filesystem::current_path(original_directory);
    std::filesystem::remove_all(test_directory);
}
