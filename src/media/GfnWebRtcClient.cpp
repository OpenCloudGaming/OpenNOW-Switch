#include "opennow/media/GfnWebRtcClient.hpp"

#include "opennow/gfn/InputEncoder.hpp"
#include "opennow/gfn/Sdp.hpp"
#include "opennow/gfn/SignalingClient.hpp"
#include "opennow/net/WssClient.hpp"
#include "opennow/util/Json.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cctype>
#include <cstring>
#include <deque>
#include <functional>
#include <iterator>
#include <mutex>
#include <condition_variable>
#include <atomic>
#include <random>
#include <sstream>
#include <thread>
#include <utility>
#include <variant>
#include <vector>

#if defined(OPENNOW_PLATFORM_SWITCH)
#include <sys/stat.h>
#endif

#if defined(OPENNOW_HAS_LIBDATACHANNEL)
#include <rtc/rtc.hpp>
#endif

#if defined(OPENNOW_HAS_LIBPEER)
extern "C" {
#include <peer.h>
}
#endif

namespace opennow::media {

namespace {

std::string trim(std::string value) {
    while (!value.empty() && std::isspace(static_cast<unsigned char>(value.front()))) {
        value.erase(value.begin());
    }
    while (!value.empty() && std::isspace(static_cast<unsigned char>(value.back()))) {
        value.pop_back();
    }
    return value;
}

std::string lower(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return value;
}

std::string jsonEscape(const std::string& value) {
    std::ostringstream out;
    for (const auto c : value) {
        switch (c) {
        case '\\': out << "\\\\"; break;
        case '"': out << "\\\""; break;
        case '\b': out << "\\b"; break;
        case '\f': out << "\\f"; break;
        case '\n': out << "\\n"; break;
        case '\r': out << "\\r"; break;
        case '\t': out << "\\t"; break;
        default:
            if (static_cast<unsigned char>(c) < 0x20) {
                out << "\\u";
                static constexpr char hex[] = "0123456789abcdef";
                out << "00" << hex[(c >> 4) & 0xf] << hex[c & 0xf];
            } else {
                out << c;
            }
            break;
        }
    }
    return out.str();
}

std::string defaultPeerName() {
    static std::random_device rd;
    static std::mt19937 rng(rd());
    static std::uniform_int_distribution<int> dist(1'000'000, 9'999'999);
    return "peer-" + std::to_string(dist(rng));
}

std::string signalingSignInUrl(const SessionInfo& session, const std::string& peerName) {
    gfn::SignalingClient signaling({
        .signalingServer = session.signalingServer,
        .sessionId = session.sessionId,
        .signalingUrl = session.signalingUrl,
    });
    auto url = signaling.signInUrl();
    const auto marker = url.find("peer_id=");
    if (marker == std::string::npos) {
        return url;
    }
    const auto valueStart = marker + 8;
    const auto valueEnd = url.find('&', valueStart);
    url.replace(valueStart, valueEnd == std::string::npos ? std::string::npos : valueEnd - valueStart, peerName);
    return url;
}

std::string peerInfoJson(std::uint32_t ackId, std::uint32_t peerId, const std::string& peerName, const StreamSettings& settings) {
    (void)settings;
    std::ostringstream json;
    json
        << "{\"ackid\":" << ackId << ",\"peer_info\":{"
        << "\"browser\":\"Chrome\","
        << "\"browserVersion\":\"131\","
        << "\"connected\":true,"
        << "\"id\":" << peerId << ","
        << "\"name\":\"" << jsonEscape(peerName) << "\","
        << "\"peerRole\":0,"
        << "\"resolution\":\"1920x1080\","
        << "\"version\":2"
        << "}}";
    return json.str();
}

std::string answerPeerMessageJson(std::uint32_t ackId, std::uint32_t from, std::uint32_t to, const std::string& answerSdp, const std::string& nvstSdp) {
    std::ostringstream answer;
    answer
        << "{\"type\":\"answer\",\"sdp\":\"" << jsonEscape(answerSdp) << "\"";
    if (!nvstSdp.empty()) {
        answer << ",\"nvstSdp\":\"" << jsonEscape(nvstSdp) << "\"";
    }
    answer << "}";

    std::ostringstream json;
    json
        << "{\"peer_msg\":{"
        << "\"from\":" << from << ","
        << "\"to\":" << to << ","
        << "\"msg\":\"" << jsonEscape(answer.str()) << "\""
        << "},\"ackid\":" << ackId << "}";
    return json.str();
}

[[maybe_unused]] std::string candidatePeerMessageJson(std::uint32_t ackId, std::uint32_t from, std::uint32_t to, const std::string& candidate, const std::string& mid, std::uint32_t mLineIndex) {
    std::ostringstream payload;
    payload
        << "{\"candidate\":\"" << jsonEscape(candidate) << "\","
        << "\"sdpMid\":\"" << jsonEscape(mid) << "\","
        << "\"sdpMLineIndex\":" << mLineIndex
        << "}";

    std::ostringstream json;
    json
        << "{\"peer_msg\":{"
        << "\"from\":" << from << ","
        << "\"to\":" << to << ","
        << "\"msg\":\"" << jsonEscape(payload.str()) << "\""
        << "},\"ackid\":" << ackId << "}";
    return json.str();
}

std::string ackJson(std::uint32_t ackId) {
    return "{\"ack\":" + std::to_string(ackId) + "}";
}

std::string heartbeatJson() {
    return "{\"hb\":1}";
}

std::uint32_t jsonU32(const util::JsonValue* object, const char* key, std::uint32_t fallback = 0) {
    if (!object) {
        return fallback;
    }
    const auto* value = object->get(key);
    if (!value || !value->isNumber()) {
        return fallback;
    }
    return static_cast<std::uint32_t>(value->asNumber(fallback));
}

std::string jsonString(const util::JsonValue* object, const char* key, const std::string& fallback = {}) {
    if (!object) {
        return fallback;
    }
    const auto* value = object->get(key);
    return value && value->isString() ? value->asString(fallback) : fallback;
}

[[maybe_unused]] bool jsonNumber(const util::JsonValue* object, const char* key, double& out) {
    if (!object) {
        return false;
    }
    const auto* value = object->get(key);
    if (!value || !value->isNumber()) {
        return false;
    }
    out = value->asNumber(0.0);
    return true;
}

std::string endpointIpForCandidate(const SessionInfo& session) {
    auto ip = gfn::extractPublicIp(session.mediaConnectionInfo.ip);
    if (ip.empty()) {
        ip = gfn::extractPublicIp(session.serverIp);
    }
    return ip;
}

std::string normalizeSdpForLibpeer(const std::string& sdp) {
    std::vector<std::string> lines;
    std::string current;
    for (std::size_t i = 0; i < sdp.size(); ++i) {
        const char c = sdp[i];
        if (c == '\r') {
            continue;
        }
        if (c == '\n') {
            lines.push_back(current);
            current.clear();
            continue;
        }
        current.push_back(c);
    }
    if (!current.empty()) {
        lines.push_back(current);
    }

    std::ostringstream out;
    for (const auto& line : lines) {
        if (!line.empty()) {
            out << line << "\r\n";
        }
    }
    return out.str();
}

[[maybe_unused]] std::string ensureIceLite(const std::string& sdp) {
    if (sdp.find("\na=ice-lite") != std::string::npos || sdp.rfind("a=ice-lite", 0) == 0) {
        return sdp;
    }

    const auto eol = sdp.find("\r\n") != std::string::npos ? std::string("\r\n") : std::string("\n");
    const auto firstMedia = sdp.find(eol + "m=");
    if (firstMedia != std::string::npos) {
        auto updated = sdp;
        updated.insert(firstMedia + eol.size(), "a=ice-lite" + eol);
        return updated;
    }

    auto updated = sdp;
    if (!updated.empty() && updated.back() != '\n') {
        updated += eol;
    }
    updated += "a=ice-lite" + eol;
    return updated;
}

[[maybe_unused]] std::string removeApplicationMediaSections(const std::string& sdp) {
    std::vector<std::string> lines;
    std::string current;
    for (std::size_t i = 0; i <= sdp.size(); ++i) {
        const char c = i < sdp.size() ? sdp[i] : '\n';
        if (c == '\r') {
            continue;
        }
        if (c != '\n') {
            current.push_back(c);
            continue;
        }
        if (!current.empty()) {
            lines.push_back(current);
        }
        current.clear();
    }

    std::vector<std::string> output;
    bool skip = false;
    for (const auto& line : lines) {
        if (line.rfind("m=", 0) == 0) {
            skip = line.rfind("m=application", 0) == 0;
        }
        if (!skip) {
            output.push_back(line);
        }
    }

    const auto ending = sdp.find("\r\n") != std::string::npos ? "\r\n" : "\n";
    std::ostringstream out;
    for (const auto& line : output) {
        out << line << ending;
    }
    return out.str();
}

#if defined(OPENNOW_HAS_LIBDATACHANNEL)
void appendRtcLog(const std::string& message) {
#if defined(OPENNOW_PLATFORM_SWITCH)
    (void)::mkdir("sdmc:/switch", 0777);
    (void)::mkdir("sdmc:/switch/OpenNOW", 0777);
    FILE* file = std::fopen("sdmc:/switch/OpenNOW/webrtc.log", "ab");
    if (file) {
        std::fprintf(file, "%s\n", message.c_str());
        std::fflush(file);
        std::fclose(file);
    }
#else
    (void)message;
#endif
}

void appendRtcSdpBlock(const char* title, const std::string& sdp) {
    appendRtcLog(std::string("[RTC] === ") + title + " START ===");
    std::string line;
    for (std::size_t i = 0; i <= sdp.size(); ++i) {
        const char c = i < sdp.size() ? sdp[i] : '\n';
        if (c == '\r') {
            continue;
        }
        if (c != '\n') {
            line.push_back(c);
            continue;
        }
        appendRtcLog("[RTC] SDP> " + line);
        line.clear();
    }
    appendRtcLog(std::string("[RTC] === ") + title + " END ===");
}

std::vector<std::string> mediaMidsFromSdp(const std::string& sdp) {
    std::vector<std::string> mids;
    std::string line;
    bool inMedia = false;
    for (std::size_t i = 0; i <= sdp.size(); ++i) {
        const char c = i < sdp.size() ? sdp[i] : '\n';
        if (c == '\r') {
            continue;
        }
        if (c != '\n') {
            line.push_back(c);
            continue;
        }
        if (line.rfind("m=", 0) == 0) {
            inMedia = true;
        } else if (inMedia && line.rfind("a=mid:", 0) == 0) {
            mids.push_back(line.substr(6));
            inMedia = false;
        }
        line.clear();
    }
    if (mids.empty()) {
        mids.push_back("0");
    }
    return mids;
}

std::string stateText(rtc::PeerConnection::State state) {
    switch (state) {
    case rtc::PeerConnection::State::New: return "new";
    case rtc::PeerConnection::State::Connecting: return "connecting";
    case rtc::PeerConnection::State::Connected: return "connected";
    case rtc::PeerConnection::State::Disconnected: return "disconnected";
    case rtc::PeerConnection::State::Failed: return "failed";
    case rtc::PeerConnection::State::Closed: return "closed";
    }
    return "unknown";
}

std::string iceStateText(rtc::PeerConnection::IceState state) {
    switch (state) {
    case rtc::PeerConnection::IceState::New: return "new";
    case rtc::PeerConnection::IceState::Checking: return "checking";
    case rtc::PeerConnection::IceState::Connected: return "connected";
    case rtc::PeerConnection::IceState::Completed: return "completed";
    case rtc::PeerConnection::IceState::Failed: return "failed";
    case rtc::PeerConnection::IceState::Disconnected: return "disconnected";
    case rtc::PeerConnection::IceState::Closed: return "closed";
    }
    return "unknown";
}

std::string gatheringStateText(rtc::PeerConnection::GatheringState state) {
    switch (state) {
    case rtc::PeerConnection::GatheringState::New: return "new";
    case rtc::PeerConnection::GatheringState::InProgress: return "in-progress";
    case rtc::PeerConnection::GatheringState::Complete: return "complete";
    }
    return "unknown";
}

bool isTcpIceCandidate(const std::string& candidate) {
    std::istringstream in(candidate);
    std::string foundation;
    std::string component;
    std::string protocol;
    in >> foundation >> component >> protocol;
    return lower(protocol) == "tcp";
}

std::uint32_t midToMLineIndex(const std::string& mid, const std::vector<std::string>& mids) {
    if (!mid.empty() && std::all_of(mid.begin(), mid.end(), [](unsigned char c) { return std::isdigit(c); })) {
        return static_cast<std::uint32_t>(std::stoul(mid));
    }
    const auto it = std::find(mids.begin(), mids.end(), mid);
    if (it != mids.end()) {
        return static_cast<std::uint32_t>(std::distance(mids.begin(), it));
    }
    return 0;
}

std::string candidateMidFromPayload(const util::JsonValue& payload, const std::vector<std::string>& mids) {
    auto mid = jsonString(&payload, "sdpMid");
    if (!mid.empty()) {
        return mid;
    }
    double mLine = 0.0;
    if (jsonNumber(&payload, "sdpMLineIndex", mLine) && mLine >= 0.0) {
        const auto index = static_cast<std::size_t>(mLine);
        if (index < mids.size()) {
            return mids[index];
        }
        return std::to_string(index);
    }
    return mids.empty() ? "0" : mids.front();
}
#endif

std::string timeoutText(const std::string& error) {
    return lower(error);
}

std::string compactLine(std::string value, std::size_t limit) {
    value.erase(std::remove(value.begin(), value.end(), '\r'), value.end());
    std::replace(value.begin(), value.end(), '\n', ' ');
    if (value.size() > limit) {
        value.resize(limit > 3 ? limit - 3 : limit);
        value += "...";
    }
    return value;
}

[[maybe_unused]] std::uint32_t parseRiIntegerAttribute(const std::string& sdp, const std::string& attribute, std::uint32_t fallback) {
    const auto needle = "a=" + attribute + ":";
    const auto pos = sdp.find(needle);
    if (pos == std::string::npos) {
        return fallback;
    }
    auto end = sdp.find_first_of("\r\n", pos);
    if (end == std::string::npos) {
        end = sdp.size();
    }
    auto value = trim(sdp.substr(pos + needle.size(), end - pos - needle.size()));
    if (value.empty()) {
        return fallback;
    }
    try {
        int base = 10;
        if (value.size() > 2 && value[0] == '0' && (value[1] == 'x' || value[1] == 'X')) {
            value = value.substr(2);
            base = 16;
        }
        return static_cast<std::uint32_t>(std::stoul(value, nullptr, base));
    } catch (...) {
        return fallback;
    }
}

[[maybe_unused]] bool isPartiallyReliableHidTransferEligible(std::uint32_t inputType) {
    return inputType == gfn::INPUT_MOUSE_REL;
}

#if defined(OPENNOW_HAS_LIBPEER)
std::string iceStatsText(PeerConnection* pc) {
    PeerConnectionIceStats stats{};
    if (!pc || peer_connection_get_ice_stats(pc, &stats) != 0) {
        return "ice=unavailable";
    }
    std::ostringstream out;
    out << "ice local=" << stats.local_candidates
        << " remote=" << stats.remote_candidates
        << " pairs=" << stats.candidate_pairs
        << " checks=" << stats.nominated_pair_checks
        << " pairState=" << stats.nominated_pair_state;
    if (stats.remote_addr[0] != '\0') {
        out << " pair=" << stats.local_addr << ":" << stats.local_port
            << "->" << stats.remote_addr << ":" << stats.remote_port;
    }
    return out.str();
}
#endif

} // namespace

struct GfnWebRtcClient::Impl {
    struct QueuedIceCandidate {
        std::string candidate;
        std::string mid = "0";
        std::uint32_t mLineIndex = 0;
    };

#if defined(OPENNOW_HAS_LIBDATACHANNEL)
    struct PendingIceCandidate {
        std::string candidate;
        std::string mid = "0";
        std::uint32_t mLineIndex = 0;
    };

