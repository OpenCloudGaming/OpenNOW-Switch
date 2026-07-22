#include "internal.hpp"

#include "../stream_diagnostics.hpp"

#ifdef __SWITCH__
#include <switch.h>
#include <sys/stat.h>
#endif

#include <algorithm>
#include <chrono>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <random>
#include <stdexcept>

namespace opennow::gfn::detail
{
namespace
{
constexpr const char* kDefaultProviderId = "PDiAhv2kJTFeQ7WOPqiQ2tRZ7lGhR2X11dXvM4TZSxg";
}

std::string Trim(const std::string& value)
{
    size_t begin = 0;
    while (begin < value.size() && std::isspace(static_cast<unsigned char>(value[begin])))
        ++begin;

    size_t end = value.size();
    while (end > begin && std::isspace(static_cast<unsigned char>(value[end - 1])))
        --end;

    return value.substr(begin, end - begin);
}

std::int64_t NowMs()
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}
std::string GetAppHome()
{
    return "sdmc:/switch/SwitchNOW";
}
void EnsureAppHome()
{
#ifdef __SWITCH__
    mkdir("sdmc:/switch", 0777);
    mkdir("sdmc:/switch/SwitchNOW", 0777);
#endif
}

JsonPtr LoadJson(const std::string& body)
{
    json_error_t error {};
    JsonPtr root(json_loads(body.c_str(), 0, &error), &json_decref);
    if (!root)
    {
        throw std::runtime_error(
            "JSON parse failed at line " + std::to_string(error.line) + ": " + error.text);
    }

    return root;
}

std::string JsonString(json_t* value)
{
    if (json_is_string(value))
    {
        const char* raw = json_string_value(value);
        return raw ? raw : "";
    }

    if (json_is_integer(value))
        return std::to_string(json_integer_value(value));

    return "";
}

std::string GetString(json_t* object, const char* key)
{
    if (!object || !json_is_object(object))
        return "";

    return JsonString(json_object_get(object, key));
}

bool GetBool(json_t* object, const char* key, bool fallback)
{
    if (!object || !json_is_object(object))
        return fallback;

    json_t* value = json_object_get(object, key);
    if (json_is_boolean(value))
        return json_boolean_value(value);

    return fallback;
}

int GetInteger(json_t* object, const char* key, int fallback)
{
    if (!object || !json_is_object(object))
        return fallback;

    json_t* value = json_object_get(object, key);
    if (!json_is_integer(value))
        return fallback;

    return static_cast<int>(json_integer_value(value));
}
std::string EnsureTrailingSlash(std::string value)
{
    if (value.empty() || value.back() == '/')
        return value;

    value.push_back('/');
    return value;
}

LoginProvider DefaultProvider()
{
    LoginProvider provider;
    provider.idp_id                = kDefaultProviderId;
    provider.code                  = "NVIDIA";
    provider.display_name          = "NVIDIA";
    provider.streaming_service_url = "https://prod.cloudmatchbeta.nvidiagrid.net/";
    provider.priority              = 0;
    return provider;
}

std::vector<unsigned char> GenerateRandomBytes(size_t length)
{
    std::vector<unsigned char> bytes(length);

#ifdef __SWITCH__
    randomGet(bytes.data(), bytes.size());
#else
    std::random_device device;
    for (auto& byte : bytes)
        byte = static_cast<unsigned char>(device());
#endif

    return bytes;
}

std::string HexEncode(const unsigned char* data, size_t length)
{
    static constexpr char kDigits[] = "0123456789abcdef";
    std::string output;
    output.reserve(length * 2);

    for (size_t index = 0; index < length; ++index)
    {
        output.push_back(kDigits[(data[index] >> 4) & 0x0F]);
        output.push_back(kDigits[data[index] & 0x0F]);
    }

    return output;
}

std::string GenerateDeviceId()
{
    const std::string path = GetAppHome() + "/device_id.txt";
    std::string stored = Trim(ReadTextFile(path));
    if (!stored.empty())
        return stored;

    const auto bytes = GenerateRandomBytes(32);
    stored           = HexEncode(bytes.data(), bytes.size());
    WriteTextFileAtomically(path, stored);
    return stored;
}

