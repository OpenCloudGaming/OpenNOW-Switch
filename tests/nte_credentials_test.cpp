#include "nte_credentials.hpp"

#include <cassert>

int main()
{
    assert(opennow::IsNevernessToEverness("NTE: Neverness to Everness"));
    assert(opennow::IsNevernessToEverness("Neverness to Everness"));
    assert(!opennow::IsNevernessToEverness("Fortnite"));

    auto credentials = opennow::ParseNteCredentials(
        "email=player@example.com\r\npassword=example value\r\n");
    assert(credentials.valid());
    assert(credentials.email == "player@example.com");
    assert(credentials.password == "example value");

    const auto positional = opennow::ParseNteCredentials(
        "player@example.com\nexample-password\n");
    assert(positional.valid());
    assert(opennow::ParseNteCredentials("email=invalid\npassword=value\n").email.empty());
    assert(opennow::SerializeNteCredentials(credentials).find("email=player@example.com") == 0);
    return 0;
}