    std::shared_ptr<rtc::PeerConnection> pc;
    std::shared_ptr<rtc::DataChannel> reliableInput;
    std::shared_ptr<rtc::DataChannel> partialInput;
    std::shared_ptr<rtc::DataChannel> controlChannel;
    std::vector<std::shared_ptr<rtc::Track>> remoteTracks;
    std::atomic<bool> rtcInitialized{false};
    std::atomic<bool> closing{false};
    std::atomic<rtc::PeerConnection::State> state{rtc::PeerConnection::State::Closed};
    std::atomic<rtc::PeerConnection::IceState> iceState{rtc::PeerConnection::IceState::Closed};
    std::atomic<rtc::PeerConnection::GatheringState> gatheringState{rtc::PeerConnection::GatheringState::New};
    std::size_t videoBytes = 0;
    std::size_t audioBytes = 0;
    std::mutex callbackMutex;
    std::mutex descriptionMutex;
    std::mutex localCandidateMutex;
    std::condition_variable descriptionCv;
    std::function<void(const std::uint8_t*, std::size_t)> videoNal;
    std::function<void(const std::uint8_t*, std::size_t)> opusPacket;
    std::string localDescription;
    bool localDescriptionReady = false;
    bool inputChannelsOpened = false;
    std::atomic<bool> inputReady{false};
    std::atomic<bool> heartbeatStop{false};
    std::thread heartbeatThread;
    gfn::InputEncoder inputEncoder;
    std::uint32_t partialReliableThresholdMs = 300;
    std::uint32_t hidDeviceMask = 0xffffffffu;
    std::uint32_t enablePartiallyReliableTransferGamepad = 0x0fu;
    std::uint32_t enablePartiallyReliableTransferHid = 0xffffffffu;
    std::vector<std::string> remoteMids;
    std::vector<PendingIceCandidate> localCandidates;