std::string UrlEncode(const std::string& input, bool plus_for_space)
{
    static constexpr char kDigits[] = "0123456789ABCDEF";

    std::string output;
    output.reserve(input.size() * 3);

    for (unsigned char ch : input)
    {
        if ((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9') ||
            ch == '-' || ch == '_' || ch == '.' || ch == '~')
        {
            output.push_back(static_cast<char>(ch));
        }
        else if (plus_for_space && ch == ' ')
        {
            output.push_back('+');
        }
        else
        {
            output.push_back('%');
            output.push_back(kDigits[(ch >> 4) & 0x0F]);
            output.push_back(kDigits[ch & 0x0F]);
        }
    }

    return output;
}

std::string ReadTextFile(const std::string& path)
{
    std::ifstream stream(path, std::ios::binary);
    if (!stream.is_open())
        return "";

    return std::string(
        std::istreambuf_iterator<char>(stream),
        std::istreambuf_iterator<char>());
}

namespace
{

void WriteTextFile(const std::string& path, const std::string& content)
{
    EnsureAppHome();

    std::ofstream stream(path, std::ios::binary | std::ios::trunc);
    if (!stream.is_open())
        throw std::runtime_error("Unable to write " + path);

    stream.write(content.data(), static_cast<std::streamsize>(content.size()));
}

} // namespace

void WriteTextFileAtomically(const std::string& path, const std::string& content)
{
    const std::string temporary = path + ".tmp";
    const std::string backup = path + ".bak";
    WriteTextFile(temporary, content);

    std::remove(backup.c_str());
    const bool had_original = std::rename(path.c_str(), backup.c_str()) == 0;
    if (std::rename(temporary.c_str(), path.c_str()) == 0)
        return;

    if (had_original)
        std::rename(backup.c_str(), path.c_str());
    std::remove(temporary.c_str());
    throw std::runtime_error("Unable to replace " + path);
}

void WriteJsonToFile(const std::string& path, json_t* root)
{
    char* dump = json_dumps(root, JSON_INDENT(2));
    if (!dump)
        throw std::runtime_error("Failed to serialize JSON");

    std::unique_ptr<char, decltype(&std::free)> output(dump, &std::free);
    WriteTextFileAtomically(path, output.get());
}

void AppendAuthLog(const std::string& line)
{
    if (!StreamDiagnosticsEnabled())
        return;
    EnsureAppHome();

    std::ofstream stream(GetAppHome() + "/auth.log", std::ios::app);
    if (!stream.is_open())
        return;

    stream << '[' << NowMs() << "] " << line << '\n';
}
std::string Lowercase(std::string value)
{
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    return value;
}

namespace
{

bool IsSensitiveJsonKey(const std::string& key)
{
    const std::string lower = Lowercase(key);
    return lower.find("token") != std::string::npos ||
           lower.find("authorization") != std::string::npos ||
           lower.find("credential") != std::string::npos ||
           lower.find("password") != std::string::npos ||
           lower.find("secret") != std::string::npos ||
           lower.find("jwt") != std::string::npos ||
           lower.find("email") != std::string::npos ||
           lower == "user_id" ||
           lower == "userid" ||
           lower == "display_name" ||
           lower == "displayname" ||
           lower.find("avatar") != std::string::npos;
}

void RedactJsonInPlace(json_t* value)
{
    if (!value)
        return;

    if (json_is_object(value))
    {
        void* iter = json_object_iter(value);
        while (iter)
        {
            const char* key = json_object_iter_key(iter);
            json_t* child = json_object_iter_value(iter);
            void* next = json_object_iter_next(value, iter);

            if (key && IsSensitiveJsonKey(key))
                json_object_set_new(value, key, json_string("<redacted>"));
            else
                RedactJsonInPlace(child);

            iter = next;
        }
        return;
    }

    if (json_is_array(value))
    {
        size_t index;
        json_t* child;
        json_array_foreach(value, index, child)
            RedactJsonInPlace(child);
    }
}

} // namespace

std::string JsonForTrace(const std::string& body)
{
    if (body.empty())
        return "<empty>";

    json_error_t error {};
    JsonPtr root(json_loads(body.c_str(), 0, &error), &json_decref);
    if (!root)
        return body;

    RedactJsonInPlace(root.get());
    char* dump = json_dumps(root.get(), JSON_INDENT(2));
    if (!dump)
        return body;

    std::unique_ptr<char, decltype(&std::free)> output(dump, &std::free);
    return output.get();
}
std::string DumpJson(json_t* value)
{
    char* raw = json_dumps(value, JSON_COMPACT);
    if (!raw)
        throw std::runtime_error("Unable to encode the NVIDIA login request");
    std::unique_ptr<char, decltype(&std::free)> output(raw, &std::free);
    return output.get();
}
std::string ResolveSessionJwt(const AuthSession& session)
{
    // CloudMatch expects NVIDIA's signed ID token. OAuth access tokens may be opaque and
    // are rejected as GFNJWT before the session request is parsed.
    return session.tokens.id_token.empty() ? session.tokens.access_token : session.tokens.id_token;
}

} // namespace opennow::gfn::detail
