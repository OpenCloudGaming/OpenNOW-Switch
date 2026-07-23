#include "server_location_policy.hpp"

#include <cassert>

int main()
{
    using namespace opennow::server_location;

    assert(IsAutomatic(""));
    assert(IsAutomatic("Auto"));
    assert(!IsAutomatic("https://np-ams.example/"));

    assert(IsValidStreamingBaseUrl("https://np-ams.example/"));
    assert(IsValidStreamingBaseUrl("https://np-ams.example:443/path"));
    assert(!IsValidStreamingBaseUrl("http://np-ams.example/"));
    assert(!IsValidStreamingBaseUrl("https://user@np-ams.example/"));
    assert(!IsValidStreamingBaseUrl("https://np-ams.example/\nHeader: value"));

    assert(
        NormalizeStreamingBaseUrl("https://np-ams.example") ==
        "https://np-ams.example/");
    assert(NormalizeStreamingBaseUrl("not a URL").empty());

    assert(
        ResolveStreamingBaseUrl("Auto", "https://prod.example") ==
        "https://prod.example/");
    assert(
        ResolveStreamingBaseUrl(
            "https://np-lon.example", "https://prod.example/") ==
        "https://np-lon.example/");
    assert(
        ResolveStreamingBaseUrl(
            "http://unsafe.example", "https://prod.example/") ==
        "https://prod.example/");
    return 0;
}