    void dispatchVideo(const rtc::binary& frame) {
        if (closing.load()) {
            return;
        }
        videoBytes += frame.size();
        std::function<void(const std::uint8_t*, std::size_t)> callback;
        {
            std::lock_guard<std::mutex> lock(callbackMutex);
            callback = videoNal;
        }
        if (!closing.load() && callback && !frame.empty()) {
            callback(reinterpret_cast<const std::uint8_t*>(frame.data()), frame.size());
        }
    }

    void dispatchAudio(const rtc::binary& frame) {
        if (closing.load()) {
            return;
        }
        audioBytes += frame.size();
        std::function<void(const std::uint8_t*, std::size_t)> callback;
        {
            std::lock_guard<std::mutex> lock(callbackMutex);
            callback = opusPacket;
        }
        if (!closing.load() && callback && !frame.empty()) {
            callback(reinterpret_cast<const std::uint8_t*>(frame.data()), frame.size());
        }
    }

    static std::vector<std::uint8_t> messageBytes(const rtc::message_variant& data) {
        if (const auto* binary = std::get_if<rtc::binary>(&data)) {
            std::vector<std::uint8_t> bytes(binary->size());
            std::transform(binary->begin(), binary->end(), bytes.begin(), [](std::byte value) {
                return std::to_integer<std::uint8_t>(value);
            });
            return bytes;
        }
        if (const auto* text = std::get_if<std::string>(&data)) {
            return {text->begin(), text->end()};
        }
        return {};
    }

    static std::uint16_t readU16Le(const std::vector<std::uint8_t>& bytes, std::size_t offset) {
        return static_cast<std::uint16_t>(bytes[offset])
            | static_cast<std::uint16_t>(static_cast<std::uint16_t>(bytes[offset + 1]) << 8);
    }

    static std::uint16_t parseInputHandshakeVersion(const std::vector<std::uint8_t>& bytes) {
        if (bytes.size() < 2) {
            return 0;
        }
        const auto firstWord = readU16Le(bytes, 0);
        if (firstWord == 526) {
            return bytes.size() >= 4 ? readU16Le(bytes, 2) : 2;
        }
        if (bytes[0] == 0x0e) {
            return firstWord;
        }
        return 0;
    }

    void stopInputHeartbeat() {
        heartbeatStop = true;
        if (heartbeatThread.joinable()) {
            heartbeatThread.join();
        }
    }

    void sendReliableBytes(const gfn::Bytes& bytes) {
        auto channel = reliableInput;
        if (!channel || !channel->isOpen() || bytes.empty()) {
            return;
        }
        (void)channel->send(reinterpret_cast<const std::byte*>(bytes.data()), bytes.size());
    }

    bool sendBytes(const gfn::Bytes& bytes, bool partiallyReliable) {
        if (bytes.empty() || !inputReady.load()) {
            return false;
        }
        auto channel = partiallyReliable ? partialInput : reliableInput;
        if (channel && channel->isOpen()) {
            return channel->send(reinterpret_cast<const std::byte*>(bytes.data()), bytes.size());
        }
        if (partiallyReliable && reliableInput && reliableInput->isOpen()) {
            return reliableInput->send(reinterpret_cast<const std::byte*>(bytes.data()), bytes.size());
        }
        return false;
    }

    bool canUsePartiallyReliableForInput(std::uint32_t inputType, std::uint16_t controllerId = 0) const {
        if (!partialInput || !partialInput->isOpen()) {
            return false;
        }
        if (inputType == gfn::INPUT_GAMEPAD) {
            if (controllerId >= 4) {
                return false;
            }
            const auto bit = static_cast<std::uint32_t>(1u << controllerId);
            return (enablePartiallyReliableTransferGamepad & bit) != 0;
        }
        if (!isPartiallyReliableHidTransferEligible(inputType)) {
            return false;
        }
        const auto bit = static_cast<std::uint32_t>(1u << inputType);
        return (hidDeviceMask & bit) != 0 && (enablePartiallyReliableTransferHid & bit) != 0;
    }

    void startInputHeartbeat() {
        stopInputHeartbeat();
        heartbeatStop = false;
        heartbeatThread = std::thread([this] {
            while (!heartbeatStop.load()) {
                if (inputReady.load()) {
                    sendReliableBytes(inputEncoder.encodeHeartbeat());
                }
                for (int i = 0; i < 20 && !heartbeatStop.load(); ++i) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(100));
                }
            }
        });
    }

    void handleInputHandshake(const rtc::message_variant& data) {
        const auto bytes = messageBytes(data);
        if (!inputReady.load()) {
            std::ostringstream hex;
            const auto count = std::min<std::size_t>(bytes.size(), 16);
            for (std::size_t i = 0; i < count; ++i) {
                if (i != 0) {
                    hex << ' ';
                }
                hex << std::hex << std::nouppercase;
                hex.width(2);
                hex.fill('0');
                hex << static_cast<unsigned>(bytes[i]);
            }
            appendRtcLog("[RTC] Input channel message: " + std::to_string(bytes.size()) + " bytes [" + hex.str() + "].");
        }
        const auto version = parseInputHandshakeVersion(bytes);
        if (version == 0) {
            appendRtcLog("[RTC] Input datachannel message was not a GFN handshake.");
            return;
        }
        inputEncoder.setProtocolVersion(version);
        const auto wasReady = inputReady.exchange(true);
        if (!wasReady) {
            appendRtcLog("[RTC] Input handshake complete (protocol v" + std::to_string(version) + ").");
            sendReliableBytes(inputEncoder.encodeHeartbeat());
            startInputHeartbeat();
        } else {
            appendRtcLog("[RTC] Input protocol version refreshed from handshake: v" + std::to_string(version) + ".");
        }
    }

    void handleControlMessage(const rtc::message_variant& data) {
        const auto bytes = messageBytes(data);
        if (bytes.empty()) {
            return;
        }
        const std::string text(bytes.begin(), bytes.end());
        auto parsed = util::parseJson(text);
        if (!parsed.ok) {
            appendRtcLog("[RTC] Control datachannel non-JSON message bytes=" + std::to_string(bytes.size()) + ".");
            return;
        }
        const auto* timer = parsed.value.get("timerNotification");
        if (timer && timer->isObject()) {
            const auto code = timer->get("code") ? timer->get("code")->asNumber(-1) : -1;
            const auto secondsLeft = timer->get("secondsLeft") ? timer->get("secondsLeft")->asNumber(-1) : -1;
            appendRtcLog("[RTC] Control timer notification code=" + std::to_string(static_cast<int>(code))
                + " secondsLeft=" + std::to_string(static_cast<int>(secondsLeft)) + ".");
            return;
        }
        appendRtcLog("[RTC] Control datachannel JSON message: " + compactLine(text, 180));
    }

    void wireInputDataChannelCallbacks() {
        if (reliableInput) {
            reliableInput->onOpen([this] {
                appendRtcLog("[RTC] Reliable input datachannel open.");
            });
            reliableInput->onClosed([this] {
                inputReady = false;
                heartbeatStop = true;
                appendRtcLog("[RTC] Reliable input datachannel closed.");
            });
            reliableInput->onError([](std::string error) {
                appendRtcLog("[RTC] Reliable input datachannel error: " + error);
            });
            reliableInput->onMessage([this](rtc::message_variant data) {
                handleInputHandshake(data);
            });
        }
        if (partialInput) {
            partialInput->onOpen([] {
                appendRtcLog("[RTC] Partially reliable input datachannel open.");
            });
            partialInput->onClosed([] {
                appendRtcLog("[RTC] Partially reliable input datachannel closed.");
            });
            partialInput->onError([](std::string error) {
                appendRtcLog("[RTC] Partially reliable input datachannel error: " + error);
            });
        }
    }

    bool createInputDataChannels(std::uint32_t partialReliableThresholdMs) {
        if (!pc || inputChannelsOpened) {
            return false;
        }
        try {
            rtc::DataChannelInit reliableInit;
            reliableInit.reliability.unordered = false;

            rtc::DataChannelInit partialInit;
            partialInit.reliability.unordered = true;
            partialInit.reliability.maxPacketLifeTime = std::chrono::milliseconds(partialReliableThresholdMs);

            reliableInput = pc->createDataChannel("input_channel_v1", reliableInit);
            partialInput = pc->createDataChannel("input_channel_partially_reliable", partialInit);
            wireInputDataChannelCallbacks();
            inputChannelsOpened = static_cast<bool>(reliableInput) || static_cast<bool>(partialInput);
            appendRtcLog(std::string("[RTC] GFN input datachannels ")
                + (inputChannelsOpened ? "created for answer negotiation with DCEP." : "not created."));
            return inputChannelsOpened;
        } catch (const std::exception& e) {
            appendRtcLog(std::string("[RTC] Failed to create input datachannels: ") + e.what());
            return false;
        } catch (...) {
            appendRtcLog("[RTC] Failed to create input datachannels.");
            return false;
        }
    }
