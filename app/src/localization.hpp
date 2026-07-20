#pragma once

#include <string>
#include <vector>

namespace opennow
{

struct InterfaceLanguageOption
{
    std::string code;
    std::string label;
};

const std::vector<InterfaceLanguageOption>& InterfaceLanguageOptions();
std::string InterfaceLanguageLabel(const std::string& code);
bool IsSupportedInterfaceLanguage(const std::string& code);
void SetInterfaceLanguage(const std::string& code);
const std::string& GetInterfaceLanguage();

// English source text is the stable key and fallback. This keeps technical
// errors readable even when a newly added message has not been translated yet.
std::string Tr(const std::string& english);
std::string Tr(const char* english);
std::string TrFormat(const std::string& english, const std::vector<std::string>& values);

} // namespace opennow
