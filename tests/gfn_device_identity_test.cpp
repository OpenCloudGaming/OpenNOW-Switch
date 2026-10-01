#include "gfn/internal.hpp"
#include "device_identity_policy.hpp"

#include <array>
#include <barrier>
#include <cassert>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fcntl.h>
#include <stdexcept>
#include <thread>

namespace
{
bool fail_install = false;
std::string install_source;
std::string install_destination;
}

extern "C" int rename(const char* source, const char* destination) noexcept
{
    if (fail_install && std::strcmp(source, install_source.c_str()) == 0 &&
        std::strcmp(destination, install_destination.c_str()) == 0)
    {
        errno = EIO;
        return -1;
    }
    return renameat(AT_FDCWD, source, AT_FDCWD, destination);
}

namespace opennow
{
bool StreamDiagnosticsEnabled() { return false; }
}

int main()
{
    using namespace opennow::gfn::detail;
    const auto original_directory = std::filesystem::current_path();
    std::string pattern = (std::filesystem::temp_directory_path() / "opennow-device-XXXXXX").string();
    const char* directory = mkdtemp(pattern.data());
    assert(directory);
    const std::filesystem::path test_directory(directory);
    std::filesystem::current_path(test_directory);
    std::filesystem::create_directories(GetAppHome());
    const std::string path = GetAppHome() + "/device_id.txt";
    install_source = path + ".tmp";
    install_destination = path;

    std::array<std::string, 32> identities;
    std::array<bool, 32> failed {};
    std::barrier start(static_cast<std::ptrdiff_t>(identities.size()));
    std::array<std::thread, 32> workers;
    for (std::size_t index = 0; index < workers.size(); ++index)
        workers[index] = std::thread([&, index] {
            start.arrive_and_wait();
            try
            {
                identities[index] = GenerateDeviceId();
            }
            catch (...)
            {
                failed[index] = true;
            }
        });
    for (auto& worker : workers)
        worker.join();
    for (std::size_t index = 0; index < identities.size(); ++index)
    {
        assert(!failed[index]);
        assert(opennow::device_identity::IsUsableStoredDeviceId(identities[index]));
        assert(identities[index] == identities.front());
    }
    assert(ReadTextFile(path) == identities.front());
    WriteTextFileAtomically(path, identities.front());
    for (int attempt = 0; attempt < 3; ++attempt)
    {
        std::filesystem::remove(path);
        assert(GenerateDeviceId() == identities.front());
        assert(ReadTextFile(path) == identities.front());
        assert(ReadTextFile(path + ".bak") == identities.front());
        WriteTextFileAtomically(path, " ");
        assert(GenerateDeviceId() == identities.front());
        assert(ReadTextFile(path) == identities.front());
        assert(ReadTextFile(path + ".bak") == identities.front());
    }
    for (const bool missing_primary : {true, false})
    {
        if (missing_primary)
            std::filesystem::remove(path);
        else
            WriteTextFileAtomically(path, " ");
        std::filesystem::create_directory(path + ".tmp");
        bool write_failed = false;
        try
        {
            GenerateDeviceId();
        }
        catch (const std::runtime_error&)
        {
            write_failed = true;
        }
        assert(write_failed);
        assert(ReadTextFile(path + ".bak") == identities.front());
        std::filesystem::remove(path + ".tmp");

        fail_install = true;
        bool install_failed = false;
        try
        {
            GenerateDeviceId();
        }
        catch (const std::runtime_error&)
        {
            install_failed = true;
        }
        fail_install = false;
        assert(install_failed);
        assert(ReadTextFile(path + ".bak") == identities.front());
        assert(!std::filesystem::exists(path + ".tmp"));
        assert(GenerateDeviceId() == identities.front());
        assert(ReadTextFile(path + ".bak") == identities.front());
    }
    WriteTextFileAtomically(path, "legacy-custom-device-id");
    assert(GenerateDeviceId() == "legacy-custom-device-id");
    std::filesystem::current_path(original_directory);
    std::filesystem::remove_all(test_directory);
}