#elif defined(OPENNOW_HAS_LIBPEER)
    PeerConnection* pc = nullptr;
    bool peerInitialized = false;
    std::size_t videoBytes = 0;
    std::size_t audioBytes = 0;
    std::mutex callbackMutex;
    std::function<void(const std::uint8_t*, std::size_t)> videoNal;
    std::function<void(const std::uint8_t*, std::size_t)> opusPacket;
    std::array<std::string, 5> iceUrls{};
    std::array<std::string, 5> iceUsernames{};
    std::array<std::string, 5> iceCredentials{};
    bool inputChannelsOpened = false;

    static void onVideo(uint8_t* data, size_t size, void* userdata) {
        auto* self = static_cast<Impl*>(userdata);
        self->videoBytes += size;
        std::function<void(const std::uint8_t*, std::size_t)> callback;
        {
            std::lock_guard<std::mutex> lock(self->callbackMutex);
            callback = self->videoNal;
        }
        if (callback) {
            callback(data, size);
        }
    }

    static void onAudio(uint8_t* data, size_t size, void* userdata) {
        auto* self = static_cast<Impl*>(userdata);
        self->audioBytes += size;
        std::function<void(const std::uint8_t*, std::size_t)> callback;
        {
            std::lock_guard<std::mutex> lock(self->callbackMutex);
            callback = self->opusPacket;
        }
        if (callback) {
            callback(data, size);
        }
    }

    static void onKeyframe(void*) {}
#else
    std::size_t videoBytes = 0;
    std::size_t audioBytes = 0;
#endif
    std::string peerName = defaultPeerName();
    std::vector<QueuedIceCandidate> queuedRemoteCandidates;
};

GfnWebRtcClient::GfnWebRtcClient() : impl_(new Impl()) {}

GfnWebRtcClient::~GfnWebRtcClient() {
    close();
    delete impl_;
}

