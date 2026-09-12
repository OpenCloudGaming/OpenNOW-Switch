#include "webrtc/signaling_diagnostics.hpp"

#include <cassert>
#include <cstdlib>
#include <string>

int main()
{
    using opennow::webrtc::CompactSignalingMessage;
    assert(CompactSignalingMessage("{\"ack\":42}") == "RX ack=42");
    assert(CompactSignalingMessage("{\"hb\":1}") == "RX hb");
    assert(CompactSignalingMessage("{\"peer_info\":{\"name\":\"synthetic-secret\",\"id\":7}}") ==
           "RX peer_info id=7");
    assert(CompactSignalingMessage("synthetic-secret") == "RX invalid-json bytes=16");

    for (const char* payload : {
             "{\"type\":\"offer\",\"sdp\":\"a=ice-pwd:synthetic-secret\",\"nvstSdp\":\"synthetic-secret\"}",
             "{\"type\":\"answer\",\"sdp\":\"a=ice-pwd:synthetic-secret\"}",
             "{\"candidate\":\"synthetic-secret\"}",
             "{\"token\":\"synthetic-secret\"}",
             "synthetic-secret"}) {
        json_t* root = json_pack("{s:{s:s}}", "peer_msg", "msg", payload);
        char* wire = json_dumps(root, 0);
        assert(wire);
        const std::string summary = CompactSignalingMessage(wire);
        assert(summary.find("synthetic-secret") == std::string::npos);
        assert(summary.find("ice-pwd") == std::string::npos);
        assert(summary.starts_with("RX "));
        std::free(wire);
        json_decref(root);
    }
}
