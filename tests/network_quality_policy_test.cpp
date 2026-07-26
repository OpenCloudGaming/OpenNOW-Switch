#include "network_quality_policy.hpp"

#include <cassert>
#include <string>

int main()
{
    using namespace opennow::network;

    assert(WifiBandFromChannel(0) == WifiBand::Unknown);
    assert(WifiBandFromChannel(1) == WifiBand::Ghz2_4);
    assert(WifiBandFromChannel(14) == WifiBand::Ghz2_4);
    assert(WifiBandFromChannel(36) == WifiBand::Ghz5);

    assert(ShouldWarnForStreaming(WifiBand::Ghz2_4));
    assert(!ShouldWarnForStreaming(WifiBand::Ghz5));
    assert(!ShouldWarnForStreaming(WifiBand::Unknown));

    assert(std::string(WifiBandLabel(WifiBand::Ghz2_4)) == "2.4 GHz");
    assert(std::string(WifiBandLabel(WifiBand::Ghz5)) == "5 GHz");
    assert(std::string(WifiBandLabel(WifiBand::Unknown)) == "Wi-Fi");
    return 0;
}