GfnWebRtcStartResult GfnWebRtcClient::startFromRemoteOffer(const GfnWebRtcConfig& config, const std::string& offerSdp) {
#if defined(OPENNOW_HAS_LIBDATACHANNEL)
    if (offerSdp.empty()) {
        return {false, {}, "remote WebRTC offer SDP is empty"};
    }

    close();
    try {
        if (!impl_->rtcInitialized.exchange(true)) {
            rtc::InitLogger(rtc::LogLevel::Info, [](rtc::LogLevel, std::string message) {
                appendRtcLog("[RTC] " + message);
            });
        }

        impl_->closing = false;
        impl_->videoBytes = 0;
        impl_->audioBytes = 0;
        impl_->inputChannelsOpened = false;
        impl_->inputReady = false;
        impl_->remoteTracks.clear();
        impl_->heartbeatStop = true;
        impl_->remoteMids = mediaMidsFromSdp(offerSdp);
        impl_->partialReliableThresholdMs =
            std::clamp(parseRiIntegerAttribute(offerSdp, "ri.partialReliableThresholdMs", 300), 1u, 5000u);
        impl_->hidDeviceMask = parseRiIntegerAttribute(offerSdp, "ri.hidDeviceMask", 0xffffffffu);
        impl_->enablePartiallyReliableTransferGamepad =
            parseRiIntegerAttribute(offerSdp, "ri.enablePartiallyReliableTransferGamepad", 0x0fu);
        impl_->enablePartiallyReliableTransferHid =
            parseRiIntegerAttribute(offerSdp, "ri.enablePartiallyReliableTransferHid", 0xffffffffu);
#if defined(OPENNOW_PLATFORM_SWITCH)
        appendRtcLog("[RTC] Switch input transport: matching OpenNOW desktop reliable + partially reliable datachannels.");
#endif
        {
            std::lock_guard<std::mutex> lock(impl_->localCandidateMutex);
            impl_->localCandidates.clear();
        }
        {
            std::lock_guard<std::mutex> lock(impl_->descriptionMutex);
            impl_->localDescription.clear();
            impl_->localDescriptionReady = false;
        }

        rtc::Configuration rtcConfig;
        rtcConfig.forceMediaTransport = true;
        rtcConfig.disableAutoGathering = false;
        rtcConfig.disableAutoNegotiation = true;
        rtcConfig.disableFingerprintVerification = true;
        rtcConfig.certificateType = rtc::CertificateType::Ecdsa;
        rtcConfig.portRangeBegin = 16000;
        rtcConfig.portRangeEnd = 16999;
        for (const auto& server : config.session.iceServers) {
            for (const auto& url : server.urls) {
                if (url.empty()) {
                    continue;
                }
                try {
                    if (!server.username.empty() || !server.credential.empty()) {
                        // libdatachannel parses TURN credentials only through the structured constructor.
                        // GFN currently gives host candidates, but keep STUN/TURN URLs available when present.
                        rtcConfig.iceServers.emplace_back(url);
                    } else {
                        rtcConfig.iceServers.emplace_back(url);
                    }
                } catch (...) {
                }
            }
        }
        if (rtcConfig.iceServers.empty()) {
            rtcConfig.iceServers.emplace_back("stun:stun.gamestream.nvidia.com:19302");
        }

        auto pc = std::make_shared<rtc::PeerConnection>(rtcConfig);
        impl_->pc = pc;
        impl_->state = rtc::PeerConnection::State::New;
        impl_->iceState = rtc::PeerConnection::IceState::New;
        impl_->gatheringState = rtc::PeerConnection::GatheringState::New;

        auto* self = impl_;
        pc->onStateChange([self](rtc::PeerConnection::State state) {
            if (!self->closing.load()) {
                self->state = state;
                appendRtcLog("[RTC] Peer state=" + stateText(state));
            }
        });
        pc->onIceStateChange([self](rtc::PeerConnection::IceState state) {
            if (!self->closing.load()) {
                self->iceState = state;
                appendRtcLog("[RTC] ICE state=" + iceStateText(state));
            }
        });
        pc->onGatheringStateChange([self](rtc::PeerConnection::GatheringState state) {
            if (!self->closing.load()) {
                self->gatheringState = state;
                appendRtcLog("[RTC] ICE gathering state=" + gatheringStateText(state));
                self->descriptionCv.notify_all();
            }
        });
        pc->onLocalDescription([self](rtc::Description description) {
            if (self->closing.load()) {
                return;
            }
            {
                std::lock_guard<std::mutex> lock(self->descriptionMutex);
                self->localDescription = static_cast<std::string>(description);
                self->localDescriptionReady = true;
            }
            self->descriptionCv.notify_all();
        });
        pc->onLocalCandidate([self](rtc::Candidate candidate) {
            if (self->closing.load()) {
                return;
            }
            const auto candidateText = candidate.candidate();
            if (candidateText.empty() || isTcpIceCandidate(candidateText)) {
                return;
            }
            Impl::PendingIceCandidate pending;
            pending.candidate = candidateText;
            pending.mid = candidate.mid();
            pending.mLineIndex = midToMLineIndex(pending.mid, self->remoteMids);
            {
                std::lock_guard<std::mutex> lock(self->localCandidateMutex);
                self->localCandidates.push_back(std::move(pending));
            }
            appendRtcLog("[RTC] Local ICE candidate queued.");
        });
        pc->onTrack([self](std::shared_ptr<rtc::Track> track) {
            if (self->closing.load()) {
                return;
            }
            self->remoteTracks.push_back(track);
            const auto desc = track->description().description();
            const auto descLower = lower(desc);
            if (descLower.find("h264") != std::string::npos || track->description().type() == "video") {
                track->setMediaHandler(std::make_shared<rtc::H264RtpDepacketizer>(rtc::NalUnit::Separator::StartSequence));
                track->chainMediaHandler(std::make_shared<rtc::RtcpReceivingSession>());
                track->onFrame([self](rtc::binary frame, rtc::FrameInfo) {
                    self->dispatchVideo(frame);
                });
            } else if (descLower.find("opus") != std::string::npos || track->description().type() == "audio") {
                track->setMediaHandler(std::make_shared<rtc::OpusRtpDepacketizer>());
                track->chainMediaHandler(std::make_shared<rtc::RtcpReceivingSession>());
                track->onFrame([self](rtc::binary frame, rtc::FrameInfo) {
                    self->dispatchAudio(frame);
                });
            }
        });
        pc->onDataChannel([self](std::shared_ptr<rtc::DataChannel> channel) {
            if (self->closing.load() || !channel) {
                return;
            }
            appendRtcLog("[RTC] Remote datachannel received: " + channel->label());
            if (channel->label() != "control_channel") {
                return;
            }
            self->controlChannel = channel;
            channel->onOpen([] {
                appendRtcLog("[RTC] Control datachannel open.");
            });
            channel->onClosed([self] {
                if (!self->closing.load()) {
                    appendRtcLog("[RTC] Control datachannel closed.");
                }
            });
            channel->onError([](std::string error) {
                appendRtcLog("[RTC] Control datachannel error: " + error);
            });
            channel->onMessage([self](rtc::message_variant data) {
                self->handleControlMessage(data);
            });
        });

        (void)impl_->createInputDataChannels(impl_->partialReliableThresholdMs);
        pc->setRemoteDescription(rtc::Description(offerSdp, rtc::Description::Type::Offer));
        pc->setLocalDescription(rtc::Description::Type::Answer);

        std::string finalDescription;
        {
            std::unique_lock<std::mutex> lock(impl_->descriptionMutex);
            const auto ready = impl_->descriptionCv.wait_for(lock, std::chrono::seconds(8), [this] {
                return impl_->localDescriptionReady;
            });
            if (!ready || impl_->localDescription.empty()) {
                close();
                return {false, {}, "libdatachannel did not create a local SDP answer"};
            }

            // Match OpenNOW desktop/Chromium behavior: don't send the answer immediately after
            // createAnswer. Wait briefly for ICE gathering, then send pc.localDescription with
            // gathered candidates included.
            (void)impl_->descriptionCv.wait_for(lock, std::chrono::seconds(5), [this] {
                return impl_->gatheringState.load() == rtc::PeerConnection::GatheringState::Complete;
            });
        }

        if (auto local = pc->localDescription()) {
            finalDescription = static_cast<std::string>(*local);
        }
        if (finalDescription.empty()) {
            std::lock_guard<std::mutex> lock(impl_->descriptionMutex);
            finalDescription = impl_->localDescription;
        }
        return {true, finalDescription, {}};
    } catch (const std::exception& e) {
        close();
        return {false, {}, std::string("libdatachannel failed to start: ") + e.what()};
    }
#elif defined(OPENNOW_HAS_LIBPEER)
    if (offerSdp.empty()) {
        return {false, {}, "remote WebRTC offer SDP is empty"};
    }

    close();
    impl_->videoBytes = 0;
    impl_->audioBytes = 0;
    impl_->inputChannelsOpened = false;
    std::fill(impl_->iceUrls.begin(), impl_->iceUrls.end(), std::string{});
    std::fill(impl_->iceUsernames.begin(), impl_->iceUsernames.end(), std::string{});
    std::fill(impl_->iceCredentials.begin(), impl_->iceCredentials.end(), std::string{});
    if (peer_init() != 0) {
        return {false, {}, "libpeer initialization failed"};
    }
    impl_->peerInitialized = true;

    PeerConfiguration peerConfig{};
    peerConfig.video_codec = CODEC_H264;
    peerConfig.audio_codec = CODEC_OPUS;
    peerConfig.datachannel = DATA_CHANNEL_BINARY;
    peerConfig.onvideotrack = &Impl::onVideo;
    peerConfig.onaudiotrack = &Impl::onAudio;
    peerConfig.on_request_keyframe = &Impl::onKeyframe;
    peerConfig.user_data = impl_;

    std::size_t iceIndex = 0;
    for (const auto& server : config.session.iceServers) {
        if (iceIndex >= std::size(peerConfig.ice_servers)) {
            break;
        }
        if (server.urls.empty()) {
            continue;
        }
        impl_->iceUrls[iceIndex] = server.urls.front();
        impl_->iceUsernames[iceIndex] = server.username;
        impl_->iceCredentials[iceIndex] = server.credential;
        peerConfig.ice_servers[iceIndex].urls = impl_->iceUrls[iceIndex].c_str();
        peerConfig.ice_servers[iceIndex].username = impl_->iceUsernames[iceIndex].empty() ? nullptr : impl_->iceUsernames[iceIndex].c_str();
        peerConfig.ice_servers[iceIndex].credential = impl_->iceCredentials[iceIndex].empty() ? nullptr : impl_->iceCredentials[iceIndex].c_str();
        ++iceIndex;
    }
    if (iceIndex == 0) {
        impl_->iceUrls[0] = "stun:stun.gamestream.nvidia.com:19302";
        peerConfig.ice_servers[0].urls = impl_->iceUrls[0].c_str();
    }

    impl_->pc = peer_connection_create(&peerConfig);
    if (!impl_->pc) {
        close();
        return {false, {}, "peer_connection_create failed"};
    }

    peer_connection_set_remote_description(impl_->pc, offerSdp.c_str(), SDP_TYPE_OFFER);
    const char* answer = peer_connection_create_answer(impl_->pc);
    if (!answer || !*answer) {
        close();
        return {false, {}, "libpeer failed to create SDP answer"};
    }

    return {true, answer, {}};
#else
    (void)config;
    (void)offerSdp;
    return {false, {}, "OpenNOW was built without libpeer native WebRTC"};
#endif
}

