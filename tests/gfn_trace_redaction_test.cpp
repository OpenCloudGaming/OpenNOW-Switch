#include "gfn/internal.hpp"

#include <cassert>
#include <stdexcept>
#include <string>

int main()
{
    using namespace opennow::gfn::detail;
    const std::string marker = "fixtureSecret";
    const std::string valid = "{\"sessionToken\":\"" + marker +
        "\",\"nested\":[{\"credential\":\"" + marker + "\"}],\"status\":2}";
    const auto redacted = JsonForTrace(valid);
    assert(redacted.find(marker) == std::string::npos);
    assert(redacted.find("status") != std::string::npos);
    for (const auto& body : {valid.substr(0, valid.size() - 1), marker,
             "\"" + marker + "\"", "[\"" + marker + "\"]"})
        assert(JsonForTrace(body).find(marker) == std::string::npos);

    bool failed = false;
    try
    {
        LoadJson(marker);
    }
    catch (const std::runtime_error& error)
    {
        failed = true;
        assert(std::string(error.what()).find(marker) == std::string::npos);
    }
    assert(failed);
}
