#include "opennow/gfn/CloudMatch.hpp"

#include <cassert>
#include <string>

int main() {
    opennow::gfn::CloudMatchRequestBuilder builder;
    const auto body = builder.buildSessionCreateBody({
        .token = "jwt",
        .streamingBaseUrl = "https://zone.example",
        .zone = "np-ams-01",
        .appId = "123",
        .internalTitle = "",
        .clientId = "client",
        .deviceId = "device",
        .settings = {},
    });

    assert(body.find("\"appId\":\"123\"") != std::string::npos);
    assert(body.find("\"widthInPixels\":1280") != std::string::npos);
    assert(body.find("\"heightInPixels\":720") != std::string::npos);
    assert(body.find("\"framesPerSecond\":60") != std::string::npos);
    assert(body.find("\"deviceHashId\":\"device\"") != std::string::npos);
    assert(body.find("\"key\":\"GSStreamerType\",\"value\":\"WebRTC\"") != std::string::npos);
    assert(body.find("\"trueHdr\":false") != std::string::npos);

    const auto create = builder.buildCreateSessionRequest({
        .token = "jwt",
        .streamingBaseUrl = "https://zone.example/",
        .zone = "np-ams-01",
        .appId = "123",
        .clientId = "client",
        .deviceId = "device",
        .settings = {},
    });
    assert(create.method == opennow::net::HttpMethod::Post);
    assert(create.url == "https://zone.example/v2/session");
    assert(create.headers.at("Authorization") == "GFNJWT jwt");
    assert(create.headers.at("Origin") == "https://play.geforcenow.com");

    const auto poll = builder.buildPollSessionRequest({
        .token = "jwt",
        .streamingBaseUrl = "https://zone.example",
        .serverIp = "203.0.113.5",
        .zone = "np-ams-01",
        .sessionId = "session",
        .clientId = "client",
        .deviceId = "device",
    });
    assert(poll.method == opennow::net::HttpMethod::Get);
    assert(poll.url == "https://203.0.113.5/v2/session/session");
    assert(poll.headers.find("Origin") == poll.headers.end());

    const auto stop = builder.buildStopSessionRequest({
        .token = "jwt",
        .streamingBaseUrl = "https://zone.example",
        .serverIp = "np-ams-01.cloudmatchbeta.nvidiagrid.net",
        .zone = "np-ams-01",
        .sessionId = "session",
        .clientId = "client",
        .deviceId = "device",
    });
    assert(stop.method == opennow::net::HttpMethod::Delete);
    assert(stop.url == "https://zone.example/v2/session/session");

    const auto claimBody = builder.buildSessionClaimBody({
        .sessionId = "session",
        .appId = "123",
        .deviceId = "device",
        .subSessionId = "sub",
    });
    assert(claimBody.find("\"action\":2") != std::string::npos);
    assert(claimBody.find("\"data\":\"RESUME\"") != std::string::npos);
    assert(claimBody.find("\"appId\":123") != std::string::npos);
    assert(claimBody.find("\"deviceHashId\":\"device\"") != std::string::npos);

    const auto parsed = opennow::gfn::CloudMatchResponseParser().parseSessionInfo(R"({
        "requestStatus": { "statusCode": 1 },
        "session": {
            "sessionId": "session-1",
            "status": 2,
            "queuePosition": 7,
            "gpuType": "RTX 4080",
            "connectionInfo": [
                { "usage": 14, "port": 443, "resourcePath": "rtsps://80-250-97-40.server.net:443/nvst/" },
                { "usage": 2, "ip": "80.250.97.40", "port": 49000 }
            ],
            "sessionControlInfo": { "ip": "np-ams-01.cloudmatchbeta.nvidiagrid.net" },
            "iceServerConfiguration": {
                "iceServers": [
                    { "urls": ["stun:one.example:19302"], "username": "u", "credential": "c" },
                    { "urls": "turn:two.example:3478" }
                ]
            }
        }
    })", "np-ams-01", "https://np-ams-01.cloudmatchbeta.nvidiagrid.net");

    assert(parsed.ok);
    assert(parsed.session.sessionId == "session-1");
    assert(parsed.session.status == 2);
    assert(parsed.session.queuePosition == 7);
    assert(parsed.session.gpuType == "RTX 4080");
    assert(parsed.session.serverIp == "80-250-97-40.server.net");
    assert(parsed.session.signalingUrl == "wss://80-250-97-40.server.net/nvst/");
    assert(parsed.session.signalingServer == "80-250-97-40.server.net:443");
    assert(parsed.session.mediaConnectionInfo.ip == "80.250.97.40");
    assert(parsed.session.mediaConnectionInfo.port == 49000);
    assert(parsed.session.iceServers.size() == 2);
    assert(parsed.session.iceServers[0].urls[0] == "stun:one.example:19302");
    assert(parsed.session.iceServers[0].username == "u");

    const auto queued = opennow::gfn::CloudMatchResponseParser().parseSessionInfo(R"({
        "requestStatus": { "statusCode": 1 },
        "session": {
            "sessionId": "queued-session",
            "status": 1,
            "queuePosition": 4
        }
    })", "np-ams-01", "https://np-ams-01.cloudmatchbeta.nvidiagrid.net");
    assert(queued.ok);
    assert(queued.session.sessionId == "queued-session");
    assert(queued.session.queuePosition == 4);
    assert(queued.session.signalingUrl.empty());

    const auto failed = opennow::gfn::CloudMatchResponseParser().parseSessionInfo(R"({
        "requestStatus": { "statusCode": 8, "statusDescription": "bad" }
    })", "", "");
    assert(!failed.ok);
    assert(failed.error == "bad");

    return 0;
}