GfnWebRtcSignalingResult GfnWebRtcClient::connectWithGfnSignaling(const GfnWebRtcSignalingConfig& config) {
    auto log = [&](const std::string& message) {
        if (config.log) {
            config.log(message);
        }
    };

#if defined(OPENNOW_HAS_LIBDATACHANNEL) || defined(OPENNOW_HAS_LIBPEER)
    {
        std::lock_guard<std::mutex> lock(impl_->callbackMutex);
        impl_->videoNal = config.videoNal;
        impl_->opusPacket = config.opusPacket;
    }
#else
    (void)config;
#endif

    if (config.session.sessionId.empty()) {
        return {false, 0, {}, {}, {}, {}, "GFN signaling requires a CloudMatch session id"};
    }
    if (config.session.signalingUrl.empty() && config.session.signalingServer.empty()) {
        return {false, 0, {}, {}, {}, {}, "GFN signaling requires a signaling URL or server"};
    }

    const auto signInUrl = signalingSignInUrl(config.session, impl_->peerName);
    const auto protocol = "x-nv-sessionid." + config.session.sessionId;
    net::WssClient socket;
    log("[INFO][SIGNALING] Connecting WSS " + signInUrl);
    const auto connected = socket.connect({
        .url = signInUrl,
        .protocol = protocol,
        .origin = "https://play.geforcenow.com",
        .userAgent = "Mozilla/5.0 (X11; Linux x86_64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/131 Safari/537.36",
        .verifyPeer = false,
        .timeout = std::chrono::milliseconds(10000),
    });
    if (!connected.ok) {
        return {false, 0, {}, {}, {}, {}, "GFN WSS connect failed: " + connected.error};
    }

    std::uint32_t ackCounter = 1;
    std::uint32_t localPeerId = 0;
    std::uint32_t remotePeerId = 1;
    std::string remotePeerIdText = "1";
    GfnWebRtcSignalingResult result;
    result.ok = false;

    auto send = [&](const std::string& json, const char* label) -> bool {
        const auto sent = socket.sendText(json);
        if (!sent.ok) {
            result.error = std::string("GFN WSS send failed for ") + label + ": " + sent.error;
            log("[ERROR][SIGNALING] " + result.error);
            return false;
        }
        return true;
    };

    auto drainLocalCandidates = [&]() -> bool {
#if defined(OPENNOW_HAS_LIBDATACHANNEL)
        if (!result.ok || localPeerId == 0 || remotePeerId == 0) {
            return true;
        }
        std::vector<Impl::PendingIceCandidate> pending;
        {
            std::lock_guard<std::mutex> lock(impl_->localCandidateMutex);
            pending.swap(impl_->localCandidates);
        }
        for (const auto& candidate : pending) {
            log("[INFO][SIGNALING] Sending local ICE candidate: " + compactLine(candidate.candidate, 160));
            if (!send(candidatePeerMessageJson(ackCounter++, localPeerId, remotePeerId, candidate.candidate, candidate.mid, candidate.mLineIndex), "local-ice")) {
                return false;
            }
        }
#endif
        return true;
    };

    auto addRemoteCandidateForMid = [&](const std::string& candidate, const std::string& mid) -> bool {
#if defined(OPENNOW_HAS_LIBDATACHANNEL)
        if (!impl_->pc || candidate.empty()) {
            return false;
        }
        try {
            const auto selectedMid = mid.empty()
                ? (impl_->remoteMids.empty() ? std::string("0") : impl_->remoteMids.front())
                : mid;
            rtc::Candidate c(candidate, selectedMid);
            impl_->pc->addRemoteCandidate(c);
            return true;
        } catch (const std::exception& e) {
            log("[WARN][SIGNALING] addRemoteCandidate failed for mid=" + mid + ": " + e.what());
            return false;
        } catch (...) {
            log("[WARN][SIGNALING] addRemoteCandidate failed for mid=" + mid + ".");
            return false;
        }
#else
        (void)mid;
        return addRemoteCandidate(candidate);
#endif
    };

    if (!send(peerInfoJson(ackCounter++, 0, impl_->peerName, config.settings), "peer_info")) {
        socket.close();
        return result;
    }
    log("[INFO][SIGNALING] Sent peer_info for " + impl_->peerName + ".");

    const auto started = std::chrono::steady_clock::now();
    auto nextHeartbeat = started + std::chrono::seconds(5);
    auto nextProgressLog = started + std::chrono::seconds(1);
    std::uint32_t messages = 0;

    while (std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started).count() < config.timeoutMs) {
        for (int i = 0; i < 3; ++i) {
            pumpOnce();
        }
        if (!drainLocalCandidates()) {
            socket.close();
            return result;
        }

        if (result.ok && std::chrono::steady_clock::now() >= nextProgressLog) {
            std::string mediaLine = "[INFO][MEDIA] Awaiting GFN media: state=" + connectionState()
                + " video=" + std::to_string(videoBytesReceived())
                + " audio=" + std::to_string(audioBytesReceived());
#if defined(OPENNOW_HAS_LIBPEER)
            mediaLine += " " + iceStatsText(impl_->pc);
#elif defined(OPENNOW_HAS_LIBDATACHANNEL)
            rtc::Candidate local;
            rtc::Candidate remote;
            if (impl_->pc && impl_->pc->getSelectedCandidatePair(&local, &remote)) {
                mediaLine += " pair=" + static_cast<std::string>(local)
                    + " -> " + static_cast<std::string>(remote);
            }
#endif
            mediaLine += ".";
            log(mediaLine);
            if (mediaConnected()) {
                (void)openInputChannels(16);
            }
            nextProgressLog = std::chrono::steady_clock::now() + std::chrono::seconds(1);
        }

        if (result.ok && (videoBytesReceived() > 0 || audioBytesReceived() > 0)) {
            socket.close();
            log("[INFO][SIGNALING] GFN media started; handing stream loop to native pipeline. Peer state="
                + connectionState() + ".");
            return result;
        }

        if (std::chrono::steady_clock::now() >= nextHeartbeat) {
            if (!send(heartbeatJson(), "heartbeat")) {
                socket.close();
                return result;
            }
            nextHeartbeat = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        }

        std::string text;
        const auto received = socket.receiveText(text, std::chrono::milliseconds(250));
        if (!received.ok) {
            const auto err = timeoutText(received.error);
            if (err.find("timed out") != std::string::npos || err.find("timeout") != std::string::npos) {
                continue;
            }
            result.error = "GFN WSS receive failed: " + received.error;
            log("[ERROR][SIGNALING] " + result.error);
            socket.close();
            return result;
        }
        ++messages;

        const auto parsed = util::parseJson(text);
        if (!parsed.ok || !parsed.value.isObject()) {
            log("[WARN][SIGNALING] Ignoring non-JSON signaling packet: " + trim(text.substr(0, 120)));
            continue;
        }

        if (const auto ackId = jsonU32(&parsed.value, "ackid"); ackId != 0) {
            (void)send(ackJson(ackId), "ack");
        }
        if (jsonU32(&parsed.value, "hb") != 0) {
            (void)send(heartbeatJson(), "heartbeat-reply");
            continue;
        }

        if (const auto* peerInfo = parsed.value.get("peer_info"); peerInfo && peerInfo->isObject()) {
            const auto id = jsonU32(peerInfo, "id");
            const auto name = jsonString(peerInfo, "name");
            if (name == impl_->peerName && id != 0) {
                localPeerId = id;
                result.localPeerId = id;
                log("[INFO][SIGNALING] Local peer id assigned: " + std::to_string(localPeerId) + ".");
            }
        }

        const auto* peerMsg = parsed.value.get("peer_msg");
        if (!peerMsg || !peerMsg->isObject()) {
            if (result.ok && (mediaConnected() || videoBytesReceived() > 0 || audioBytesReceived() > 0)) {
                for (int i = 0; i < 60; ++i) {
                    pumpOnce();
                    if (mediaConnected()) {
                        break;
                    }
                    std::this_thread::sleep_for(std::chrono::milliseconds(16));
                }
                socket.close();
                log("[INFO][SIGNALING] Answer exchange completed after " + std::to_string(messages)
                    + " signaling messages. Peer state=" + connectionState() + ".");
                return result;
            }
            continue;
        }

        remotePeerId = jsonU32(peerMsg, "from", remotePeerId);
        remotePeerIdText = std::to_string(remotePeerId);
        result.remotePeerId = remotePeerIdText;
        const auto rawPeerPayload = jsonString(peerMsg, "msg");
        if (rawPeerPayload.empty()) {
            continue;
        }
        if (rawPeerPayload == "BYE") {
            result.error = "GFN server closed signaling with BYE";
            log("[ERROR][SIGNALING] " + result.error);
            socket.close();
            return result;
        }

        const auto peerPayload = util::parseJson(rawPeerPayload);
        if (!peerPayload.ok || !peerPayload.value.isObject()) {
            log("[WARN][SIGNALING] Ignoring non-JSON peer message.");
            continue;
        }

        const auto type = jsonString(&peerPayload.value, "type");
        if (type == "offer") {
            auto offerSdp = jsonString(&peerPayload.value, "sdp");
            if (offerSdp.empty()) {
                result.error = "GFN offer did not contain SDP";
                log("[ERROR][SIGNALING] " + result.error);
                socket.close();
                return result;
            }

            log("[INFO][SIGNALING] Received GFN WebRTC offer SDP bytes=" + std::to_string(offerSdp.size()) + ".");
#if defined(OPENNOW_HAS_LIBDATACHANNEL)
            appendRtcSdpBlock("GFN REMOTE OFFER ORIGINAL", offerSdp);
#endif
            const auto mediaIp = config.session.mediaConnectionInfo.ip.empty()
                ? config.session.serverIp
                : config.session.mediaConnectionInfo.ip;
            if (!mediaIp.empty()) {
                offerSdp = gfn::fixServerIp(offerSdp, mediaIp);
            }
            offerSdp = gfn::duplicateSessionWebrtcAttributesToMedia(offerSdp);
#if defined(OPENNOW_HAS_LIBDATACHANNEL)
            log("[INFO][SIGNALING] Keeping WebRTC application/datachannel m-line for GFN stream startup.");
#endif
            offerSdp = gfn::preferCodec(offerSdp, config.settings.codec);
            const auto candidateIp = endpointIpForCandidate(config.session);
            std::string manualCandidate;
            if (!candidateIp.empty() && config.session.mediaConnectionInfo.port > 0) {
                manualCandidate = "candidate:1 1 udp 2130706431 " + candidateIp + " "
                    + std::to_string(config.session.mediaConnectionInfo.port) + " typ host";
                offerSdp = normalizeSdpForLibpeer(offerSdp);
                log("[INFO][SIGNALING] Prepared GFN media ICE candidate " + candidateIp + ":"
                    + std::to_string(config.session.mediaConnectionInfo.port) + ".");
            } else {
                offerSdp = normalizeSdpForLibpeer(offerSdp);
            }
            log("[INFO][SIGNALING] Processed offer SDP: " + gfn::summarizeMediaTransportAttributes(offerSdp) + ".");
#if defined(OPENNOW_HAS_LIBDATACHANNEL)
            appendRtcSdpBlock("GFN REMOTE OFFER PROCESSED", offerSdp);
#endif

            const auto start = startFromRemoteOffer({.session = config.session}, offerSdp);
            if (!start.ok) {
                result.error = start.error;
                log("[ERROR][SIGNALING] " + result.error);
                socket.close();
                return result;
            }
#if defined(OPENNOW_HAS_LIBDATACHANNEL) || defined(OPENNOW_HAS_LIBPEER)
            {
                std::lock_guard<std::mutex> lock(impl_->callbackMutex);
                impl_->videoNal = config.videoNal;
                impl_->opusPacket = config.opusPacket;
            }
#endif

            auto answerSdp = gfn::mungeAnswerSdp(start.answerSdp, config.settings.bitrateKbps);
            gfn::NvstSdpParams nvstParams;
            nvstParams.width = config.settings.width;
            nvstParams.height = config.settings.height;
            nvstParams.fps = config.settings.fps;
            nvstParams.maxBitrateKbps = config.settings.bitrateKbps;
            nvstParams.codec = config.settings.codec;
#if defined(OPENNOW_HAS_LIBDATACHANNEL)
            nvstParams.partialReliableThresholdMs = impl_->partialReliableThresholdMs;
            nvstParams.hidDeviceMask = impl_->hidDeviceMask;
            nvstParams.enablePartiallyReliableTransferGamepad = impl_->enablePartiallyReliableTransferGamepad;
            nvstParams.enablePartiallyReliableTransferHid = impl_->enablePartiallyReliableTransferHid;
#endif
            const auto nvstSdp = gfn::buildNvstSdpForAnswer(nvstParams, answerSdp);
            if (nvstSdp.empty()) {
                result.error = "Local answer SDP did not contain credentials required for nvstSdp";
                log("[ERROR][SIGNALING] " + result.error);
                socket.close();
                return result;
            }
            log("[INFO][SIGNALING] Created SDP answer bytes=" + std::to_string(answerSdp.size())
                + " nvstSdp bytes=" + std::to_string(nvstSdp.size()) + ".");
#if defined(OPENNOW_HAS_LIBDATACHANNEL)
            appendRtcSdpBlock("GFN LOCAL ANSWER", answerSdp);
            appendRtcSdpBlock("GFN NVST SDP", nvstSdp);
#endif

            if (localPeerId == 0) {
                localPeerId = 1;
            }
            if (!send(answerPeerMessageJson(ackCounter++, localPeerId, remotePeerId, answerSdp, nvstSdp), "answer")) {
                socket.close();
                return result;
            }
            result.ok = true;
            result.offerSdp = offerSdp;
            result.answerSdp = answerSdp;
            result.nvstSdp = nvstSdp;
            result.localPeerId = localPeerId;
            result.remotePeerId = remotePeerIdText;
            log("[INFO][SIGNALING] Sent SDP answer peer_msg from=" + std::to_string(localPeerId)
                + " to=" + std::to_string(remotePeerId) + ".");

            if (!manualCandidate.empty()) {
                bool injectedManualCandidate = false;
#if defined(OPENNOW_HAS_LIBDATACHANNEL)
                std::array<std::string, 4> desktopMediaMids{"0", "1", "2", "3"};
                for (const auto& manualMid : desktopMediaMids) {
                    if (addRemoteCandidateForMid(manualCandidate, manualMid)) {
                        injectedManualCandidate = true;
                        log("[INFO][SIGNALING] Injected manual server ICE candidate after answer for mid " + manualMid + ".");
                        break;
                    }
                }
#else
                injectedManualCandidate = addRemoteCandidateForMid(manualCandidate, {});
#endif
                if (injectedManualCandidate) {
#if defined(OPENNOW_HAS_LIBPEER)
                    log("[INFO][MEDIA] " + iceStatsText(impl_->pc) + ".");
#elif defined(OPENNOW_HAS_LIBDATACHANNEL)
                    log("[INFO][MEDIA] ICE state=" + iceStateText(impl_->iceState.load()) + ".");
#endif
                } else {
                    log("[WARN][SIGNALING] Manual server ICE candidate injection failed.");
                }
            }
            for (const auto& candidate : impl_->queuedRemoteCandidates) {
                (void)addRemoteCandidateForMid(candidate.candidate, candidate.mid);
            }
            impl_->queuedRemoteCandidates.clear();
            continue;
        }

        const auto candidate = jsonString(&peerPayload.value, "candidate");
        if (!candidate.empty()) {
            log("[INFO][SIGNALING] Received remote ICE candidate: " + compactLine(candidate, 180));
            std::string mid;
#if defined(OPENNOW_HAS_LIBDATACHANNEL)
            mid = candidateMidFromPayload(peerPayload.value, impl_->remoteMids);
#endif
            if (!addRemoteCandidateForMid(candidate, mid)) {
                Impl::QueuedIceCandidate queued;
                queued.candidate = candidate;
                queued.mid = mid;
#if defined(OPENNOW_HAS_LIBDATACHANNEL)
                queued.mLineIndex = midToMLineIndex(mid, impl_->remoteMids);
#endif
                impl_->queuedRemoteCandidates.push_back(std::move(queued));
            } else {
#if defined(OPENNOW_HAS_LIBPEER)
                log("[INFO][MEDIA] " + iceStatsText(impl_->pc) + ".");
#elif defined(OPENNOW_HAS_LIBDATACHANNEL)
                log("[INFO][MEDIA] ICE state=" + iceStateText(impl_->iceState.load()) + ".");
#endif
            }
            continue;
        }

        log("[DEBUG][SIGNALING] Unhandled peer payload type=" + type + ".");
    }

    socket.close();
    if (result.ok) {
        if (mediaConnected() || videoBytesReceived() > 0 || audioBytesReceived() > 0) {
            log("[INFO][SIGNALING] GFN answer exchange completed. Peer state=" + connectionState() + ".");
            return result;
        }
        result.ok = false;
        result.error = "GFN answer was accepted, but WebRTC did not connect before timeout. state="
            + connectionState() + " video=" + std::to_string(videoBytesReceived())
            + " audio=" + std::to_string(audioBytesReceived());
        log("[ERROR][SIGNALING] " + result.error);
        return result;
    }
    result.error = "Timed out waiting for GFN WebRTC offer";
    return result;
}

