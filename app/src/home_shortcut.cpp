#include "home_shortcut.hpp"

#include "app_version.hpp"
#include "atomic_file_replace.hpp"
#include "cover_image_cache.hpp"
#include "nro_shortcut_policy.hpp"

#include <borealis/extern/nanovg/stb_image.h>
#include <stb_image_write.h>

#ifdef __SWITCH__
#include <switch.h>
#include <sys/stat.h>
#endif

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <fstream>
#include <iterator>
#include <limits>
#include <sstream>
#include <utility>
#include <vector>

namespace opennow::shortcut
{
namespace
{

constexpr const char* kDefaultExecutable =
    "sdmc:/switch/SwitchNOW/SwitchNOW.nro";
constexpr const char* kShortcutTemplate =
    "romfs:/shortcut/OpenNOWShortcut.nro";
constexpr const char* kDefaultIcon =
    "romfs:/icon/icon.jpg";
constexpr const char* kInstallerRomfsPath =
    "romfs:/shortcut/OpenNOWForwarderInstaller.nro";
constexpr const char* kInstallerSdPath =
    "sdmc:/switch/SwitchNOW/.opennow/OpenNOWForwarderInstaller.nro";
constexpr const char* kInstallerRequestPath =
    "sdmc:/switch/SwitchNOW/forwarder_request.json";

std::string g_executable_path = kDefaultExecutable;

bool ReadBinaryFile(const std::string& path, std::vector<std::uint8_t>& data)
{
    std::ifstream stream(path, std::ios::binary);
    if (!stream.is_open())
        return false;
    data.assign(
        std::istreambuf_iterator<char>(stream),
        std::istreambuf_iterator<char>());
    return !data.empty();
}

bool WriteBinaryFileAtomically(
    const std::string& path, const std::vector<std::uint8_t>& data)
{
    const std::string temporary = path + ".tmp";
    {
        std::ofstream stream(temporary, std::ios::binary | std::ios::trunc);
        if (!stream.is_open())
            return false;
        stream.write(
            reinterpret_cast<const char*>(data.data()),
            static_cast<std::streamsize>(data.size()));
        if (!stream.good())
            return false;
    }
    return storage::ReplaceWithTemporaryFile(temporary, path);
}

bool WriteTextFileAtomically(const std::string& path, const std::string& data)
{
    return WriteBinaryFileAtomically(
        path, std::vector<std::uint8_t>(data.begin(), data.end()));
}

std::string ReadTextFile(const std::string& path)
{
    std::ifstream stream(path, std::ios::binary);
    if (!stream.is_open())
        return {};
    return {
        std::istreambuf_iterator<char>(stream),
        std::istreambuf_iterator<char>()};
}

void EnsureShortcutDirectories(const std::string& game_directory)
{
#ifdef __SWITCH__
    mkdir("sdmc:/switch", 0777);
    mkdir("sdmc:/switch/OpenNOW Shortcuts", 0777);
    mkdir(game_directory.c_str(), 0777);
#else
    (void)game_directory;
#endif
}

std::string JsonEscape(std::string_view value)
{
    std::string escaped;
    escaped.reserve(value.size() + 8);
    for (unsigned char ch : value)
    {
        switch (ch)
        {
            case '"': escaped += "\\\""; break;
            case '\\': escaped += "\\\\"; break;
            case '\b': escaped += "\\b"; break;
            case '\f': escaped += "\\f"; break;
            case '\n': escaped += "\\n"; break;
            case '\r': escaped += "\\r"; break;
            case '\t': escaped += "\\t"; break;
            default:
                if (ch >= 0x20)
                    escaped.push_back(static_cast<char>(ch));
                break;
        }
    }
    return escaped;
}

void JpegWriteCallback(void* context, void* data, int size)
{
    if (!context || !data || size <= 0)
        return;
    auto& output = *static_cast<std::vector<std::uint8_t>*>(context);
    const auto* bytes = static_cast<const std::uint8_t*>(data);
    output.insert(output.end(), bytes, bytes + size);
}

std::vector<std::uint8_t> MakeSquareJpeg(const std::string& encoded)
{
    if (encoded.empty() ||
        encoded.size() > static_cast<size_t>(std::numeric_limits<int>::max()))
        return {};

    int width = 0;
    int height = 0;
    int channels = 0;
    unsigned char* decoded = stbi_load_from_memory(
        reinterpret_cast<const unsigned char*>(encoded.data()),
        static_cast<int>(encoded.size()), &width, &height, &channels, 3);
    if (!decoded || width <= 0 || height <= 0)
    {
        stbi_image_free(decoded);
        return {};
    }

    constexpr int kOutputSize = 256;
    const int crop_size = std::min(width, height);
    const int crop_x = (width - crop_size) / 2;
    const int crop_y = (height - crop_size) / 2;
    std::vector<std::uint8_t> square(kOutputSize * kOutputSize * 3);
    for (int y = 0; y < kOutputSize; ++y)
    {
        const int source_y = crop_y +
            std::min(crop_size - 1, y * crop_size / kOutputSize);
        for (int x = 0; x < kOutputSize; ++x)
        {
            const int source_x = crop_x +
                std::min(crop_size - 1, x * crop_size / kOutputSize);
            const size_t source =
                (static_cast<size_t>(source_y) * width + source_x) * 3;
            const size_t destination =
                (static_cast<size_t>(y) * kOutputSize + x) * 3;
            std::copy_n(decoded + source, 3, square.begin() + destination);
        }
    }
    stbi_image_free(decoded);

    for (int quality : {92, 86, 78, 68})
    {
        std::vector<std::uint8_t> jpeg;
        if (stbi_write_jpg_to_func(
                JpegWriteCallback, &jpeg, kOutputSize, kOutputSize, 3,
                square.data(), quality) &&
            !jpeg.empty() && jpeg.size() < 0x20000)
            return jpeg;
    }
    return {};
}

std::optional<LaunchRequest> ReadManifestArgument(
    const std::vector<std::string>& arguments)
{
    for (const std::string& argument : arguments)
    {
        if (argument.size() < 9 ||
            argument.substr(argument.size() - 8) != ".opennow")
            continue;
        const std::string manifest = ReadTextFile(argument);
        if (manifest.empty())
            return std::nullopt;
        return Parse(manifest);
    }
    return std::nullopt;
}

} // namespace

void SetExecutablePath(std::string path)
{
    if (!path.empty() && path.find('\n') == std::string::npos &&
        path.size() <= 1024)
        g_executable_path = std::move(path);
}

const std::string& ExecutablePath()
{
    return g_executable_path;
}

std::optional<LaunchRequest> ReadLaunchRequest(int argc, char* argv[])
{
    std::vector<std::string> arguments;
    arguments.reserve(argc > 1 ? static_cast<size_t>(argc - 1) : 0);
    for (int i = 1; i < argc; ++i)
    {
        if (argv[i])
            arguments.emplace_back(argv[i]);
    }
    if (auto manifest = ReadManifestArgument(arguments))
        return manifest;
    return ParseArguments(arguments);
}

CreateResult CreateGameShortcut(LaunchRequest request)
{
    CreateResult result;
    result.title = request.title;
    request.executable_path = g_executable_path;
    if (!IsValid(request))
    {
        result.error = "The selected game does not have valid shortcut metadata.";
        return result;
    }

    const std::string stem = SafeFileStem(request.title, request.launch_app_id);
    const std::string directory =
        "sdmc:/switch/OpenNOW Shortcuts/" + stem + " [" +
        request.launch_app_id + "]";
    EnsureShortcutDirectories(directory);

    std::vector<std::uint8_t> template_nro;
    if (!ReadBinaryFile(kShortcutTemplate, template_nro))
    {
        result.error = "The OpenNOW shortcut launcher is missing from RomFS.";
        return result;
    }

    std::vector<std::uint8_t> icon;
    try
    {
        icon = MakeSquareJpeg(LoadCachedImageData(request.image_url));
        result.used_game_cover = !icon.empty();
    }
    catch (...)
    {
        icon.clear();
    }
    if (icon.empty() && !ReadBinaryFile(kDefaultIcon, icon))
    {
        result.error = "Could not load the OpenNOW shortcut icon.";
        return result;
    }

    std::vector<std::uint8_t> shortcut_nro;
    if (!BuildShortcutNro(template_nro, icon, request.title, shortcut_nro))
    {
        result.error = "Could not personalize the shortcut launcher.";
        return result;
    }

    result.nro_path = directory + "/" + stem + ".nro";
    result.manifest_path = directory + "/" + stem + ".opennow";
    if (!WriteBinaryFileAtomically(result.nro_path, shortcut_nro))
    {
        result.error = "Could not write the shortcut NRO to the SD card.";
        return result;
    }
    if (!WriteTextFileAtomically(result.manifest_path, Serialize(request)))
    {
        result.error = "Could not write the shortcut launch manifest.";
        return result;
    }

    result.success = true;
    return result;
}

bool StartForwarderInstaller(
    const std::string& nro_path, const std::string& title,
    std::string& error)
{
#ifdef __SWITCH__
    if (appletGetAppletType() != AppletType_Application)
    {
        error =
            "Horizon installation requires full-memory/application mode. "
            "Hold R while opening an installed title, then launch OpenNOW.";
        return false;
    }
    if (nro_path.empty() || nro_path.size() >= FS_MAX_PATH || title.empty() ||
        nro_path.find('\n') != std::string::npos)
    {
        error = "The forwarder target is invalid.";
        return false;
    }

    mkdir("sdmc:/switch/SwitchNOW/.opennow", 0777);
    std::vector<std::uint8_t> installer;
    if (!ReadBinaryFile(kInstallerRomfsPath, installer))
    {
        error = "The OpenNOW Forwarder Installer is missing from RomFS.";
        return false;
    }
    if (!WriteBinaryFileAtomically(kInstallerSdPath, installer))
    {
        error = "Could not prepare the OpenNOW Forwarder Installer on the SD card.";
        return false;
    }

    const std::string arguments = "\"" + nro_path + "\"";
    const std::string request =
        "{\n"
        "  \"version\": 1,\n"
        "  \"name\": \"" + JsonEscape(title) + "\",\n"
        "  \"author\": \"OpenCloudGaming\",\n"
        "  \"displayVersion\": \"" + std::string(kAppVersion) + "\",\n"
        "  \"nroPath\": \"" + JsonEscape(nro_path) + "\",\n"
        "  \"args\": \"" + JsonEscape(arguments) + "\"\n"
        "}\n";
    if (!WriteTextFileAtomically(kInstallerRequestPath, request))
    {
        error = "Could not write the forwarder installation request.";
        return false;
    }

    envSetNextLoad(kInstallerSdPath, kInstallerSdPath);
    brls::Application::quit();
    return true;
#else
    (void)nro_path;
    (void)title;
    error = "Horizon forwarder installation is only available on Nintendo Switch.";
    return false;
#endif
}

} // namespace opennow::shortcut
