#include "nte_credentials.hpp"

#include "atomic_file_replace.hpp"

#ifdef __SWITCH__
#include <sys/stat.h>
#endif

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <sstream>
#include <vector>

namespace opennow
{
namespace
{

std::string Trim(std::string value)
{
    const auto is_space = [](unsigned char ch) { return std::isspace(ch) != 0; };
    value.erase(value.begin(), std::find_if(value.begin(), value.end(),
        [&](unsigned char ch) { return !is_space(ch); }));
    value.erase(std::find_if(value.rbegin(), value.rend(),
        [&](unsigned char ch) { return !is_space(ch); }).base(), value.end());
    return value;
}

std::string Lower(std::string value)
{
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    return value;
}

bool SafeSingleLine(const std::string& value)
{
    return !value.empty() && value.find_first_of("\r\n") == std::string::npos;
}

void EnsureNteDirectory()
{
#ifdef __SWITCH__
    mkdir("sdmc:/switch", 0777);
    mkdir("sdmc:/switch/SwitchNOW", 0777);
    mkdir("sdmc:/switch/SwitchNOW/nte", 0777);
#endif
}

} // namespace

bool NteCredentials::valid() const
{
    return SafeSingleLine(email) && SafeSingleLine(password) &&
           email.find('@') != std::string::npos;
}

bool IsNevernessToEverness(const std::string& title)
{
    const std::string lower = Lower(title);
    return lower.find("neverness to everness") != std::string::npos ||
           lower == "nte" || lower.rfind("nte:", 0) == 0;
}

NteCredentials ParseNteCredentials(const std::string& text)
{
    NteCredentials credentials;
    std::vector<std::string> positional;
    std::istringstream input(text);
    std::string line;
    while (std::getline(input, line))
    {
        if (!line.empty() && line.back() == '\r')
            line.pop_back();
        line = Trim(line);
        if (line.empty() || line.front() == '#')
            continue;

        const size_t separator = line.find('=');
        if (separator == std::string::npos)
        {
            positional.push_back(line);
            continue;
        }

        const std::string key = Lower(Trim(line.substr(0, separator)));
        const std::string value = Trim(line.substr(separator + 1));
        if (key == "email" || key == "login" || key == "username")
            credentials.email = value;
        else if (key == "password")
            credentials.password = value;
    }

    if (credentials.email.empty() && !positional.empty())
        credentials.email = positional[0];
    if (credentials.password.empty() && positional.size() > 1)
        credentials.password = positional[1];
    if (!credentials.valid())
        return {};
    return credentials;
}

std::string SerializeNteCredentials(const NteCredentials& credentials)
{
    if (!credentials.valid())
        return {};
    return "email=" + credentials.email + "\npassword=" + credentials.password + "\n";
}

std::string NteCredentialsPath()
{
    return "sdmc:/switch/SwitchNOW/nte/nte.txt";
}

NteCredentials LoadNteCredentials()
{
    std::ifstream stream(NteCredentialsPath(), std::ios::binary);
    if (!stream.is_open())
        return {};
    const std::string text {
        std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>()};
    return ParseNteCredentials(text);
}

bool SaveNteCredentials(const NteCredentials& credentials)
{
    const std::string text = SerializeNteCredentials(credentials);
    if (text.empty())
        return false;

    EnsureNteDirectory();
    const std::string path = NteCredentialsPath();
    const std::string temporary = path + ".tmp";
    std::ofstream stream(temporary, std::ios::binary | std::ios::trunc);
    if (!stream.is_open())
        return false;
    stream.write(text.data(), static_cast<std::streamsize>(text.size()));
    stream.flush();
    const bool ok = stream.good();
    stream.close();
    if (!ok)
    {
        std::remove(temporary.c_str());
        return false;
    }
    return storage::ReplaceWithTemporaryFile(temporary, path);
}

bool ClearNteCredentials()
{
    const int result = std::remove(NteCredentialsPath().c_str());
    return result == 0 || !LoadNteCredentials().valid();
}

} // namespace opennow