bool GfnWebRtcClient::addRemoteCandidate(const std::string& candidate) {
#if defined(OPENNOW_HAS_LIBDATACHANNEL)
    if (!impl_->pc || candidate.empty()) {
        return false;
    }
    try {
        const auto mid = impl_->remoteMids.empty() ? std::string("0") : impl_->remoteMids.front();
        rtc::Candidate c(candidate, mid);
        impl_->pc->addRemoteCandidate(c);
        return true;
    } catch (...) {
        return false;
    }
#elif defined(OPENNOW_HAS_LIBPEER)
    if (!impl_->pc || candidate.empty()) {
        return false;
    }
    auto mutableCandidate = candidate;
    return peer_connection_add_ice_candidate(impl_->pc, mutableCandidate.data()) == 0;
#else
    (void)candidate;
    return false;
#endif
}

bool GfnWebRtcClient::pumpOnce() {
#if defined(OPENNOW_HAS_LIBDATACHANNEL)
    return impl_->pc != nullptr;
#elif defined(OPENNOW_HAS_LIBPEER)
    return impl_->pc && peer_connection_loop(impl_->pc) == 0;
#else
    return false;
#endif
}

bool GfnWebRtcClient::openInputChannels(std::uint32_t partialReliableThresholdMs) {
#if defined(OPENNOW_HAS_LIBDATACHANNEL)
    if (!impl_->pc) {
        return false;
    }
    if (impl_->inputChannelsOpened) {
        return true;
    }
    if (!mediaConnected()) {
        return false;
    }
    return impl_->createInputDataChannels(partialReliableThresholdMs);
#elif defined(OPENNOW_HAS_LIBPEER)
    if (!impl_->pc || impl_->inputChannelsOpened || !mediaConnected()) {
        return false;
    }
    char reliableLabel[] = "input_channel_v1";
    char partialLabel[] = "input_channel_partially_reliable";
    char protocol[] = "";
    const int reliable = peer_connection_create_datachannel_sid(
        impl_->pc,
        DATA_CHANNEL_RELIABLE,
        0,
        0,
        reliableLabel,
        protocol,
        0);
    const int partial = peer_connection_create_datachannel_sid(
        impl_->pc,
        DATA_CHANNEL_PARTIAL_RELIABLE_TIMED_UNORDERED,
        0,
        partialReliableThresholdMs,
        partialLabel,
        protocol,
        2);
    impl_->inputChannelsOpened = reliable == 0 || partial == 0;
    return impl_->inputChannelsOpened;
#else
    (void)partialReliableThresholdMs;
    return false;
#endif
}

