#pragma once

#include <jansson.h>

#include <cstring>
#include <string>

namespace opennow::webrtc
{

inline std::string CompactSignalingMessage(const std::string& msg)
{
    json_error_t error;
    json_t* root = json_loads(msg.c_str(), 0, &error);
    if (!root)
        return "RX invalid-json bytes=" + std::to_string(msg.size());

    std::string summary = "RX message";
    json_t* ack = json_object_get(root, "ack");
    if (ack && json_is_integer(ack)) {
        summary = "RX ack=" + std::to_string(json_integer_value(ack));
    } else if (json_object_get(root, "hb")) {
        summary = "RX hb";
    } else if (json_t* peer_info = json_object_get(root, "peer_info"); peer_info && json_is_object(peer_info)) {
        json_t* id = json_object_get(peer_info, "id");
        summary = "RX peer_info";
        if (id && json_is_integer(id))
            summary += " id=" + std::to_string(json_integer_value(id));
    } else if (json_t* peer_msg = json_object_get(root, "peer_msg"); peer_msg && json_is_object(peer_msg)) {
        const char* raw_payload = json_string_value(json_object_get(peer_msg, "msg"));
        if (raw_payload) {
            json_error_t payload_error;
            json_t* payload = json_loads(raw_payload, 0, &payload_error);
            if (payload) {
                const char* type = json_string_value(json_object_get(payload, "type"));
                const char* candidate = json_string_value(json_object_get(payload, "candidate"));
                if (type && std::string(type) == "offer") {
                    const char* sdp = json_string_value(json_object_get(payload, "sdp"));
                    const char* nvst = json_string_value(json_object_get(payload, "nvstSdp"));
                    summary = "RX offer sdpBytes=" + std::to_string(sdp ? std::strlen(sdp) : 0) +
                        " nvstBytes=" + std::to_string(nvst ? std::strlen(nvst) : 0);
                } else if (candidate) {
                    summary = "RX candidate bytes=" + std::to_string(std::strlen(candidate));
                } else {
                    summary = "RX peer_msg bytes=" + std::to_string(std::strlen(raw_payload));
                }
                json_decref(payload);
            } else {
                summary = "RX peer_msg invalid-json bytes=" + std::to_string(std::strlen(raw_payload));
            }
        }
    }

    json_decref(root);
    return summary;
}

}
