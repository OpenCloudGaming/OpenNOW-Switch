#include "localization.hpp"

#include <cassert>
#include <set>

int main()
{
    const auto& languages = opennow::InterfaceLanguageOptions();
    assert(languages.size() == 8);

    std::set<std::string> codes;
    for (const auto& language : languages)
    {
        assert(!language.code.empty());
        assert(!language.label.empty());
        assert(codes.insert(language.code).second);

        opennow::SetInterfaceLanguage(language.code);
        assert(opennow::GetInterfaceLanguage() == language.code);
        assert(!opennow::Tr("Settings").empty());
        assert(!opennow::Tr("Store").empty());
        assert(!opennow::Tr("Library").empty());
        assert(!opennow::Tr("Language").empty());
    }

    opennow::SetInterfaceLanguage("unsupported");
    assert(opennow::GetInterfaceLanguage() == "en");
    assert(opennow::Tr("Untranslated technical message") ==
           "Untranslated technical message");
    return 0;
}
