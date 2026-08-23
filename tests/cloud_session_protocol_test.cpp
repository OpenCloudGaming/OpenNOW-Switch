#include "gfn/cloud_session_internal.hpp"

#include <jansson.h>

#include <cassert>
#include <memory>
#include <string>

int main()
{
    opennow::StreamSettings settings;
    settings.width = 1280;
    settings.height = 720;
    settings.fps = 60;
    settings.bitrate_kbps = 12000;

    const std::string body = opennow::gfn::cloud_session::BuildSessionBody(
        "app-id", "title", "device-id", "sub-session-id", "network-test-id", settings);
    json_error_t error {};
    std::unique_ptr<json_t, decltype(&json_decref)> root(
        json_loads(body.c_str(), 0, &error), &json_decref);
    assert(root);

    json_t* session_request = json_object_get(root.get(), "sessionRequestData");
    assert(json_is_object(session_request));
    json_t* features = json_object_get(session_request, "requestedStreamingFeatures");
    assert(json_is_object(features));
    assert(json_integer_value(json_object_get(features, "maxBitrateKbps")) == 12000);
    assert(json_integer_value(json_object_get(features, "codec")) == 1);
    assert(json_is_false(json_object_get(features, "vsync")));
    assert(json_integer_value(json_object_get(features, "dynamicStreamingMode")) == 3);
    assert(json_integer_value(json_object_get(features, "audioChannelCount")) == 2);
    return 0;
}
