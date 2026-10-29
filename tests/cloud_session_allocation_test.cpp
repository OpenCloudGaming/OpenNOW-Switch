#include "gfn/cloud_session_internal.hpp"

#include <cassert>
#include <cstdlib>
#include <string>

int main(int argc, char** argv)
{
    const int selected = argc > 1 ? std::atoi(argv[1]) : -1;
    struct Case { const char* input; const char* signaling; const char* media; int port; };
    const Case cases[] = {
        {R"({"connectionInfo":[{"usage":14,"appLevelProtocol":6,"resourcePath":"rtsps://classic.example:48322"},{"usage":14,"appLevelProtocol":5,"ip":"browser.example","port":443,"resourcePath":"/nvst/"}]})", "wss://browser.example:443/nvst/", "browser.example", 443},
        {R"({"connectionInfo":[{"usage":14,"ip":".cloudmatchbeta.nvidiagrid.net","port":443,"resourcePath":"/nvst/"},{"usage":14,"ip":"seat.example","port":443,"resourcePath":"/nvst/"}]})", "wss://seat.example:443/nvst/", "seat.example", 443},
        {R"({"connectionInfo":[{"usage":14,"appLevelProtocol":5,"ip":"seat.example","port":49003,"resourcePath":"/nvst/"}]})", "wss://seat.example:443/nvst/", "seat.example", 49003},
        {R"({"sessionControlInfo":{"ip":"np-ams-06.cloudmatchbeta.nvidiagrid.net","port":443}})", "", "", 0},
        {R"({"connectionInfo":[{"usage":14,"ip":"seat.example","port":443,"resourcePath":"/nvst/"},{"usage":14,"appLevelProtocol":6,"ip":"classic.example","port":48322,"resourcePath":"rtsps://classic.example:48322"}]})", "wss://seat.example:443/nvst/", "seat.example", 443},
        {R"({"connectionInfo":[{"usage":14,"appLevelProtocol":6,"resourcePath":"wss://classic.example/nvst/"},{"usage":14,"appLevelProtocol":5,"resourcePath":"https://browser.example:9443/custom/"},{"usage":17,"ip":"203.0.113.20","port":49005}]})", "wss://browser.example:9443/custom/", "203.0.113.20", 49005},
        {R"({"connectionInfo":[{"usage":14,"resourcePath":"wss://user:secret@bad.example/nvst/"},{"usage":14,"ip":"0.0.0.0","port":443},{"usage":14,"resourcePath":"wss://good.example:8443/nvst/"}]})", "wss://good.example:8443/nvst/", "good.example", 8443},
        {R"({"connectionInfo":[{"usage":14,"ip":"2001:db8::10","port":49003,"resourcePath":"/nvst/"}]})", "wss://[2001:db8::10]:443/nvst/", "2001:db8::10", 49003},
        {R"({"signalingUrl":"https://seat.example:9443/custom/","connectionInfo":[{"usage":2,"ip":"203.0.113.12","port":49002}]})", "wss://seat.example:9443/custom/", "203.0.113.12", 49002},
        {R"({"connectionInfo":[{"usage":16,"appLevelProtocol":6,"ip":"classic.example","port":48322}]})", "wss://classic.example/nvst/", "", 0},
        {R"({"connectionInfo":[{"usage":14,"resourcePath":"wss://[not-ip.example]/nvst/"},{"usage":14,"resourcePath":"wss://seat.example:65536/nvst/"},{"usage":14,"resourcePath":"wss://seat.example:443junk/nvst/"},{"usage":14,"resourcePath":"http://seat.example/nvst/"},{"usage":14,"resourcePath":"wss://good.example:9443/nvst/"}]})", "wss://good.example:9443/nvst/", "good.example", 9443},
        {R"({"connectionInfo":[{"usage":14,"appLevelProtocol":6,"resourcePath":"rtsps://legacy.example:48322"}]})", "wss://legacy.example/nvst/", "", 0},
        {R"({"connectionInfo":[{"usage":14,"appLevelProtocol":1,"ip":"legacy.example","port":48010}]})", "wss://legacy.example/nvst/", "", 0},
        {R"({"connectionInfo":[{"usage":14,"appLevelProtocol":4,"ip":"webrtc.example","port":8443,"resourcePath":"/nvst/"}]})", "wss://webrtc.example:8443/nvst/", "webrtc.example", 8443},
        {R"({"connectionInfo":[{"usage":14,"appLevelProtocol":4,"ip":"2001:db8::1","port":8443}]})", "wss://[2001:db8::1]:8443/nvst/", "2001:db8::1", 8443},
        {R"({"connectionInfo":[{"usage":14,"appLevelProtocol":6,"resourcePath":"rtsps://legacy.example:322"},{"usage":14,"appLevelProtocol":6,"resourcePath":"wss://legacy-web.example:9443/custom/"}]})", "wss://legacy-web.example:9443/custom/", "", 0},
        {R"({"connectionInfo":[{"usage":14,"appLevelProtocol":6,"ip":"203.0.113.10","resourcePath":"rtsps://legacy.example:48322"}]})", "wss://legacy.example/nvst/", "", 0},
        {R"({"connectionInfo":[{"usage":14,"appLevelProtocol":6,"ip":"203.0.113.10","resourcePath":"rtsps://user:secret@bad.example:322"},{"usage":14,"appLevelProtocol":6,"resourcePath":"rtsps://legacy.example:48322"}]})", "wss://legacy.example/nvst/", "", 0},
    };
    for (int i = 0; i < static_cast<int>(std::size(cases)); ++i)
    {
        if (selected >= 0 && selected != i)
            continue;
        auto root = opennow::gfn::detail::LoadJson(cases[i].input);
        opennow::SessionInfo info;
        opennow::gfn::cloud_session::ApplySessionNetworkInfo(root.get(), info);
        assert(info.signaling_url == cases[i].signaling);
        assert(info.media_ip == cases[i].media);
        assert(info.media_port == cases[i].port);
    }
    if (selected < 0 || selected == static_cast<int>(std::size(cases)))
    {
        for (const char* text : {"2", "\"2\"", "\"ready\""})
        {
            auto root = opennow::gfn::detail::LoadJson(std::string("{\"status\":") + text + "}");
            assert(opennow::gfn::cloud_session::ParseSessionStatus(json_object_get(root.get(), "status")) == 2);
        }
    }
}