bool GfnWebRtcClient::inputReady() const {
#if defined(OPENNOW_HAS_LIBDATACHANNEL)
    return impl_->inputReady.load();
#elif defined(OPENNOW_HAS_LIBPEER)
    return impl_->inputChannelsOpened;
#else
    return false;
#endif
}

bool GfnWebRtcClient::sendReliableInput(const gfn::Bytes& payload) {
#if defined(OPENNOW_HAS_LIBDATACHANNEL)
    return impl_->sendBytes(payload, false);
#else
    (void)payload;
    return false;
#endif
}

bool GfnWebRtcClient::sendPartiallyReliableInput(const gfn::Bytes& payload) {
#if defined(OPENNOW_HAS_LIBDATACHANNEL)
    return impl_->sendBytes(payload, true);
#else
    (void)payload;
    return false;
#endif
}

bool GfnWebRtcClient::sendGamepadInput(const gfn::GamepadInput& input, std::uint16_t connectedBitmap) {
#if defined(OPENNOW_HAS_LIBDATACHANNEL)
    const bool partiallyReliable = impl_->canUsePartiallyReliableForInput(gfn::INPUT_GAMEPAD, input.controllerId);
    auto bytes = impl_->inputEncoder.encodeGamepadState(input, connectedBitmap, partiallyReliable);
    return impl_->sendBytes(bytes, partiallyReliable);
#else
    (void)input;
    (void)connectedBitmap;
    return false;
#endif
}

bool GfnWebRtcClient::sendMouseMove(const gfn::MouseMovePayload& input) {
#if defined(OPENNOW_HAS_LIBDATACHANNEL)
    const bool partiallyReliable = impl_->canUsePartiallyReliableForInput(gfn::INPUT_MOUSE_REL);
    auto bytes = impl_->inputEncoder.encodeMouseMove(input);
    return impl_->sendBytes(bytes, partiallyReliable);
#else
    (void)input;
    return false;
#endif
}

bool GfnWebRtcClient::sendMouseButton(const gfn::MouseButtonPayload& input, bool pressed) {
#if defined(OPENNOW_HAS_LIBDATACHANNEL)
    auto bytes = pressed
        ? impl_->inputEncoder.encodeMouseButtonDown(input)
        : impl_->inputEncoder.encodeMouseButtonUp(input);
    return impl_->sendBytes(bytes, false);
#else
    (void)input;
    (void)pressed;
    return false;
#endif
}

std::string GfnWebRtcClient::connectionState() const {
#if defined(OPENNOW_HAS_LIBDATACHANNEL)
    if (!impl_->pc) {
        return "closed";
    }
    return stateText(impl_->state.load()) + "/" + iceStateText(impl_->iceState.load());
#elif defined(OPENNOW_HAS_LIBPEER)
    if (!impl_->pc) {
        return "closed";
    }
    const char* value = peer_connection_state_to_string(peer_connection_get_state(impl_->pc));
    return value ? value : "unknown";
#else
    return "unavailable";
#endif
}

bool GfnWebRtcClient::mediaConnected() const {
#if defined(OPENNOW_HAS_LIBDATACHANNEL)
    if (!impl_->pc) {
        return false;
    }
    const auto state = impl_->state.load();
    const auto ice = impl_->iceState.load();
    return state == rtc::PeerConnection::State::Connected
        || ice == rtc::PeerConnection::IceState::Connected
        || ice == rtc::PeerConnection::IceState::Completed;
#elif defined(OPENNOW_HAS_LIBPEER)
    if (!impl_->pc) {
        return false;
    }
    const auto state = peer_connection_get_state(impl_->pc);
    return state == PEER_CONNECTION_CONNECTED || state == PEER_CONNECTION_COMPLETED;
#else
    return false;
#endif
}

void GfnWebRtcClient::close() {
#if defined(OPENNOW_HAS_LIBDATACHANNEL)
    impl_->closing = true;
    impl_->inputReady = false;
    impl_->stopInputHeartbeat();
    {
        std::lock_guard<std::mutex> lock(impl_->descriptionMutex);
        impl_->localDescriptionReady = true;
    }
    impl_->descriptionCv.notify_all();
    {
        std::lock_guard<std::mutex> lock(impl_->callbackMutex);
        impl_->videoNal = nullptr;
        impl_->opusPacket = nullptr;
    }
    {
        std::lock_guard<std::mutex> lock(impl_->localCandidateMutex);
        impl_->localCandidates.clear();
    }

    auto reliableInput = std::move(impl_->reliableInput);
    auto partialInput = std::move(impl_->partialInput);
    auto controlChannel = std::move(impl_->controlChannel);
    auto remoteTracks = std::move(impl_->remoteTracks);
    auto pc = std::move(impl_->pc);

    if (reliableInput) {
        reliableInput->resetCallbacks();
        reliableInput->close();
    }
    if (partialInput) {
        partialInput->resetCallbacks();
        partialInput->close();
    }
    if (controlChannel) {
        controlChannel->resetCallbacks();
        controlChannel->close();
    }
    for (auto& track : remoteTracks) {
        if (track) {
            track->resetCallbacks();
            track->close();
        }
    }
    if (pc) {
        pc->resetCallbacks();
        pc->close();
    }
    impl_->state = rtc::PeerConnection::State::Closed;
    impl_->iceState = rtc::PeerConnection::IceState::Closed;
    impl_->inputChannelsOpened = false;
    impl_->inputReady = false;
#elif defined(OPENNOW_HAS_LIBPEER)
    if (impl_->pc) {
        peer_connection_close(impl_->pc);
        peer_connection_destroy(impl_->pc);
        impl_->pc = nullptr;
    }
    if (impl_->peerInitialized) {
        peer_deinit();
        impl_->peerInitialized = false;
    }
    {
        std::lock_guard<std::mutex> lock(impl_->callbackMutex);
        impl_->videoNal = nullptr;
        impl_->opusPacket = nullptr;
    }
#endif
}

std::size_t GfnWebRtcClient::videoBytesReceived() const {
    return impl_->videoBytes;
}

std::size_t GfnWebRtcClient::audioBytesReceived() const {
    return impl_->audioBytes;
}

} // namespace opennow::media
