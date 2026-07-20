#include "webrtc_session.hpp"
#include "stream/RemoteCandidatePolicy.hpp"
#include "borealis.hpp"
#include "stream/audio/AudioRtpUtils.hpp"
#include "stream/ffmpeg/AVFrameHolder.hpp"
#include "stream_diagnostics.hpp"
#include "video_quality_policy.hpp"
#include <jansson.h>
#include <algorithm>
#include <chrono>
#include <cctype>
#include <cstdio>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <mutex>
#include <random>
#include <set>
#include <sstream>
#include <thread>
#include <vector>

#ifdef __SWITCH__
#include <sys/stat.h>
#endif

namespace
{

constexpr const char* kNvdecActiveMarker =
    "sdmc:/switch/SwitchNOW/nvdec_active.marker";

bool ConsumeNvdecCrashMarker()
{
    std::ifstream marker(kNvdecActiveMarker, std::ios::binary);
    const bool exists = marker.good();
    marker.close();
    if (exists)
        std::remove(kNvdecActiveMarker);
    return exists;
}

bool CreateNvdecCrashMarker()
{
#ifdef __SWITCH__
    mkdir("sdmc:/switch", 0777);
    mkdir("sdmc:/switch/SwitchNOW", 0777);
#endif
    std::ofstream marker(kNvdecActiveMarker, std::ios::binary | std::ios::trunc);
    if (!marker.is_open())
        return false;
    marker << "NVDEC stream active; remove after clean shutdown\n";
    marker.flush();
    return marker.good();
}

bool StartsWith(const std::string& value, const char* prefix)
{
    return value.rfind(prefix, 0) == 0;
}

constexpr std::array<uint64_t, 9> kVideoLatencyThresholdsUs = {
    1000, 2000, 4000, 8000, 12000, 16000, 24000, 33000, 50000
};

void RecordLatency(std::array<std::atomic<uint64_t>, 10>& buckets, uint64_t latency_us)
{
    size_t bucket = 0;
    while (bucket < kVideoLatencyThresholdsUs.size() &&
           latency_us > kVideoLatencyThresholdsUs[bucket]) {
        ++bucket;
    }
    buckets[bucket].fetch_add(1, std::memory_order_relaxed);
}

uint64_t LatencyPercentile95(const std::array<std::atomic<uint64_t>, 10>& buckets)
{
    uint64_t total = 0;
    for (const auto& bucket : buckets)
        total += bucket.load(std::memory_order_relaxed);
    if (total == 0)
        return 0;

    const uint64_t target = (total * 95 + 99) / 100;
    uint64_t accumulated = 0;
    for (size_t i = 0; i < buckets.size(); ++i) {
        accumulated += buckets[i].load(std::memory_order_relaxed);
        if (accumulated >= target)
            return i < kVideoLatencyThresholdsUs.size() ? kVideoLatencyThresholdsUs[i] : 50001;
    }
    return 50001;
}

template <typename T>
void AtomicMax(std::atomic<T>& target, T value)
{
    T previous = target.load(std::memory_order_relaxed);
    while (previous < value &&
           !target.compare_exchange_weak(previous, value, std::memory_order_relaxed)) {
    }
}

std::mutex& StreamLogMutex()
{
    static std::mutex mutex;
    return mutex;
}

std::chrono::steady_clock::time_point& InputLogStartTime()
{
    static auto start = std::chrono::steady_clock::now();
    return start;
}

void AppendInputLog(const std::string& line)
{
    if (!opennow::StreamDiagnosticsEnabled())
        return;
#ifdef __SWITCH__
    mkdir("sdmc:/switch", 0777);
    mkdir("sdmc:/switch/SwitchNOW", 0777);
#endif
    const auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - InputLogStartTime()).count();
    std::lock_guard<std::mutex> lock(StreamLogMutex());
    std::ofstream stream("sdmc:/switch/SwitchNOW/input.log", std::ios::app);
    if (stream.is_open())
        stream << "[+" << elapsed_ms << "ms] " << line << '\n';
}

void AppendStreamLog(const std::string& line)
{
    if (!opennow::StreamDiagnosticsEnabled())
        return;
#ifdef __SWITCH__
    mkdir("sdmc:/switch", 0777);
    mkdir("sdmc:/switch/SwitchNOW", 0777);
#endif

    static const auto log_start = std::chrono::steady_clock::now();
    const auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - log_start).count();

    std::lock_guard<std::mutex> lock(StreamLogMutex());

    std::ofstream stream("sdmc:/switch/SwitchNOW/signaling.log", std::ios::app);
    if (stream.is_open())
        stream << "[+" << elapsed_ms << "ms] " << line << '\n';

    std::ofstream trace("sdmc:/switch/SwitchNOW/stream_trace.log", std::ios::app);
    if (trace.is_open())
        trace << "[+" << elapsed_ms << "ms] APP " << line << '\n';
}

void ResetStreamTraceLog()
{
    if (!opennow::StreamDiagnosticsEnabled())
        return;
#ifdef __SWITCH__
    mkdir("sdmc:/switch", 0777);
    mkdir("sdmc:/switch/SwitchNOW", 0777);
#endif
    std::lock_guard<std::mutex> lock(StreamLogMutex());
    {
        std::ofstream stream("sdmc:/switch/SwitchNOW/signaling.log", std::ios::trunc);
        if (stream.is_open()) {
            stream << "SwitchNOW signaling and media log\n";
            stream << "One file per stream attempt.\n";
            stream << "======================================\n";
        }
    }
    {
        std::ofstream stream("sdmc:/switch/SwitchNOW/stream_trace.log", std::ios::trunc);
        if (stream.is_open()) {
            stream << "SwitchNOW stream trace\n";
            stream << "One file per stream attempt. Safe to send for debugging.\n";
            stream << "=======================================================\n";
        }
    }
    {
        InputLogStartTime() = std::chrono::steady_clock::now();
        std::ofstream stream("sdmc:/switch/SwitchNOW/input.log", std::ios::trunc);
        if (stream.is_open()) {
            stream << "SwitchNOW input flight recorder\n";
            stream << "Controller samples -> Xbox encoding -> DataChannel -> SCTP result.\n";
            stream << "===============================================================\n";
        }
    }
}

void AppendTraceLog(const std::string& line)
{
    if (!opennow::StreamDiagnosticsEnabled())
        return;
#ifdef __SWITCH__
    mkdir("sdmc:/switch", 0777);
    mkdir("sdmc:/switch/SwitchNOW", 0777);
#endif

    static const auto trace_start = std::chrono::steady_clock::now();
    const auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - trace_start).count();

    std::lock_guard<std::mutex> lock(StreamLogMutex());
    std::ofstream stream("sdmc:/switch/SwitchNOW/stream_trace.log", std::ios::app);
    if (stream.is_open())
        stream << "[+" << elapsed_ms << "ms] " << line << '\n';
}

void AppendTraceBlock(const std::string& title, const std::string& body)
{
    if (!opennow::StreamDiagnosticsEnabled())
        return;
    AppendTraceLog("----- " + title + " BEGIN bytes=" + std::to_string(body.size()) + " -----");
    {
        std::lock_guard<std::mutex> lock(StreamLogMutex());
        std::ofstream stream("sdmc:/switch/SwitchNOW/stream_trace.log", std::ios::app);
        if (stream.is_open())
            stream << body << (body.empty() || body.back() == '\n' ? "" : "\n");
    }
    AppendTraceLog("----- " + title + " END -----");
}

std::string HexPreview(const uint8_t* data, size_t size, size_t max_bytes = 12)
{
    if (!data || size == 0)
        return "";

    std::ostringstream out;
    const size_t count = std::min(size, max_bytes);
    for (size_t i = 0; i < count; ++i) {
        if (i > 0)
            out << ' ';
        out << std::hex << std::setw(2) << std::setfill('0') << static_cast<int>(data[i]);
    }
    if (size > count)
        out << " ...";
    return out.str();
}

bool ContainsH264Idr(const uint8_t* data, size_t size)
{
    if (!data || size < 5)
        return false;

    for (size_t i = 0; i + 4 < size; ++i) {
        size_t header = 0;
        if (data[i] == 0 && data[i + 1] == 0 && data[i + 2] == 1)
            header = i + 3;
        else if (i + 4 < size && data[i] == 0 && data[i + 1] == 0 &&
                 data[i + 2] == 0 && data[i + 3] == 1)
            header = i + 4;

        if (header > 0 && header < size && (data[header] & 0x1f) == 5)
            return true;
    }
    return false;
}

std::string BuildSignInUrl(std::string base_url, const std::string& peer_name, const std::string& session_id)
{
    const size_t query_pos = base_url.find('?');
    if (query_pos != std::string::npos)
        base_url.erase(query_pos);

    if (StartsWith(base_url, "http://"))
        base_url.replace(0, 4, "ws");
    else if (StartsWith(base_url, "https://"))
        base_url.replace(0, 5, "wss");

    while (!base_url.empty() && base_url.back() == '/')
        base_url.pop_back();

    if (base_url.size() < 8 || base_url.substr(base_url.size() - 8) != "sign_in")
        base_url += "/sign_in";

    return base_url + "?peer_id=" + peer_name + "&version=2&peer_role=1&pairing_id=" + session_id;
}

std::string MakePeerName()
{
    std::random_device rd;
    const auto ticks = static_cast<unsigned long long>(
        std::chrono::steady_clock::now().time_since_epoch().count());
    return "peer-" + std::to_string((ticks ^ rd()) % 10000000000ULL);
}

std::string ReplaceAll(std::string value, const std::string& from, const std::string& to)
{
    size_t pos = 0;
    while ((pos = value.find(from, pos)) != std::string::npos) {
        value.replace(pos, from.size(), to);
        pos += to.size();
    }
    return value;
}

std::vector<std::string> SplitSdpLines(const std::string& sdp)
{
    std::vector<std::string> lines;
    size_t start = 0;

    while (start < sdp.size()) {
        size_t end = sdp.find("\r\n", start);
        if (end == std::string::npos)
            end = sdp.find('\n', start);

        if (end == std::string::npos) {
            lines.push_back(sdp.substr(start));
            break;
        }

        lines.push_back(sdp.substr(start, end - start));
        start = end + (sdp[end] == '\r' && end + 1 < sdp.size() && sdp[end + 1] == '\n' ? 2 : 1);
    }

    return lines;
}

std::string JoinSdpLines(const std::vector<std::string>& lines)
{
    std::string result;
    for (const auto& line : lines) {
        result += line;
        result += "\r\n";
    }
    return result;
}

bool StartsWithString(const std::string& value, const std::string& prefix)
{
    return value.rfind(prefix, 0) == 0;
}

void PutU16Le(std::vector<uint8_t>& out, uint16_t value)
{
    out.push_back(static_cast<uint8_t>(value & 0xff));
    out.push_back(static_cast<uint8_t>((value >> 8) & 0xff));
}

void PutI16Le(std::vector<uint8_t>& out, int16_t value)
{
    PutU16Le(out, static_cast<uint16_t>(value));
}

void PutU32Le(std::vector<uint8_t>& out, uint32_t value)
{
    out.push_back(static_cast<uint8_t>(value & 0xff));
    out.push_back(static_cast<uint8_t>((value >> 8) & 0xff));
    out.push_back(static_cast<uint8_t>((value >> 16) & 0xff));
    out.push_back(static_cast<uint8_t>((value >> 24) & 0xff));
}

void PutU64Le(std::vector<uint8_t>& out, uint64_t value)
{
    for (int i = 0; i < 8; ++i)
        out.push_back(static_cast<uint8_t>((value >> (i * 8)) & 0xff));
}

void PutU16Be(std::vector<uint8_t>& out, uint16_t value)
{
    out.push_back(static_cast<uint8_t>((value >> 8) & 0xff));
    out.push_back(static_cast<uint8_t>(value & 0xff));
}

void PutU32Be(std::vector<uint8_t>& out, uint32_t value)
{
    out.push_back(static_cast<uint8_t>((value >> 24) & 0xff));
    out.push_back(static_cast<uint8_t>((value >> 16) & 0xff));
    out.push_back(static_cast<uint8_t>((value >> 8) & 0xff));
    out.push_back(static_cast<uint8_t>(value & 0xff));
}

void PutU64Be(std::vector<uint8_t>& out, uint64_t value)
{
    for (int i = 7; i >= 0; --i)
        out.push_back(static_cast<uint8_t>((value >> (i * 8)) & 0xff));
}

std::vector<uint8_t> WrapSingleInput(int protocol_version, uint64_t timestamp_us,
                                     const std::vector<uint8_t>& payload)
{
    if (protocol_version <= 2)
        return payload;

    std::vector<uint8_t> wrapped;
    wrapped.reserve(10 + payload.size());
    wrapped.push_back(0x23);
    PutU64Be(wrapped, timestamp_us);
    wrapped.push_back(0x22);
    wrapped.insert(wrapped.end(), payload.begin(), payload.end());
    return wrapped;
}

std::vector<uint8_t> WrapReliableGamepad(int protocol_version, uint64_t timestamp_us,
                                         const std::vector<uint8_t>& payload)
{
    if (protocol_version <= 2)
        return payload;

    std::vector<uint8_t> wrapped;
    wrapped.reserve(12 + payload.size());
    wrapped.push_back(0x23);
    PutU64Be(wrapped, timestamp_us);
    wrapped.push_back(0x21);
    PutU16Be(wrapped, static_cast<uint16_t>(payload.size()));
    wrapped.insert(wrapped.end(), payload.begin(), payload.end());
    return wrapped;
}

std::vector<uint8_t> WrapPartiallyReliableGamepad(int protocol_version, uint64_t timestamp_us,
                                                  uint8_t controller_id, uint16_t sequence,
                                                  const std::vector<uint8_t>& payload)
{
    if (protocol_version <= 2)
        return payload;

    std::vector<uint8_t> wrapped;
    wrapped.reserve(16 + payload.size());
    wrapped.push_back(0x23);
    PutU64Be(wrapped, timestamp_us);
    wrapped.push_back(0x26);
    wrapped.push_back(controller_id);
    PutU16Be(wrapped, sequence);
    wrapped.push_back(0x21);
    PutU16Be(wrapped, static_cast<uint16_t>(payload.size()));
    wrapped.insert(wrapped.end(), payload.begin(), payload.end());
    return wrapped;
}

uint64_t NowUs()
{
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count());
}

int16_t AxisToI16(float value)
{
    value = std::max(-1.0f, std::min(1.0f, value));
    return static_cast<int16_t>(value * 32767.0f);
}

std::vector<uint8_t> BuildGamepadPayload(uint64_t timestamp_us, uint16_t buttons,
                                         uint8_t left_trigger, uint8_t right_trigger,
                                         int16_t lx, int16_t ly, int16_t rx, int16_t ry)
{
    std::vector<uint8_t> payload;
    payload.reserve(38);
    PutU32Le(payload, 12);
    PutU16Le(payload, 26);
    PutU16Le(payload, 0);
    PutU16Le(payload, 0x0101);
    PutU16Le(payload, 20);
    PutU16Le(payload, buttons);
    PutU16Le(payload, static_cast<uint16_t>(left_trigger | (right_trigger << 8)));
    PutI16Le(payload, lx);
    PutI16Le(payload, ly);
    PutI16Le(payload, rx);
    PutI16Le(payload, ry);
    PutU16Le(payload, 0);
    PutU16Le(payload, 85);
    PutU16Le(payload, 0);
    PutU64Le(payload, timestamp_us);
    return payload;
}

std::vector<uint8_t> BuildMouseButtonPayload(uint64_t timestamp_us, bool pressed)
{
    std::vector<uint8_t> payload;
    payload.reserve(18);
    PutU32Le(payload, pressed ? 8u : 9u);
    payload.push_back(1);
    payload.push_back(0);
    PutU32Be(payload, 0);
    PutU64Be(payload, timestamp_us);
    return payload;
}

std::vector<uint8_t> BuildMouseMovePayload(uint64_t timestamp_us, int16_t dx, int16_t dy)
{
    std::vector<uint8_t> payload;
    payload.reserve(22);
    PutU32Le(payload, 7);
    PutU16Be(payload, static_cast<uint16_t>(dx));
    PutU16Be(payload, static_cast<uint16_t>(dy));
    PutU16Be(payload, 0);
    PutU32Be(payload, 0);
    PutU64Be(payload, timestamp_us);
    return payload;
}

std::vector<uint8_t> BuildKeyboardPayload(uint64_t timestamp_us, uint16_t keycode,
                                          uint16_t scancode, uint16_t modifiers,
                                          bool pressed)
{
    std::vector<uint8_t> payload;
    payload.reserve(18);
    PutU32Le(payload, pressed ? 3u : 4u);
    PutU16Be(payload, keycode);
    PutU16Be(payload, modifiers);
    PutU16Be(payload, scancode);
    PutU64Be(payload, timestamp_us);
    return payload;
}

bool InputEncodingSelfTest()
{
    constexpr uint64_t timestamp = 0x0102030405060708ULL;
    const auto raw = BuildGamepadPayload(timestamp, 0x1234, 0x56, 0x78, 1, -2, 3, -4);
    if (raw.size() != 38 || raw[0] != 12 || raw[8] != 0x01 || raw[9] != 0x01 ||
        raw[24] != 0 || raw[25] != 0 || raw[26] != 0x55 || raw[27] != 0 ||
        raw[28] != 0 || raw[29] != 0 || raw[30] != 0x08 || raw[37] != 0x01) {
        return false;
    }

    const auto reliable = WrapReliableGamepad(3, timestamp, raw);
    if (reliable.size() != 50 || reliable[0] != 0x23 || reliable[9] != 0x21 ||
        reliable[10] != 0 || reliable[11] != 38 || reliable[12] != 12) {
        return false;
    }

    const auto partial = WrapPartiallyReliableGamepad(3, timestamp, 0, 1, raw);
    if (partial.size() != 54 || partial[0] != 0x23 || partial[9] != 0x26 ||
        partial[10] != 0 || partial[11] != 0 || partial[12] != 1 ||
        partial[13] != 0x21 || partial[14] != 0 || partial[15] != 38 || partial[16] != 12) {
        return false;
    }

    const auto mouse = BuildMouseButtonPayload(timestamp, true);
    const auto wrapped_mouse = WrapSingleInput(3, timestamp, mouse);
    const auto move = BuildMouseMovePayload(timestamp, 10, -5);
    const auto wrapped_move = WrapReliableGamepad(3, timestamp, move);
    const auto key = BuildKeyboardPayload(timestamp, 0x41, 0x1e, 0, true);
    return mouse.size() == 18 && mouse[0] == 8 && mouse[4] == 1 && mouse[17] == 0x08 &&
           wrapped_mouse.size() == 28 && wrapped_mouse[0] == 0x23 &&
           wrapped_mouse[9] == 0x22 && wrapped_mouse[10] == 8 &&
           move.size() == 22 && move[0] == 7 && move[4] == 0 && move[5] == 10 &&
           move[6] == 0xff && move[7] == 0xfb && wrapped_move.size() == 34 &&
           wrapped_move[9] == 0x21 && wrapped_move[10] == 0 && wrapped_move[11] == 22 &&
           key.size() == 18 && key[0] == 3 && key[4] == 0 && key[5] == 0x41 &&
           key[8] == 0 && key[9] == 0x1e;
}

int ParseIntegerAttribute(const std::string& sdp, const std::string& attribute, int fallback)
{
    const std::string prefix = "a=" + attribute + ":";
    for (const auto& line : SplitSdpLines(sdp)) {
        if (!StartsWithString(line, prefix))
            continue;

        const std::string raw = line.substr(prefix.size());
        char* end = nullptr;
        long value = 0;
        if (raw.rfind("0x", 0) == 0 || raw.rfind("0X", 0) == 0)
            value = std::strtol(raw.c_str() + 2, &end, 16);
        else
            value = std::strtol(raw.c_str(), &end, 10);

        if (end != raw.c_str())
            return static_cast<int>(value);
    }
    return fallback;
}

struct RiInputCapabilities {
    int partial_reliable_threshold_ms = 16;
    uint32_t hid_device_mask = 0xffffffffu;
    uint32_t partial_reliable_gamepad_mask = 0x0fu;
    uint32_t partial_reliable_hid_mask = 0xffffffffu;
};

RiInputCapabilities ParseRiInputCapabilities(const std::string& offer_sdp)
{
    RiInputCapabilities caps;
    const int threshold = ParseIntegerAttribute(offer_sdp, "ri.partialReliableThresholdMs", caps.partial_reliable_threshold_ms);
    if (threshold > 0)
        caps.partial_reliable_threshold_ms = std::max(1, std::min(5000, threshold));
    caps.hid_device_mask = static_cast<uint32_t>(ParseIntegerAttribute(
        offer_sdp, "ri.hidDeviceMask", static_cast<int>(caps.hid_device_mask)));
    caps.partial_reliable_gamepad_mask = static_cast<uint32_t>(ParseIntegerAttribute(
        offer_sdp, "ri.enablePartiallyReliableTransferGamepad", static_cast<int>(caps.partial_reliable_gamepad_mask)));
    caps.partial_reliable_hid_mask = static_cast<uint32_t>(ParseIntegerAttribute(
        offer_sdp, "ri.enablePartiallyReliableTransferHid", static_cast<int>(caps.partial_reliable_hid_mask)));
    return caps;
}

int ExtractRtpmapPayloadType(const std::string& line, const char* codec)
{
    constexpr const char* prefix = "a=rtpmap:";
    if (!StartsWith(line, prefix))
        return 0;

    const size_t pt_start = std::strlen(prefix);
    const size_t space = line.find(' ', pt_start);
    if (space == std::string::npos)
        return 0;

    const std::string codec_prefix = std::string(codec) + "/";
    const std::string codec_value = line.substr(space + 1);
    if (!StartsWithString(codec_value, codec_prefix))
        return 0;

    char* end = nullptr;
    long pt = std::strtol(line.c_str() + pt_start, &end, 10);
    if (pt <= 0 || pt > 127)
        return 0;

    return static_cast<int>(pt);
}

std::string FindFmtpForPayload(const std::vector<std::string>& lines, int payload_type)
{
    const std::string prefix = "a=fmtp:" + std::to_string(payload_type);
    for (const auto& line : lines) {
        if (StartsWithString(line, prefix))
            return line;
    }
    return "";
}

std::vector<std::string> FindRtcpFbForPayload(const std::vector<std::string>& lines, int payload_type)
{
    std::vector<std::string> feedback;
    const std::string prefix = "a=rtcp-fb:" + std::to_string(payload_type) + " ";
    for (const auto& line : lines) {
        if (StartsWithString(line, prefix))
            feedback.push_back(line);
    }
    return feedback;
}

int SelectOfferH264PayloadType(const std::string& offer_sdp)
{
    const std::vector<std::string> lines = SplitSdpLines(offer_sdp);
    std::vector<int> h264_payloads;
    bool in_video = false;

    for (const auto& line : lines) {
        if (StartsWith(line, "m="))
            in_video = StartsWith(line, "m=video");

        if (!in_video)
            continue;

        int pt = ExtractRtpmapPayloadType(line, "H264");
        if (pt > 0)
            h264_payloads.push_back(pt);
    }

    for (int pt : h264_payloads) {
        const std::string fmtp = FindFmtpForPayload(lines, pt);
        if (fmtp.find("packetization-mode=1") != std::string::npos)
            return pt;
    }

    return h264_payloads.empty() ? 96 : h264_payloads.front();
}

int ExtractOfferMediaPort(const std::string& offer_sdp, const char* media_name)
{
    const std::string prefix = std::string("m=") + media_name + " ";
    for (const auto& line : SplitSdpLines(offer_sdp)) {
        if (!StartsWithString(line, prefix))
            continue;

        const size_t port_start = prefix.size();
        const size_t port_end = line.find(' ', port_start);
        const std::string port_text = line.substr(
            port_start,
            port_end == std::string::npos ? std::string::npos : port_end - port_start);

        char* end = nullptr;
        const long port = std::strtol(port_text.c_str(), &end, 10);
        if (end != port_text.c_str() && port > 0 && port <= 65535)
            return static_cast<int>(port);
    }

    return 0;
}

int CountSdpLinesWithPrefix(const std::string& sdp, const std::string& prefix)
{
    int count = 0;
    for (const auto& line : SplitSdpLines(sdp)) {
        if (StartsWithString(line, prefix))
            ++count;
    }
    return count;
}

std::string PreviewText(const std::string& value, size_t max_chars = 160)
{
    if (value.size() <= max_chars)
        return value;
    return value.substr(0, max_chars) + "...";
}

std::string CompactSignalingMessage(const std::string& msg)
{
    json_error_t error;
    json_t* root = json_loads(msg.c_str(), 0, &error);
    if (!root)
        return "RX invalid-json " + PreviewText(msg, 120);

    std::string summary = "RX message";
    json_t* ack = json_object_get(root, "ack");
    if (ack && json_is_integer(ack)) {
        summary = "RX ack=" + std::to_string(json_integer_value(ack));
    } else if (json_object_get(root, "hb")) {
        summary = "RX hb";
    } else if (json_t* peer_info = json_object_get(root, "peer_info"); peer_info && json_is_object(peer_info)) {
        const char* name = json_string_value(json_object_get(peer_info, "name"));
        json_t* id = json_object_get(peer_info, "id");
        summary = "RX peer_info";
        if (name)
            summary += " name=" + std::string(name);
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
                    summary = "RX candidate " + PreviewText(candidate, 120);
                } else {
                    summary = "RX peer_msg " + PreviewText(raw_payload, 120);
                }
                json_decref(payload);
            } else {
                summary = "RX peer_msg raw " + PreviewText(raw_payload, 120);
            }
        }
    }

    json_decref(root);
    return summary;
}

std::string MediaKindFromMLine(const std::string& line)
{
    if (StartsWithString(line, "m=audio "))
        return "audio";
    if (StartsWithString(line, "m=video "))
        return "video";
    if (StartsWithString(line, "m=application "))
        return "application";
    return "";
}

std::vector<std::string> ExtractOfferMediaOrder(const std::string& offer_sdp)
{
    std::vector<std::string> order;
    for (const auto& line : SplitSdpLines(offer_sdp)) {
        const std::string media = MediaKindFromMLine(line);
        if (!media.empty())
            order.push_back(media);
    }
    return order;
}

std::string ExtractOfferMid(const std::string& offer_sdp, const std::string& media)
{
    bool in_media = false;
    for (const auto& line : SplitSdpLines(offer_sdp)) {
        if (StartsWithString(line, "m="))
            in_media = MediaKindFromMLine(line) == media;
        if (in_media && StartsWithString(line, "a=mid:"))
            return line.substr(std::strlen("a=mid:"));
    }
    return "";
}

std::string ExtractOfferBundleGroup(const std::string& offer_sdp)
{
    for (const auto& line : SplitSdpLines(offer_sdp)) {
        if (StartsWithString(line, "a=group:BUNDLE "))
            return line;
    }
    return "";
}

struct SdpMediaSection {
    std::string media;
    std::vector<std::string> lines;
};

std::string AlignAnswerSdpToOffer(const std::string& answer_sdp, const std::string& offer_sdp)
{
    std::vector<std::string> session_lines;
    std::vector<SdpMediaSection> sections;
    std::vector<std::string> candidates;
    std::set<std::string> seen_candidates;

    for (const auto& line : SplitSdpLines(answer_sdp)) {
        if (StartsWithString(line, "m=")) {
            sections.push_back({MediaKindFromMLine(line), {line}});
            continue;
        }

        if (StartsWithString(line, "a=candidate:")) {
            if (seen_candidates.insert(line).second)
                candidates.push_back(line);
            continue;
        }

        if (sections.empty())
            session_lines.push_back(line);
        else
            sections.back().lines.push_back(line);
    }

    const std::string offer_bundle = ExtractOfferBundleGroup(offer_sdp);
    bool bundle_replaced = false;
    for (auto& line : session_lines) {
        if (!offer_bundle.empty() && StartsWithString(line, "a=group:BUNDLE ")) {
            line = offer_bundle;
            bundle_replaced = true;
        }
    }
    if (!offer_bundle.empty() && !bundle_replaced)
        session_lines.push_back(offer_bundle);

    for (auto& section : sections) {
        const std::string offer_mid = ExtractOfferMid(offer_sdp, section.media);
        if (offer_mid.empty())
            continue;

        bool mid_replaced = false;
        for (auto& line : section.lines) {
            if (StartsWithString(line, "a=mid:")) {
                line = "a=mid:" + offer_mid;
                mid_replaced = true;
                break;
            }
        }
        if (!mid_replaced && section.lines.size() > 1)
            section.lines.insert(section.lines.begin() + 1, "a=mid:" + offer_mid);
    }

    std::vector<SdpMediaSection> ordered;
    std::vector<bool> used(sections.size(), false);
    for (const auto& media : ExtractOfferMediaOrder(offer_sdp)) {
        for (size_t i = 0; i < sections.size(); ++i) {
            if (!used[i] && sections[i].media == media) {
                ordered.push_back(sections[i]);
                used[i] = true;
                break;
            }
        }
    }
    for (size_t i = 0; i < sections.size(); ++i) {
        if (!used[i])
            ordered.push_back(sections[i]);
    }

    if (!ordered.empty() && !candidates.empty()) {
        auto& first_section = ordered.front().lines;
        first_section.insert(first_section.end(), candidates.begin(), candidates.end());
    }

    std::vector<std::string> out = session_lines;
    for (const auto& section : ordered)
        out.insert(out.end(), section.lines.begin(), section.lines.end());
    return JoinSdpLines(out);
}

std::string AdaptAnswerSdpToOffer(
    const std::string& answer_sdp,
    const std::string& offer_sdp,
    const opennow::StreamSettings& settings)
{
    const int h264_payload_type = SelectOfferH264PayloadType(offer_sdp);
    const std::vector<std::string> offer_lines = SplitSdpLines(offer_sdp);
    const std::string offer_h264_fmtp = FindFmtpForPayload(offer_lines, h264_payload_type);
    const std::vector<std::string> offer_h264_feedback = FindRtcpFbForPayload(offer_lines, h264_payload_type);
    std::vector<std::string> lines = SplitSdpLines(answer_sdp);
    std::vector<std::string> out;
    bool in_video = false;
    bool video_bitrate_added = false;
    bool video_feedback_added = false;

    const std::string payload = std::to_string(h264_payload_type);

    for (const auto& line : lines) {
        if (StartsWith(line, "m=")) {
            in_video = StartsWith(line, "m=video");
            if (in_video) {
                out.push_back("m=video 9 UDP/TLS/RTP/SAVPF " + payload);
                continue;
            } else if (StartsWith(line, "m=audio")) {
                out.push_back("m=audio 9 UDP/TLS/RTP/SAVPF 111");
                continue;
            } else if (StartsWith(line, "m=application")) {
                out.push_back("m=application 9 UDP/DTLS/SCTP webrtc-datachannel");
                continue;
            }
        }

        if (in_video && StartsWith(line, "c=IN ")) {
            out.push_back(line);
            out.push_back("b=AS:" + std::to_string(settings.bitrate_kbps));
            video_bitrate_added = true;
            continue;
        }

        if (in_video && StartsWith(line, "a=rtcp-fb:96")) {
            if (!video_feedback_added) {
                if (!offer_h264_feedback.empty()) {
                    for (const auto& feedback : offer_h264_feedback)
                        out.push_back(feedback);
                } else {
                    out.push_back("a=rtcp-fb:" + payload + " transport-cc");
                    out.push_back("a=rtcp-fb:" + payload + " ccm fir");
                    out.push_back("a=rtcp-fb:" + payload + " nack");
                    out.push_back("a=rtcp-fb:" + payload + " nack pli");
                }
                video_feedback_added = true;
            }
            continue;
        }

        if (in_video && StartsWith(line, "a=fmtp:96")) {
            if (!offer_h264_fmtp.empty())
                out.push_back(offer_h264_fmtp);
            else
                out.push_back("a=fmtp:" + payload + " level-asymmetry-allowed=1;packetization-mode=1;profile-level-id=42e01f");
            continue;
        }

        if (in_video && StartsWith(line, "a=rtpmap:96")) {
            out.push_back("a=rtpmap:" + payload + " H264/90000");
            continue;
        }

        if (in_video && StartsWith(line, "a=ssrc:"))
            continue;

        if (in_video && StartsWith(line, "a=sendrecv")) {
            out.push_back("a=recvonly");
            continue;
        }

        out.push_back(line);
    }

    if (!video_bitrate_added) {
        for (size_t i = 0; i < out.size(); ++i) {
            if (StartsWith(out[i], "m=video")) {
                out.insert(out.begin() + static_cast<long>(i + 1), "b=AS:" + std::to_string(settings.bitrate_kbps));
                break;
            }
        }
    }

    return AlignAnswerSdpToOffer(JoinSdpLines(out), offer_sdp);
}

std::string ExtractSdpValue(const std::string& sdp, const std::string& prefix)
{
    for (const auto& line : SplitSdpLines(sdp)) {
        if (StartsWithString(line, prefix))
            return line.substr(prefix.size());
    }
    return "";
}

int ExtractNvstIntValue(const std::string& nvst_sdp, const std::string& prefix)
{
    for (const auto& line : SplitSdpLines(nvst_sdp)) {
        if (!StartsWithString(line, prefix))
            continue;

        const std::string value = line.substr(prefix.size());
        char* end = nullptr;
        const long parsed = std::strtol(value.c_str(), &end, 10);
        if (end != value.c_str() && parsed > 0 && parsed <= 65535)
            return static_cast<int>(parsed);
    }
    return 0;
}

std::string BuildNvstSdp(
    const std::string& answer_sdp,
    const opennow::StreamSettings& settings,
    const RiInputCapabilities& ri_caps)
{
    const std::string ice_ufrag = ExtractSdpValue(answer_sdp, "a=ice-ufrag:");
    const std::string ice_pwd = ExtractSdpValue(answer_sdp, "a=ice-pwd:");
    const std::string fingerprint = ExtractSdpValue(answer_sdp, "a=fingerprint:sha-256 ");
    const auto tuning = opennow::video::ResolveQualityTuning(settings.image_quality_mode);
    const int min_bitrate = std::max(
        5000, (settings.bitrate_kbps * tuning.minimum_bitrate_percent) / 100);
    const int initial_bitrate = std::max(
        min_bitrate, (settings.bitrate_kbps * tuning.initial_bitrate_percent) / 100);

    std::vector<std::string> lines = {
        "v=0",
        "o=SdpTest test_id_13 14 IN IPv4 127.0.0.1",
        "s=-",
        "t=0 0",
        "a=general.icePassword:" + ice_pwd,
        "a=general.iceUserNameFragment:" + ice_ufrag,
        "a=general.dtlsFingerprint:" + fingerprint,
        "m=video 0 RTP/AVP",
        "a=msid:fbc-video-0",
        "a=vqos.fec.rateDropWindow:10",
        "a=vqos.fec.minRequiredFecPackets:2",
        "a=vqos.fec.repairMinPercent:" + std::to_string(tuning.fec_repair_min_percent),
        "a=vqos.fec.repairPercent:" + std::to_string(tuning.fec_repair_percent),
        "a=vqos.fec.repairMaxPercent:" + std::to_string(tuning.fec_repair_max_percent),
        "a=vqos.dynamicStreamingMode:0",
        "a=vqos.drc.enable:0",
        "a=vqos.dfc.enable:0",
        "a=vqos.dfc.adjustResAndFps:0",
        "a=video.dx9EnableNv12:1",
        "a=video.dx9EnableHdr:1",
        "a=vqos.qpg.enable:1",
        "a=vqos.resControl.qp.qpg.featureSetting:7",
        "a=bwe.useOwdCongestionControl:1",
        "a=video.enableRtpNack:1",
        "a=vqos.bw.txRxLag.minFeedbackTxDeltaMs:200",
        "a=vqos.drc.bitrateIirFilterFactor:18",
        "a=video.packetSize:1140",
        "a=packetPacing.minNumPacketsPerGroup:" +
            std::to_string(tuning.pacing_min_packets_per_group),
        "a=vqos.adjustStreamingFpsDuringOutOfFocus:1",
        "a=vqos.resControl.cpmRtc.ignoreOutOfFocusWindowState:1",
        "a=vqos.resControl.perfHistory.rtcIgnoreOutOfFocusWindowState:1",
        "a=vqos.resControl.cpmRtc.featureMask:0",
        "a=vqos.resControl.cpmRtc.enable:0",
        "a=vqos.resControl.cpmRtc.minResolutionPercent:100",
        "a=vqos.resControl.cpmRtc.resolutionChangeHoldonMs:999999",
        "a=packetPacing.numGroups:" + std::to_string(tuning.pacing_groups),
        "a=packetPacing.maxDelayUs:" + std::to_string(tuning.pacing_max_delay_us),
        "a=packetPacing.minNumPacketsFrame:10",
        "a=video.rtpNackQueueLength:1024",
        "a=video.rtpNackQueueMaxPackets:512",
        "a=video.rtpNackMaxPacketCount:25",
        "a=vqos.drc.qpMaxResThresholdAdj:4",
        "a=vqos.grc.qpMaxResThresholdAdj:4",
        "a=vqos.drc.iirFilterFactor:100",
        "a=video.clientViewportWd:" + std::to_string(settings.width),
        "a=video.clientViewportHt:" + std::to_string(settings.height),
        "a=video.maxFPS:" + std::to_string(settings.fps),
        "a=video.initialBitrateKbps:" + std::to_string(initial_bitrate),
        "a=video.initialPeakBitrateKbps:" + std::to_string(settings.bitrate_kbps),
        "a=vqos.bw.maximumBitrateKbps:" + std::to_string(settings.bitrate_kbps),
        "a=vqos.bw.minimumBitrateKbps:" + std::to_string(min_bitrate),
        "a=vqos.bw.peakBitrateKbps:" + std::to_string(settings.bitrate_kbps),
        "a=vqos.bw.serverPeakBitrateKbps:" + std::to_string(settings.bitrate_kbps),
        "a=vqos.bw.enableBandwidthEstimation:1",
        "a=vqos.bw.disableBitrateLimit:0",
        "a=vqos.grc.maximumBitrateKbps:" + std::to_string(settings.bitrate_kbps),
        "a=vqos.grc.enable:0",
        "a=video.maxNumReferenceFrames:4",
        "a=video.mapRtpTimestampsToFrames:1",
        "a=video.encoderCscMode:3",
        "a=video.dynamicRangeMode:0",
        "a=video.bitDepth:8",
        "a=video.scalingFeature1:0",
        "a=video.prefilterParams.prefilterModel:0",
        "m=audio 0 RTP/AVP",
        "a=msid:audio",
        "m=mic 0 RTP/AVP",
        "a=msid:mic",
        "a=rtpmap:0 PCMU/8000",
        "m=application 0 RTP/AVP",
        "a=msid:input_1",
        "a=ri.partialReliableThresholdMs:" + std::to_string(ri_caps.partial_reliable_threshold_ms),
        "a=ri.hidDeviceMask:" + std::to_string(ri_caps.hid_device_mask),
        "a=ri.enablePartiallyReliableTransferGamepad:" + std::to_string(ri_caps.partial_reliable_gamepad_mask),
        "a=ri.enablePartiallyReliableTransferHid:" + std::to_string(ri_caps.partial_reliable_hid_mask),
        "",
    };

    std::string result;
    for (const auto& line : lines) {
        result += line;
        result += "\n";
    }
    return result;
}

std::string ExtractSignalingHost(const std::string& url)
{
    size_t start = url.find("://");
    start = start == std::string::npos ? 0 : start + 3;

    size_t end = url.find('/', start);
    std::string host_port = url.substr(start, end == std::string::npos ? std::string::npos : end - start);

    const size_t at = host_port.rfind('@');
    if (at != std::string::npos)
        host_port.erase(0, at + 1);

    if (!host_port.empty() && host_port.front() == '[') {
        const size_t close = host_port.find(']');
        return close == std::string::npos ? host_port : host_port.substr(1, close - 1);
    }

    const size_t colon = host_port.find(':');
    return colon == std::string::npos ? host_port : host_port.substr(0, colon);
}

std::string DottedIpv4FromGfnHost(const std::string& host)
{
    int octets[4] = {-1, -1, -1, -1};
    size_t pos = 0;

    for (int i = 0; i < 4; ++i) {
        if (pos >= host.size() || !std::isdigit(static_cast<unsigned char>(host[pos])))
            return "";

        int value = 0;
        while (pos < host.size() && std::isdigit(static_cast<unsigned char>(host[pos]))) {
            value = value * 10 + (host[pos] - '0');
            ++pos;
        }

        if (value < 0 || value > 255)
            return "";

        octets[i] = value;
        if (i < 3) {
            if (pos >= host.size() || host[pos] != '-')
                return "";
            ++pos;
        }
    }

    return std::to_string(octets[0]) + "." + std::to_string(octets[1]) + "." +
           std::to_string(octets[2]) + "." + std::to_string(octets[3]);
}

std::string NormalizeGfnMediaIp(const std::string& host)
{
    std::string media_ip = DottedIpv4FromGfnHost(host);
    return media_ip.empty() ? host : media_ip;
}

std::string PrepareGfnOfferSdp(
    std::string sdp,
    const std::string& signaling_url,
    const std::string& media_ip_hint,
    int media_port_hint)
{
    std::string media_ip = NormalizeGfnMediaIp(media_ip_hint);

    if (media_ip.empty()) {
        const std::string host = ExtractSignalingHost(signaling_url);
        media_ip = NormalizeGfnMediaIp(host);
    }

    if (!media_ip.empty())
        sdp = ReplaceAll(std::move(sdp), "0.0.0.0", media_ip);

    return sdp;
}

std::string BuildManualMediaCandidate(
    const std::string& signaling_url,
    const std::string& media_ip_hint,
    int media_port_hint,
    int foundation)
{
    std::string media_ip = NormalizeGfnMediaIp(media_ip_hint);
    if (media_ip.empty()) {
        const std::string host = ExtractSignalingHost(signaling_url);
        media_ip = NormalizeGfnMediaIp(host);
    }

    if (media_ip.empty() || media_port_hint <= 0)
        return "";

    return "a=candidate:" + std::to_string(foundation) + " 1 UDP 2130706431 " + media_ip + " " +
        std::to_string(media_port_hint) + " typ host";
}

} // namespace

extern "C" {
    static void on_ice_candidate_cb(char* sdp_text, void* userdata) {
        if (userdata && sdp_text) {
            static_cast<WebRtcSession*>(userdata)->on_ice_candidate(std::string(sdp_text));
        }
    }

    static void on_peer_state_change_cb(PeerConnectionState state, void* userdata) {
        if (userdata) {
            static_cast<WebRtcSession*>(userdata)->on_peer_state_change(state);
        }
    }

    static void on_datachannel_message_cb(char* msg, size_t len, void* userdata, uint16_t sid) {
        if (userdata) {
            static_cast<WebRtcSession*>(userdata)->on_datachannel_message(msg, len, sid);
        }
    }

    static void on_datachannel_open_cb(void* userdata) {
        if (userdata) {
            static_cast<WebRtcSession*>(userdata)->on_datachannel_open();
        }
    }

    static void on_datachannel_close_cb(void* userdata) {
        if (userdata) {
            static_cast<WebRtcSession*>(userdata)->on_datachannel_close();
        }
    }

    static void on_video_packet_cb(const PeerVideoPacket* packet, void* userdata) {
        if (userdata && packet)
            static_cast<WebRtcSession*>(userdata)->on_video_packet(*packet);
    }

    static void on_audio_packet_cb(const PeerAudioPacket* packet, void* userdata) {
        if (userdata && packet)
            static_cast<WebRtcSession*>(userdata)->on_audio_packet(*packet);
    }

    static void on_rtp_sender_report_cb(uint32_t ssrc, uint64_t ntp_us,
                                        uint32_t rtp_timestamp, void* userdata) {
        if (userdata)
            static_cast<WebRtcSession*>(userdata)->on_rtp_sender_report(ssrc, ntp_us, rtp_timestamp);
    }
}

WebRtcSession::WebRtcSession(
    const std::string& signaling_url,
    const std::string& jwt_token,
    const std::string& session_id,
    const std::string& media_ip,
    int media_port,
    const std::vector<opennow::IceServerInfo>& ice_servers)
    : signaling_url_(signaling_url),
      jwt_token_(jwt_token),
      session_id_(session_id),
      media_ip_(media_ip),
      media_port_(media_port),
      ice_servers_(ice_servers),
      settings_(opennow::LoadStreamSettings()),
      peer_name_(MakePeerName()) {
    opennow::SetStreamDiagnosticsEnabled(settings_.debug_diagnostics);
    
    // Initialize libpeer
    peer_init();
    renderer_ = std::make_unique<DKVideoRenderer>();
    audio_ = std::make_unique<AudioPipeline>();
    audio_->configure(settings_.audio_volume, settings_.audio_buffer_ms);

    const bool previous_nvdec_crash = settings_.video_backend == "Auto" &&
                                      ConsumeNvdecCrashMarker();
    auto_safe_mode_used_ = previous_nvdec_crash;
    const bool force_software = settings_.video_backend == "Software" ||
                                previous_nvdec_crash;
    decoder_ = std::make_unique<FFmpegVideoDecoder>();
    decoder_setup_result_ = decoder_->setup(
        VIDEO_FORMAT_H264, settings_.width, settings_.height, settings_.fps, nullptr,
        force_software ? VIDEO_DECODER_FORCE_SOFTWARE : VIDEO_DECODER_PREFER_HARDWARE);

    if (decoder_setup_result_ != 0 && !force_software) {
        decoder_->cleanup();
        decoder_ = std::make_unique<FFmpegVideoDecoder>();
        decoder_setup_result_ = decoder_->setup(
            VIDEO_FORMAT_H264, settings_.width, settings_.height, settings_.fps, nullptr,
            VIDEO_DECODER_FORCE_SOFTWARE);
        decoder_fallback_used_ = decoder_setup_result_ == 0;
    }

    if (decoder_setup_result_ == 0 && decoder_->uses_hardware_frames())
        video_backend_name_ = "NVDEC-NVTEGRA/Deko3D-zero-copy";
    else if (decoder_setup_result_ == 0)
        video_backend_name_ = auto_safe_mode_used_
            ? "FFmpeg-SW-3T/Deko3D-upload(crash-safe-mode)"
            : decoder_fallback_used_
            ? "FFmpeg-SW-3T/Deko3D-upload(auto-fallback)"
            : "FFmpeg-SW-3T/Deko3D-upload";
    else
        video_backend_name_ = "decoder-setup-failed";

    if (decoder_setup_result_ == 0 && decoder_->uses_hardware_frames())
        nvdec_marker_owned_ = CreateNvdecCrashMarker();
}

WebRtcSession::~WebRtcSession() {
    stop();
    peer_deinit();
}

void WebRtcSession::setup_peer_connection() {
    PeerConfiguration config = {};
    config.video_codec = CODEC_H264;
    config.audio_codec = CODEC_OPUS;
    config.datachannel = DATA_CHANNEL_STRING;
    config.onvideopacket = on_video_packet_cb;
    config.onaudiopacket = on_audio_packet_cb;
    config.onrtpsenderreport = on_rtp_sender_report_cb;
    config.user_data = this;

    if (ice_servers_.empty()) {
        ice_servers_.push_back({"stun:s1.stun.gamestream.nvidia.com:19308", "", ""});
        ice_servers_.push_back({"stun:stun.l.google.com:19302", "", ""});
        ice_servers_.push_back({"stun:stun1.l.google.com:19302", "", ""});
    }

    const size_t ice_count = std::min<size_t>(ice_servers_.size(), 5);
    for (size_t i = 0; i < ice_count; ++i) {
        config.ice_servers[i].urls = ice_servers_[i].url.c_str();
        config.ice_servers[i].username =
            ice_servers_[i].username.empty() ? nullptr : ice_servers_[i].username.c_str();
        config.ice_servers[i].credential =
            ice_servers_[i].credential.empty() ? nullptr : ice_servers_[i].credential.c_str();
    }

    pc_ = peer_connection_create(&config);
    if (!pc_) {
        brls::Logger::error("Failed to create PeerConnection");
        return;
    }

    peer_connection_onicecandidate(pc_, on_ice_candidate_cb);
    peer_connection_oniceconnectionstatechange(pc_, on_peer_state_change_cb);
    peer_connection_ondatachannel(pc_, on_datachannel_message_cb, on_datachannel_open_cb, on_datachannel_close_cb);
    // Note: libpeer requires patching or explicit callback for track data
    // Assuming on_track or similar is available. For now, we stub it.
    // peer_connection_ontrack(pc_, on_track_cb, this);
}

void WebRtcSession::start() {
    stop_requested_.store(false, std::memory_order_release);
    session_started_at_ = std::chrono::steady_clock::now();
    ResetStreamTraceLog();
    AppendStreamLog("SESSION start url=" + signaling_url_ +
                    " media=" + (media_ip_.empty() ? std::string("(auto)") : media_ip_) +
                    ":" + std::to_string(media_port_) +
                    " preset=" + settings_.label +
                    " " + std::to_string(settings_.width) + "x" +
                    std::to_string(settings_.height) + "@" +
                    std::to_string(settings_.fps) +
                    " bitrate=" + std::to_string(settings_.bitrate_kbps) +
                    " iceServers=" + std::to_string(ice_servers_.size()));
    AppendStreamLog("VIDEO backend=" + video_backend_name_ +
                    " requested=" + settings_.video_backend +
                    " fallback=" + std::to_string(decoder_fallback_used_ ? 1 : 0) +
                    " crashSafeMode=" + std::to_string(auto_safe_mode_used_ ? 1 : 0) +
                    " decoderSetup=" + std::to_string(decoder_setup_result_) +
                    " expectedFormat=" +
                    std::string(decoder_ && decoder_->uses_hardware_frames()
                        ? "NVTEGRA" : "YUV420P/NV12"));
    const bool input_codec_ok = InputEncodingSelfTest();
    AppendStreamLog("INPUT codec_selftest ok=" + std::to_string(input_codec_ok ? 1 : 0));
    AppendInputLog("SESSION start codecSelfTest=" + std::to_string(input_codec_ok ? 1 : 0) +
                   " expectedController=xbox fixedReliableSid=0");
    AppendTraceLog("SESSION start");
    AppendTraceLog("url=" + signaling_url_);
    AppendTraceLog("sessionId=" + session_id_);
    AppendTraceLog("peerName=" + peer_name_);
    AppendTraceLog("mediaHint=" + (media_ip_.empty() ? std::string("(auto)") : media_ip_) +
                   ":" + std::to_string(media_port_));
    AppendTraceLog("preset=" + settings_.label + " resolution=" +
                   std::to_string(settings_.width) + "x" + std::to_string(settings_.height) +
                   " fps=" + std::to_string(settings_.fps) +
                   " bitrateKbps=" + std::to_string(settings_.bitrate_kbps));
    for (size_t i = 0; i < ice_servers_.size(); ++i) {
        AppendTraceLog("iceServer[" + std::to_string(i) + "] url=" + ice_servers_[i].url +
                       " user=" + (ice_servers_[i].username.empty() ? std::string("(empty)") : std::string("(set)")) +
                       " credential=" + (ice_servers_[i].credential.empty() ? std::string("(empty)") : std::string("(set)")));
    }
    setup_peer_connection();
    if (!pc_) {
        current_state_ = "PeerConnection setup failed";
        AppendStreamLog("SESSION error peer_connection_setup_failed");
        return;
    }
    
    const std::string sign_in_url = BuildSignInUrl(signaling_url_, peer_name_, session_id_);
    signaling_client_ = std::make_unique<SignalingClient>(sign_in_url);
    signaling_client_->set_on_message([this](const std::string& msg) {
        handle_signaling_message(msg);
    });

    std::vector<std::string> headers;
    headers.push_back("Origin: https://play.geforcenow.com");
    headers.push_back("User-Agent: Mozilla/5.0 (Windows NT 10.0; Win64; x64) Chrome/131.0.0.0 Safari/537.36");
    if (!session_id_.empty()) {
        headers.push_back("Sec-WebSocket-Protocol: x-nv-sessionid." + session_id_);
    }
    signaling_client_->set_custom_headers(headers);

    if (!signaling_client_->connect()) {
        current_state_ = "WebSocket Connect Failed: " + signaling_client_->get_last_error();
        AppendStreamLog("SESSION error websocket_connect_failed " + signaling_client_->get_last_error());
        signaling_ready_ = false;
        return;
    }
    signaling_ready_ = true;
    current_state_ = "Signaling connected, waiting for offer";
    AppendStreamLog("SIGNAL connected");
    send_peer_info();
    send_heartbeat();
    if (settings_.audio_enabled)
        audio_->start();
    start_decoder_worker();
    start_network_worker();
}

void WebRtcSession::start_decoder_worker() {
    if (decoder_running_.exchange(true))
        return;

    decoder_thread_ = std::thread(&WebRtcSession::decoder_loop, this);
    AppendStreamLog("DECODE worker_started");
}

void WebRtcSession::decoder_loop() {
    for (;;) {
        DecodeUnit unit;
        {
            std::unique_lock<std::mutex> lock(decoder_queue_mutex_);
            decoder_queue_cv_.wait(lock, [this] {
                return !decoder_running_.load(std::memory_order_acquire) || !decoder_queue_.empty();
            });
            if (!decoder_running_.load(std::memory_order_acquire))
                break;

            unit = std::move(decoder_queue_.front());
            decoder_queue_.pop_front();
        }

        const auto decode_started_at = std::chrono::steady_clock::now();
        const uint64_t queue_wait_us = unit.enqueued_at.time_since_epoch().count() == 0 ? 0 :
            static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(
                decode_started_at - unit.enqueued_at).count());
        RecordLatency(queue_wait_buckets_, queue_wait_us);
        AtomicMax(queue_wait_us_max_, queue_wait_us);

        if (decoder_reset_requested_.exchange(false) && decoder_)
            decoder_->reset_stream();

        const int decoded = decoder_
            ? decoder_->submit_decode_unit(unit.data.data(), static_cast<int>(unit.data.size()), unit.rtp_timestamp)
            : -1;
        const uint64_t decode_us = static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::microseconds>(
                std::chrono::steady_clock::now() - decode_started_at).count());
        decode_us_total_.fetch_add(decode_us, std::memory_order_relaxed);
        RecordLatency(decode_latency_buckets_, decode_us);
        AtomicMax(decode_us_max_, decode_us);

        const int access_unit_count = packets_received_.load();
        if (decoded > 0) {
            last_decoded_frame_at_us_.store(NowUs(), std::memory_order_release);
            const int frame_count = frames_decoded_.fetch_add(decoded) + decoded;
            if (!first_decoded_frame_logged_) {
                first_decoded_frame_logged_ = true;
                AppendStreamLog("DECODE first_frame frames=" + std::to_string(frame_count) +
                                " accessUnits=" + std::to_string(access_unit_count));
            } else if (frame_count - last_logged_frame_count_.load(std::memory_order_relaxed) >= 120) {
                AppendStreamLog("DECODE frame_progress frames=" + std::to_string(frame_count) +
                                " accessUnits=" + std::to_string(access_unit_count));
                last_logged_frame_count_.store(frame_count, std::memory_order_relaxed);
            }
        } else if (decoded < 0) {
            const int error_count = decode_errors_.fetch_add(1) + 1;
            if (error_count <= 5 ||
                error_count - last_logged_decode_error_count_.load(std::memory_order_relaxed) >= 20) {
                AppendStreamLog("DECODE error count=" + std::to_string(error_count) +
                                " accessUnit=" + std::to_string(access_unit_count) +
                                " size=" + std::to_string(unit.data.size()) +
                                " idr=" + std::to_string(unit.idr ? 1 : 0) +
                                " preview=" + HexPreview(unit.data.data(), unit.data.size()));
                last_logged_decode_error_count_.store(error_count, std::memory_order_relaxed);
            }
            decoder_resync_required_.store(true);
            if (!decoder_ || !decoder_->uses_hardware_frames())
                decoder_reset_requested_.store(true);
            keyframe_needed_.store(true);
        }

        if (unit.idr && decoded >= 0) {
            decoder_resync_required_.store(false);
            keyframe_needed_.store(false);
        }

        {
            std::lock_guard<std::mutex> queue_lock(decoder_queue_mutex_);
            recycle_decode_buffer_locked(std::move(unit.data));
        }
    }

    AppendStreamLog("DECODE worker_stopped");
}

void WebRtcSession::recycle_decode_buffer_locked(std::vector<uint8_t>&& buffer) {
    constexpr size_t kPoolLimit = 8;
    constexpr size_t kMaximumReusableCapacity = 2 * 1024 * 1024;
    if (decoder_buffer_pool_.size() >= kPoolLimit ||
        buffer.capacity() > kMaximumReusableCapacity) {
        return;
    }
    buffer.clear();
    decoder_buffer_pool_.push_back(std::move(buffer));
}

void WebRtcSession::clear_decoder_queue_locked() {
    while (!decoder_queue_.empty()) {
        recycle_decode_buffer_locked(std::move(decoder_queue_.front().data));
        decoder_queue_.pop_front();
    }
}

void WebRtcSession::enqueue_decode_unit(const uint8_t* data, size_t size, uint32_t rtp_timestamp) {
    if (!data || size == 0 || !decoder_running_.load(std::memory_order_acquire))
        return;

    const bool idr = ContainsH264Idr(data, size);
    std::lock_guard<std::mutex> lock(decoder_queue_mutex_);

    if (decoder_resync_required_.load() && !idr) {
        decoder_queue_drops_++;
        return;
    }

    // Absorb short dynamic-scene bursts without immediately discarding the
    // reference chain. Eight 60 fps pictures cap reserve latency near 133 ms.
    constexpr size_t kMaxQueuedAccessUnits = 8;
    if (decoder_queue_.size() >= kMaxQueuedAccessUnits) {
        decoder_queue_drops_ += static_cast<int>(decoder_queue_.size());
        clear_decoder_queue_locked();
        decoder_resync_required_.store(true);
        if (!decoder_ || !decoder_->uses_hardware_frames())
            decoder_reset_requested_.store(true);
        keyframe_needed_.store(true);
        if (!idr) {
            decoder_queue_drops_++;
            return;
        }
    }

    std::vector<uint8_t> buffer;
    if (!decoder_buffer_pool_.empty()) {
        buffer = std::move(decoder_buffer_pool_.front());
        decoder_buffer_pool_.pop_front();
        decoder_buffer_reuses_.fetch_add(1, std::memory_order_relaxed);
    } else {
        decoder_buffer_allocations_.fetch_add(1, std::memory_order_relaxed);
    }
    buffer.resize(size);
    std::memcpy(buffer.data(), data, size);
    decoder_queue_.push_back({
        std::move(buffer), idr, rtp_timestamp,
        std::chrono::steady_clock::now()
    });
    AtomicMax(decoder_queue_high_water_, decoder_queue_.size());
    decoder_queue_cv_.notify_one();
}

void WebRtcSession::start_network_worker() {
    if (network_running_.exchange(true))
        return;

    network_thread_ = std::thread(&WebRtcSession::network_loop, this);
    AppendStreamLog("TRANSPORT worker_started");
}

void WebRtcSession::network_loop() {
    while (network_running_.load(std::memory_order_acquire)) {
        bool can_run = false;
        {
            std::lock_guard<std::recursive_mutex> lock(peer_mutex_);
            can_run = pc_ && signaling_ready_ && remote_description_set_ &&
                      (remote_ice_count_ > 0 || manual_candidate_added_);
            if (can_run) {
                // Motion-heavy frames arrive as short UDP bursts. Drain a
                // bounded batch while the peer lock is already held instead
                // of paying one mutex hand-off and scheduler yield per packet.
                constexpr int kMaximumDatagramsPerBatch = 24;
                constexpr auto kMaximumBatchTime = std::chrono::microseconds(1000);
                const auto batch_started_at = std::chrono::steady_clock::now();
                int batch_size = 0;
                for (int packet = 0; packet < kMaximumDatagramsPerBatch; ++packet) {
                    if (peer_connection_loop(pc_) == 0)
                        break;
                    batch_size++;
                    if (batch_size >= 8 &&
                        std::chrono::steady_clock::now() - batch_started_at >=
                            kMaximumBatchTime) {
                        break;
                    }
                }
                if (batch_size > 0) {
                    transport_batches_.fetch_add(1, std::memory_order_relaxed);
                    transport_datagrams_.fetch_add(
                        static_cast<uint64_t>(batch_size), std::memory_order_relaxed);
                    AtomicMax(transport_batch_high_water_, static_cast<size_t>(batch_size));
                }
            }
        }

        if (!can_run)
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        else
            std::this_thread::yield();
    }

    AppendStreamLog("TRANSPORT worker_stopped");
}

void WebRtcSession::draw(NVGcontext* vg, int width, int height, AVFrame* frame, uint64_t generation) {
    if (renderer_ && frame) {
        rendered_video_width_.store(frame->width, std::memory_order_relaxed);
        rendered_video_height_.store(frame->height, std::memory_order_relaxed);
        const auto render_started_at = std::chrono::steady_clock::now();
        last_rendered_video_pts_.store(frame->pts);
        renderer_->drawLatest(vg, width, height, frame, VIDEO_FORMAT_H264, generation);
        const uint64_t render_us = static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::microseconds>(
                std::chrono::steady_clock::now() - render_started_at).count());
        render_us_total_.fetch_add(render_us, std::memory_order_relaxed);
        RecordLatency(render_latency_buckets_, render_us);
        AtomicMax(render_us_max_, render_us);
        const uint64_t previous_generation = last_presented_generation_.exchange(generation);
        if (generation != 0 && generation != previous_generation)
            presented_frames_.fetch_add(1, std::memory_order_relaxed);
    }
}

void WebRtcSession::poll() {
    if (stop_requested_.load(std::memory_order_acquire))
        return;

    std::lock_guard<std::recursive_mutex> lock(peer_mutex_);

    if (signaling_client_) {
        signaling_client_->poll();
    }

    maybe_send_keepalive();

    if (pc_ && signaling_ready_) {
        if (!remote_description_set_) {
            current_state_ = "Signaling connected, waiting for offer";
            return;
        }

        maybe_add_manual_media_candidate();

        if (remote_ice_count_ == 0 && !manual_candidate_added_) {
            current_state_ = "Waiting for remote ICE";
            return;
        }

        const PeerConnectionState state = peer_connection_get_state(pc_);
        current_state_ = std::string("Peer ") + peer_connection_state_to_string(state);
        if (state == PEER_CONNECTION_COMPLETED) {
            if (keyframe_needed_.exchange(false) && decoder_resync_required_.load())
                request_keyframe("decoder_resync");
            maybe_open_datachannel();
            maybe_activate_input();
            maybe_send_startup_control_messages();
            maybe_send_input_heartbeat();
            maybe_recover_rtp_damage();
            maybe_recover_decode_stall();
        }
        if (state == PEER_CONNECTION_COMPLETED && !initial_keyframe_requested_) {
            last_startup_keyframe_request_ = std::chrono::steady_clock::now();
            request_keyframe("stream_start");
            initial_keyframe_requested_ = true;
        }
        maybe_request_startup_keyframe_retry();
        maybe_log_stream_diagnostics();
    }
}

bool WebRtcSession::send_gamepad_input(
    uint16_t buttons,
    uint8_t left_trigger,
    uint8_t right_trigger,
    float lx,
    float ly,
    float rx,
    float ry) {
    std::lock_guard<std::recursive_mutex> lock(peer_mutex_);
    gamepad_input_attempt_count_++;
    const PeerConnectionState peer_state = pc_ ? peer_connection_get_state(pc_) : PEER_CONNECTION_CLOSED;
    if (!pc_ || peer_state != PEER_CONNECTION_COMPLETED || !input_ready_) {
        gamepad_input_blocked_count_++;
        if (gamepad_input_blocked_count_ <= 8 || gamepad_input_blocked_count_ % 50 == 0) {
            AppendInputLog("PAD blocked attempt=" + std::to_string(gamepad_input_attempt_count_) +
                           " pc=" + std::to_string(pc_ ? 1 : 0) +
                           " peer=" + (pc_ ? std::string(peer_connection_state_to_string(peer_state)) : "none") +
                           " sctpOpen=" + std::to_string(datachannel_opened_ ? 1 : 0) +
                           " channelRequested=" + std::to_string(datachannel_open_requested_ ? 1 : 0) +
                           " inputReady=" + std::to_string(input_ready_ ? 1 : 0) +
                           " buttons=0x" + HexPreview(reinterpret_cast<const uint8_t*>(&buttons), sizeof(buttons), 2));
        }
        return false;
    }

    const uint64_t timestamp_us = NowUs();
    const std::vector<uint8_t> payload = BuildGamepadPayload(
        timestamp_us, buttons, left_trigger, right_trigger,
        AxisToI16(lx), AxisToI16(-ly), AxisToI16(rx), AxisToI16(-ry));

    const bool use_partial = partial_input_channel_requested_ && input_protocol_version_ >= 3;
    std::vector<uint8_t> wire_payload = use_partial
        ? WrapPartiallyReliableGamepad(input_protocol_version_, timestamp_us, 0, gamepad_sequence_++, payload)
        : WrapReliableGamepad(input_protocol_version_, timestamp_us, payload);
    const uint16_t sid = use_partial ? 2 : 0;
    const int sent = send_datachannel_binary(sid, "gamepad", wire_payload.data(), wire_payload.size());
    if (sent < 0)
        gamepad_send_failure_count_++;
    else
        gamepad_tx_count_++;
    if (gamepad_input_attempt_count_ <= 12 || gamepad_tx_count_ <= 8 ||
        gamepad_input_attempt_count_ % 100 == 0 || sent < 0) {
        AppendInputLog("PAD tx attempt=" + std::to_string(gamepad_input_attempt_count_) +
                       " report=" + std::to_string(gamepad_tx_count_) +
                       " sid=" + std::to_string(sid) +
                       " protocol=" + std::to_string(input_protocol_version_) +
                       " buttons=" + std::to_string(buttons) +
                       " lt=" + std::to_string(left_trigger) +
                       " rt=" + std::to_string(right_trigger) +
                       " axes=" + std::to_string(AxisToI16(lx)) + "/" +
                       std::to_string(AxisToI16(-ly)) + "/" +
                       std::to_string(AxisToI16(rx)) + "/" +
                       std::to_string(AxisToI16(-ry)) +
                       " wireBytes=" + std::to_string(wire_payload.size()) +
                       " sent=" + std::to_string(sent) +
                       " hex=" + HexPreview(wire_payload.data(), wire_payload.size(), 32));
    }
    return sent >= 0;
}

void WebRtcSession::send_mouse_left_button(bool pressed) {
    std::lock_guard<std::recursive_mutex> lock(peer_mutex_);
    if (!pc_ || !input_ready_ || peer_connection_get_state(pc_) != PEER_CONNECTION_COMPLETED) {
        AppendInputLog("MOUSE blocked pressed=" + std::to_string(pressed ? 1 : 0) +
                       " pc=" + std::to_string(pc_ ? 1 : 0) +
                       " inputReady=" + std::to_string(input_ready_ ? 1 : 0));
        return;
    }

    const uint64_t timestamp_us = NowUs();
    const std::vector<uint8_t> payload = BuildMouseButtonPayload(timestamp_us, pressed);
    std::vector<uint8_t> wire_payload = WrapSingleInput(input_protocol_version_, timestamp_us, payload);
    const int sent = send_datachannel_binary(0, pressed ? "mouse_left_down" : "mouse_left_up",
                                             wire_payload.data(), wire_payload.size());
    if (sent >= 0)
        mouse_tx_count_++;
    AppendInputLog("MOUSE tx pressed=" + std::to_string(pressed ? 1 : 0) +
                   " sent=" + std::to_string(sent) +
                   " bytes=" + std::to_string(wire_payload.size()) +
                   " hex=" + HexPreview(wire_payload.data(), wire_payload.size(), 32));
}

void WebRtcSession::send_mouse_move(int16_t dx, int16_t dy) {
    constexpr int16_t kSafeMouseDelta = 4096;
    const int16_t safe_dx = std::clamp(dx, static_cast<int16_t>(-kSafeMouseDelta), kSafeMouseDelta);
    const int16_t safe_dy = std::clamp(dy, static_cast<int16_t>(-kSafeMouseDelta), kSafeMouseDelta);
    if (safe_dx != dx || safe_dy != dy) {
        AppendInputLog("MOUSE_MOVE clamped requested=" + std::to_string(dx) + "/" +
                       std::to_string(dy) + " safe=" + std::to_string(safe_dx) + "/" +
                       std::to_string(safe_dy));
    }
    dx = safe_dx;
    dy = safe_dy;
    if (dx == 0 && dy == 0)
        return;

    std::lock_guard<std::recursive_mutex> lock(peer_mutex_);
    if (!pc_ || !input_ready_ || peer_connection_get_state(pc_) != PEER_CONNECTION_COMPLETED) {
        AppendInputLog("MOUSE_MOVE blocked dx=" + std::to_string(dx) +
                       " dy=" + std::to_string(dy));
        return;
    }

    const uint64_t timestamp_us = NowUs();
    const auto payload = BuildMouseMovePayload(timestamp_us, dx, dy);
    const auto wire_payload = input_protocol_version_ <= 2
        ? payload
        : WrapReliableGamepad(input_protocol_version_, timestamp_us, payload);
    const int sent = send_datachannel_binary(0, "mouse_move", wire_payload.data(), wire_payload.size());
    AppendInputLog("MOUSE_MOVE tx dx=" + std::to_string(dx) +
                   " dy=" + std::to_string(dy) + " sent=" + std::to_string(sent));
}

void WebRtcSession::send_keyboard_key(uint16_t keycode, uint16_t scancode,
                                      uint16_t modifiers, bool pressed) {
    std::lock_guard<std::recursive_mutex> lock(peer_mutex_);
    if (!pc_ || !input_ready_ || peer_connection_get_state(pc_) != PEER_CONNECTION_COMPLETED) {
        if (!opennow::SensitiveInputLoggingSuppressed())
            AppendInputLog("KEY blocked vk=" + std::to_string(keycode) +
                           " pressed=" + std::to_string(pressed ? 1 : 0));
        return;
    }

    const uint64_t timestamp_us = NowUs();
    const auto payload = BuildKeyboardPayload(timestamp_us, keycode, scancode, modifiers, pressed);
    const auto wire_payload = WrapSingleInput(input_protocol_version_, timestamp_us, payload);
    const int sent = send_datachannel_binary(0, pressed ? "key_down" : "key_up",
                                             wire_payload.data(), wire_payload.size());
    if (!opennow::SensitiveInputLoggingSuppressed())
        AppendInputLog("KEY tx vk=" + std::to_string(keycode) +
                       " scan=" + std::to_string(scancode) +
                       " mods=" + std::to_string(modifiers) +
                       " pressed=" + std::to_string(pressed ? 1 : 0) +
                       " sent=" + std::to_string(sent));
}

void WebRtcSession::request_stop() {
    if (stop_requested_.exchange(true, std::memory_order_acq_rel))
        return;

    network_running_.store(false, std::memory_order_release);
    decoder_running_.store(false, std::memory_order_release);
    decoder_queue_cv_.notify_all();
    AppendInputLog("SESSION asynchronous_stop_requested");
}

void WebRtcSession::stop() {
    request_stop();
    if (network_thread_.joinable())
        network_thread_.join();

    if (audio_)
        audio_->stop();

    if (decoder_thread_.joinable())
        decoder_thread_.join();

    {
        std::lock_guard<std::mutex> queue_lock(decoder_queue_mutex_);
        clear_decoder_queue_locked();
        decoder_buffer_pool_.clear();
    }

    std::lock_guard<std::recursive_mutex> lock(peer_mutex_);
    if (!pc_ && !decoder_)
        return;

    log_stream_summary("stop");
    AppendInputLog("SUMMARY attempts=" + std::to_string(gamepad_input_attempt_count_) +
                   " blocked=" + std::to_string(gamepad_input_blocked_count_) +
                   " reportsSent=" + std::to_string(gamepad_tx_count_) +
                   " sendFailures=" + std::to_string(gamepad_send_failure_count_) +
                   " mouseSent=" + std::to_string(mouse_tx_count_) +
                   " sctpOpen=" + std::to_string(datachannel_opened_ ? 1 : 0) +
                   " channelRequested=" + std::to_string(datachannel_open_requested_ ? 1 : 0) +
                   " inputReady=" + std::to_string(input_ready_ ? 1 : 0) +
                   " protocol=" + std::to_string(input_protocol_version_));
    if (pc_) {
        peer_connection_close(pc_);
        peer_connection_destroy(pc_);
        pc_ = nullptr;
    }
    // Zero-copy Deko3D mappings reference NVDEC-owned memory, so release all
    // renderer mappings before destroying the decoder hardware frame pool.
    if (renderer_)
        renderer_.reset();
    if (decoder_) {
        decoder_->cleanup();
        decoder_.reset();
    }
    if (nvdec_marker_owned_) {
        std::remove(kNvdecActiveMarker);
        nvdec_marker_owned_ = false;
    }
}

void WebRtcSession::handle_signaling_message(const std::string& msg) {
    signaling_rx_count_++;
    AppendStreamLog("RX " + msg);
    AppendTraceLog(CompactSignalingMessage(msg));

    if (last_messages_.size() >= 3) {
        last_messages_.erase(last_messages_.begin());
    }
    last_messages_.push_back(CompactSignalingMessage(msg));

    json_error_t error;
    json_t* root = json_loads(msg.c_str(), 0, &error);
    if (!root) return;

    json_t* peer_info = json_object_get(root, "peer_info");
    if (peer_info && json_is_object(peer_info)) {
        const char* name = json_string_value(json_object_get(peer_info, "name"));
        json_t* id = json_object_get(peer_info, "id");
        if (name && peer_name_ == name && json_is_integer(id)) {
            peer_id_ = static_cast<int>(json_integer_value(id));
            current_state_ = "Signaling peer id assigned";
        }
    }

    json_t* ackid = json_object_get(root, "ackid");
    if (ackid && json_is_integer(ackid)) {
        bool should_ack = true;
        if (peer_info && json_is_object(peer_info)) {
            json_t* id = json_object_get(peer_info, "id");
            should_ack = !json_is_integer(id) || static_cast<int>(json_integer_value(id)) != peer_id_;
        }
        if (should_ack) {
            json_t* ack = json_object();
            json_object_set_new(ack, "ack", json_integer(json_integer_value(ackid)));
            char* dump = json_dumps(ack, 0);
            if (dump) {
                signaling_client_->send_message(std::string(dump));
                free(dump);
            }
            json_decref(ack);
        }
    }

    if (json_object_get(root, "hb")) {
        heartbeat_rx_count_++;
        json_t* heartbeat = json_object();
        json_object_set_new(heartbeat, "hb", json_integer(1));
        char* dump = json_dumps(heartbeat, 0);
        if (dump) {
            heartbeat_tx_count_++;
            AppendStreamLog("TX " + std::string(dump));
            signaling_client_->send_message(std::string(dump));
            free(dump);
        }
        json_decref(heartbeat);
        json_decref(root);
        return;
    }

    json_t* peer_msg = json_object_get(root, "peer_msg");
    if (peer_msg && json_is_object(peer_msg)) {
        json_t* from = json_object_get(peer_msg, "from");
        if (json_is_integer(from))
            remote_peer_id_ = static_cast<int>(json_integer_value(from));

        json_t* raw_payload = json_object_get(peer_msg, "msg");
        if (raw_payload && json_is_string(raw_payload)) {
            json_error_t payload_error;
            json_t* payload = json_loads(json_string_value(raw_payload), 0, &payload_error);
            if (payload) {
                json_t* type = json_object_get(payload, "type");
                if (type && json_is_string(type) && std::string(json_string_value(type)) == "offer") {
                    json_t* sdp = json_object_get(payload, "sdp");
                    if (sdp && json_is_string(sdp) && pc_) {
                        offer_received_ = true;
                        offer_count_++;
                        current_state_ = "Got offer, sending answer";
                        const std::string raw_offer_sdp = json_string_value(sdp);
                        AppendTraceBlock("RAW OFFER SDP", raw_offer_sdp);
                        const std::string offer_sdp =
                            PrepareGfnOfferSdp(raw_offer_sdp, signaling_url_, media_ip_, media_port_);
                        AppendTraceBlock("PREPARED OFFER SDP", offer_sdp);
                        json_t* nvst_offer = json_object_get(payload, "nvstSdp");
                        const std::string nvst_offer_sdp =
                            nvst_offer && json_is_string(nvst_offer) ? json_string_value(nvst_offer) : "";
                        if (!nvst_offer_sdp.empty())
                            AppendTraceBlock("OFFER NVST SDP", nvst_offer_sdp);
                        server_ice_ufrag_ = ExtractSdpValue(offer_sdp, "a=ice-ufrag:");
                        const RiInputCapabilities ri_caps = ParseRiInputCapabilities(offer_sdp);
                        partial_reliable_threshold_ms_ = ri_caps.partial_reliable_threshold_ms;
                        const int offer_video_port = ExtractOfferMediaPort(offer_sdp, "video");
                        const int selected_h264_pt = SelectOfferH264PayloadType(offer_sdp);
                        AppendStreamLog("SDP offer received count=" + std::to_string(offer_count_) +
                                        " iceUfrag=" + server_ice_ufrag_ +
                                        " videoPort=" + std::to_string(offer_video_port) +
                                        " h264Pt=" + std::to_string(selected_h264_pt) +
                                        " remoteCandidates=" + std::to_string(CountSdpLinesWithPrefix(offer_sdp, "a=candidate:")) +
                                        " riThreshold=" + std::to_string(ri_caps.partial_reliable_threshold_ms) +
                                        " riGamepadMask=" + std::to_string(ri_caps.partial_reliable_gamepad_mask) +
                                        " riHidMask=" + std::to_string(ri_caps.hid_device_mask));
                        AppendInputLog("CAPS offer partialThresholdMs=" +
                                       std::to_string(ri_caps.partial_reliable_threshold_ms) +
                                       " gamepadMask=" + std::to_string(ri_caps.partial_reliable_gamepad_mask) +
                                       " hidMask=" + std::to_string(ri_caps.hid_device_mask) +
                                       " applicationMLine=" +
                                       std::to_string(offer_sdp.find("m=application") != std::string::npos ? 1 : 0));
                        peer_connection_set_remote_description(pc_, offer_sdp.c_str(), SDP_TYPE_OFFER);
                        remote_description_set_ = true;
                        const char* answer_sdp = peer_connection_create_answer(pc_);
                        if (answer_sdp) {
                            AppendTraceBlock("LIBPEER RAW ANSWER SDP", answer_sdp);
                            const std::string adapted_answer_sdp = AdaptAnswerSdpToOffer(answer_sdp, offer_sdp, settings_);
                            const std::string nvst_sdp = BuildNvstSdp(adapted_answer_sdp, settings_, ri_caps);
                            AppendTraceBlock("ADAPTED ANSWER SDP", adapted_answer_sdp);
                            AppendTraceBlock("ANSWER NVST SDP", nvst_sdp);
                            AppendStreamLog("SDP answer created localUfrag=" +
                                            ExtractSdpValue(adapted_answer_sdp, "a=ice-ufrag:") +
                                            " localCandidates=" +
                                            std::to_string(CountSdpLinesWithPrefix(adapted_answer_sdp, "a=candidate:")) +
                                            " h264Pt=" + std::to_string(selected_h264_pt));
                            json_t* answer = json_object();
                            json_object_set_new(answer, "type", json_string("answer"));
                            json_object_set_new(answer, "sdp", json_string(adapted_answer_sdp.c_str()));
                            json_object_set_new(answer, "nvstSdp", json_string(nvst_sdp.c_str()));
                            send_peer_payload(answer);
                            answer_sent_ = true;
                            json_decref(answer);
                            flush_pending_local_candidates();
                            schedule_manual_media_candidates(offer_sdp, nvst_offer_sdp);
                        }
                    }
                } else {
                    json_t* candidate = json_object_get(payload, "candidate");
                    if (candidate && json_is_string(candidate) && pc_) {
                        std::string candidate_text = json_string_value(candidate);
                        if (candidate_text.rfind("a=candidate:", 0) != 0)
                            candidate_text = "a=" + candidate_text;
                        AppendStreamLog("REMOTE trickle-candidate " + candidate_text);
                        if (peer_connection_add_ice_candidate(pc_, candidate_text.data()) == 0) {
                            remote_trickle_received_ = true;
                            remote_ice_count_++;
                            current_state_ = "Got remote ICE";
                        } else {
                            current_state_ = "Remote ICE rejected";
                            AppendStreamLog("REMOTE trickle-candidate rejected " + candidate_text);
                        }
                    }
                }
                json_decref(payload);
            }
        }
    }
    json_decref(root);
}

void WebRtcSession::on_ice_candidate(const std::string& sdp) {
    if (!signaling_client_)
        return;

    if (sdp.find("v=0") != std::string::npos || sdp.find("m=") != std::string::npos) {
        queue_local_candidates_from_sdp(sdp);
        if (answer_sent_)
            flush_pending_local_candidates();
        return;
    }

    send_local_candidate(sdp);
}

void WebRtcSession::on_peer_state_change(PeerConnectionState state) {
    current_state_ = std::string("Peer ") + peer_connection_state_to_string(state);
    AppendStreamLog("PEER state " + std::string(peer_connection_state_to_string(state)));
    AppendInputLog("TRANSPORT peerState=" + std::string(peer_connection_state_to_string(state)));
    if (state == PEER_CONNECTION_COMPLETED && !peer_completed_seen_) {
        peer_terminal_.store(false, std::memory_order_release);
        peer_ever_completed_.store(true, std::memory_order_release);
        peer_terminal_kind_.store(
            static_cast<int>(opennow::PeerTerminalKind::None),
            std::memory_order_release);
        peer_completed_seen_ = true;
        peer_completed_at_ = std::chrono::steady_clock::now();
        AppendStreamLog("STREAM transport_completed waiting_for_first_video_packet");
    }
    if (state == PEER_CONNECTION_FAILED || state == PEER_CONNECTION_CLOSED || state == PEER_CONNECTION_DISCONNECTED) {
        peer_terminal_.store(true, std::memory_order_release);
        opennow::PeerTerminalKind terminal_kind = opennow::PeerTerminalKind::Closed;
        if (state == PEER_CONNECTION_FAILED)
            terminal_kind = opennow::PeerTerminalKind::Failed;
        else if (state == PEER_CONNECTION_DISCONNECTED)
            terminal_kind = opennow::PeerTerminalKind::Disconnected;
        peer_terminal_kind_.store(static_cast<int>(terminal_kind), std::memory_order_release);
        network_running_.store(false, std::memory_order_release);
        log_stream_summary("peer_state_terminal");
    }
}

void WebRtcSession::record_ui_event(const std::string& event) {
    AppendStreamLog("UI " + event);
}

void WebRtcSession::on_datachannel_message(const char* msg, size_t len, uint16_t sid) {
    std::string preview;
    if (msg && len > 0)
        preview.assign(msg, msg + std::min<size_t>(len, 180));

    const char* label_raw = pc_ ? peer_connection_lookup_sid_label(pc_, sid) : nullptr;
    const std::string label = label_raw ? label_raw : "";
    AppendStreamLog("DATA rx sid=" + std::to_string(sid) +
                    " label=" + (label.empty() ? std::string("(unknown)") : label) +
                    " bytes=" + std::to_string(len) +
                    " hex=" + HexPreview(reinterpret_cast<const uint8_t*>(msg), len, 16) +
                    " preview=" + PreviewText(preview, 120));
    AppendInputLog("RX sid=" + std::to_string(sid) +
                   " label=" + (label.empty() ? std::string("unknown") : label) +
                   " bytes=" + std::to_string(len) +
                   " hex=" + HexPreview(reinterpret_cast<const uint8_t*>(msg), len, 24));

    if (msg && len >= 2 && (label.empty() || label == "input_channel_v1" || sid == 0)) {
        const auto* bytes = reinterpret_cast<const uint8_t*>(msg);
        const uint16_t first_word = static_cast<uint16_t>(bytes[0] | (bytes[1] << 8));
        int version = 2;
        bool handshake = false;

        if (first_word == 526) {
            version = len >= 4 ? static_cast<int>(bytes[2] | (bytes[3] << 8)) : 2;
            handshake = true;
        } else if (bytes[0] == 0x0e) {
            version = first_word;
            handshake = true;
        }

        if (handshake) {
            input_ready_ = true;
            input_protocol_version_ = std::max(2, version);
            AppendStreamLog("DATA input_handshake_complete version=" +
                            std::to_string(input_protocol_version_) +
                            " firstWord=" + std::to_string(first_word));
            AppendInputLog("HANDSHAKE complete protocol=" + std::to_string(input_protocol_version_) +
                           " firstWord=" + std::to_string(first_word));
            maybe_send_input_heartbeat();
        }
    }
}

void WebRtcSession::on_datachannel_open() {
    std::lock_guard<std::recursive_mutex> lock(peer_mutex_);
    datachannel_opened_ = true;
    consecutive_input_send_failures_ = 0;
    AppendStreamLog("DATA sctp_open attempts=" + std::to_string(datachannel_open_attempts_) +
                    " requested=" + std::to_string(datachannel_open_requested_ ? 1 : 0));
    AppendInputLog("SCTP associationOpen=1 attempts=" + std::to_string(datachannel_open_attempts_));
}

void WebRtcSession::on_datachannel_close() {
    std::lock_guard<std::recursive_mutex> lock(peer_mutex_);
    AppendStreamLog("DATA sctp_close");
    AppendInputLog("SCTP associationOpen=0 reason=callback_close");
    reset_input_channel_for_retry("sctp_close");
}

int WebRtcSession::next_ack_id() {
    ack_counter_ += 1;
    return ack_counter_;
}

void WebRtcSession::send_peer_info() {
    if (!signaling_client_)
        return;

    json_t* root = json_object();
    json_object_set_new(root, "ackid", json_integer(next_ack_id()));

    json_t* info = json_object();
    json_object_set_new(info, "browser", json_string("Chrome"));
    json_object_set_new(info, "browserVersion", json_string("131"));
    json_object_set_new(info, "connected", json_true());
    json_object_set_new(info, "id", json_integer(peer_id_));
    json_object_set_new(info, "name", json_string(peer_name_.c_str()));
    json_object_set_new(info, "peerRole", json_integer(0));
    const std::string resolution = std::to_string(settings_.width) + "x" + std::to_string(settings_.height);
    json_object_set_new(info, "resolution", json_string(resolution.c_str()));
    json_object_set_new(info, "version", json_integer(2));
    json_object_set_new(root, "peer_info", info);

    char* dump = json_dumps(root, 0);
    if (dump) {
        AppendStreamLog("TX " + std::string(dump));
        signaling_client_->send_message(std::string(dump));
        free(dump);
    }
    last_peer_info_sent_ = std::chrono::steady_clock::now();
    json_decref(root);
}

void WebRtcSession::send_peer_payload(json_t* payload) {
    if (!signaling_client_ || !payload)
        return;

    char* payload_dump = json_dumps(payload, 0);
    if (!payload_dump)
        return;

    json_t* root = json_object();
    json_t* peer_msg = json_object();
    json_object_set_new(peer_msg, "from", json_integer(peer_id_));
    json_object_set_new(peer_msg, "to", json_integer(remote_peer_id_));
    json_object_set_new(peer_msg, "msg", json_string(payload_dump));
    json_object_set_new(root, "peer_msg", peer_msg);
    json_object_set_new(root, "ackid", json_integer(next_ack_id()));

    char* dump = json_dumps(root, 0);
    if (dump) {
        AppendStreamLog("TX " + std::string(dump));
        AppendTraceLog("TX peer_payload " + PreviewText(std::string(payload_dump), 500));
        signaling_client_->send_message(std::string(dump));
        free(dump);
    }

    free(payload_dump);
    json_decref(root);
}

void WebRtcSession::send_heartbeat() {
    if (!signaling_client_)
        return;

    json_t* heartbeat = json_object();
    json_object_set_new(heartbeat, "hb", json_integer(1));
    char* dump = json_dumps(heartbeat, 0);
    if (dump) {
        heartbeat_tx_count_++;
        AppendStreamLog("TX " + std::string(dump));
        signaling_client_->send_message(std::string(dump));
        free(dump);
    }
    json_decref(heartbeat);
    last_heartbeat_sent_ = std::chrono::steady_clock::now();
}

void WebRtcSession::maybe_send_keepalive() {
    if (!signaling_ready_ || !signaling_client_)
        return;

    const auto now = std::chrono::steady_clock::now();
    if (last_heartbeat_sent_.time_since_epoch().count() == 0 ||
        now - last_heartbeat_sent_ >= std::chrono::seconds(5)) {
        send_heartbeat();
    }

    if (!offer_received_ &&
        (last_peer_info_sent_.time_since_epoch().count() == 0 ||
         now - last_peer_info_sent_ >= std::chrono::seconds(2))) {
        send_peer_info();
    }
}

void WebRtcSession::queue_local_candidates_from_sdp(const std::string& sdp) {
    for (const auto& line : SplitSdpLines(sdp)) {
        if (!StartsWithString(line, "a=candidate:"))
            continue;

        bool seen = false;
        for (const auto& candidate : pending_local_candidates_) {
            if (candidate == line) {
                seen = true;
                break;
            }
        }
        if (!seen) {
            pending_local_candidates_.push_back(line);
            AppendStreamLog("LOCAL queued-candidate " + line);
        }
    }
}

void WebRtcSession::send_local_candidate(const std::string& candidate_sdp) {
    if (!signaling_client_ || candidate_sdp.empty())
        return;

    std::string candidate = candidate_sdp;
    if (candidate.rfind("a=candidate:", 0) == 0)
        candidate.erase(0, 2);
    while (!candidate.empty() && (candidate.back() == '\r' || candidate.back() == '\n'))
        candidate.pop_back();

    json_t* req = json_object();
    json_object_set_new(req, "candidate", json_string(candidate.c_str()));
    json_object_set_new(req, "sdpMid", json_string("0"));
    json_object_set_new(req, "sdpMLineIndex", json_integer(0));
    AppendStreamLog("LOCAL trickle-candidate " + candidate);
    AppendTraceLog("LOCAL trickle-candidate mid=0 " + candidate);
    send_peer_payload(req);
    json_decref(req);
    local_ice_count_++;
}

void WebRtcSession::flush_pending_local_candidates() {
    if (!answer_sent_ || pending_local_candidates_.empty())
        return;

    std::vector<std::string> candidates;
    candidates.swap(pending_local_candidates_);
    for (const auto& candidate : candidates)
        send_local_candidate(candidate);
}

void WebRtcSession::schedule_manual_media_candidates(const std::string& offer_sdp, const std::string& nvst_sdp) {
    if (manual_candidate_added_)
        return;

    // CloudMatch can assign a per-session NVST port that differs from the
    // bundle ports advertised in the SDP. Prefer that endpoint: some bundle
    // ports answer ICE but never accept the DTLS media handshake.
    const std::vector<int> ports = opennow::stream::PrioritizeRemoteCandidatePorts(
        media_port_,
        {ExtractOfferMediaPort(offer_sdp, "video"),
         ExtractOfferMediaPort(offer_sdp, "audio"),
         ExtractOfferMediaPort(offer_sdp, "application"),
         ExtractNvstIntValue(nvst_sdp, "a=general.serverBundlePort:")});

    pending_manual_candidates_.clear();
    int foundation = 1;
    for (int port : ports) {
        std::string candidate = BuildManualMediaCandidate(signaling_url_, media_ip_, port, foundation++);
        if (!candidate.empty())
            pending_manual_candidates_.push_back(candidate);
    }

    if (pending_manual_candidates_.empty())
        return;

    manual_candidate_due_ = std::chrono::steady_clock::now() + std::chrono::milliseconds(900);
    std::string port_list;
    for (int port : ports) {
        if (!port_list.empty())
            port_list += ",";
        port_list += std::to_string(port);
    }
    AppendStreamLog("LOCAL delayed-manual-candidates scheduled ufrag=" + server_ice_ufrag_ +
                    " ports=" + port_list +
                    " count=" + std::to_string(pending_manual_candidates_.size()));
}

void WebRtcSession::maybe_add_manual_media_candidate() {
    if (!pc_ || manual_candidate_added_ || pending_manual_candidates_.empty())
        return;

    const PeerConnectionState state = peer_connection_get_state(pc_);
    if (state == PEER_CONNECTION_COMPLETED) {
        pending_manual_candidates_.clear();
        return;
    }

    if (manual_candidate_due_.time_since_epoch().count() != 0 &&
        std::chrono::steady_clock::now() < manual_candidate_due_) {
        return;
    }

    // Prefer NVIDIA's trickled endpoint first. This fallback is intentionally
    // delayed so it does not block the real media candidate in libpeer.
    AppendStreamLog("LOCAL delayed-manual-candidates adding ufrag=" + server_ice_ufrag_ +
                    " remoteTrickle=" + std::to_string(remote_trickle_received_ ? 1 : 0) +
                    " count=" + std::to_string(pending_manual_candidates_.size()));
    int added = 0;
    for (auto& candidate : pending_manual_candidates_) {
        if (peer_connection_add_ice_candidate(pc_, candidate.data()) == 0) {
            added++;
            remote_ice_count_++;
            AppendStreamLog("LOCAL delayed-manual-candidate add-ok " + candidate);
        } else {
            AppendStreamLog("LOCAL delayed-manual-candidate rejected " + candidate);
        }
    }

    pending_manual_candidates_.clear();
    manual_candidate_added_ = true;
    if (added > 0) {
        current_state_ = "Added fallback ICE";
    } else {
        current_state_ = "Fallback ICE rejected";
    }
}

void WebRtcSession::maybe_log_stream_diagnostics() {
    if (!settings_.debug_diagnostics)
        return;
    if (!pc_)
        return;

    const auto now = std::chrono::steady_clock::now();
    if (last_stream_diagnostic_log_.time_since_epoch().count() != 0 &&
        now - last_stream_diagnostic_log_ < std::chrono::seconds(2)) {
        return;
    }

    const PeerConnectionState state = peer_connection_get_state(pc_);
    if (state == PEER_CONNECTION_FAILED ||
        state == PEER_CONNECTION_CLOSED ||
        state == PEER_CONNECTION_DISCONNECTED) {
        return;
    }
    if (state != PEER_CONNECTION_CHECKING &&
        state != PEER_CONNECTION_CONNECTED &&
        state != PEER_CONNECTION_COMPLETED &&
        packets_received_.load() == last_logged_packet_count_ &&
        frames_decoded_.load() == last_logged_frame_count_.load(std::memory_order_relaxed) &&
        decode_errors_.load() == last_logged_decode_error_count_.load(std::memory_order_relaxed)) {
        return;
    }

    last_stream_diagnostic_log_ = now;
    log_stream_summary("periodic");
}

void WebRtcSession::maybe_open_datachannel() {
    if (!pc_ || datachannel_open_requested_)
        return;

    const auto now = std::chrono::steady_clock::now();
    if (last_datachannel_open_attempt_.time_since_epoch().count() != 0 &&
        now - last_datachannel_open_attempt_ < std::chrono::milliseconds(750)) {
        return;
    }

    if (datachannel_open_attempts_ >= 60)
        return;

    last_datachannel_open_attempt_ = now;
    datachannel_open_attempts_++;

    char protocol[] = "";

    if (!reliable_input_channel_requested_) {
        char reliable_label[] = "input_channel_v1";
        const int ret = peer_connection_create_datachannel_sid(
            pc_,
            DATA_CHANNEL_RELIABLE,
            0,
            0,
            reliable_label,
            protocol,
            0);

        AppendStreamLog("DATA create_channel label=input_channel_v1 sid=0 type=reliable ret=" +
                        std::to_string(ret) +
                        " attempt=" + std::to_string(datachannel_open_attempts_));
        AppendInputLog("DCEP create sid=0 label=input_channel_v1 attempt=" +
                       std::to_string(datachannel_open_attempts_) +
                       " result=" + std::to_string(ret) +
                       " sctpOpen=" + std::to_string(datachannel_opened_ ? 1 : 0));

        if (ret >= 0)
            reliable_input_channel_requested_ = true;
    }

    // The Switch fallback SCTP backend intentionally uses one reliable channel.
    // Partial reliability requires Forward-TSN negotiation and is optional for GFN input.
    datachannel_open_requested_ = reliable_input_channel_requested_;
    if (datachannel_open_requested_ && input_activation_due_.time_since_epoch().count() == 0) {
        // Prefer the server handshake; use v2 only as a compatibility fallback.
        input_activation_due_ = now + std::chrono::milliseconds(1500);
        AppendStreamLog("DATA fast_channel disabled reason=internal_sctp_no_forward_tsn");
        AppendInputLog("DCEP reliableChannelRequested=1 fastChannel=disabled activationDelayMs=1500");
    }
}

void WebRtcSession::reset_input_channel_for_retry(const char* reason) {
    datachannel_opened_ = false;
    datachannel_open_requested_ = false;
    reliable_input_channel_requested_ = false;
    partial_input_channel_requested_ = false;
    input_ready_ = false;
    startup_control_sent_ = false;
    input_activation_due_ = {};
    last_datachannel_open_attempt_ = {};
    last_input_heartbeat_sent_ = {};
    consecutive_input_send_failures_ = 0;
    AppendInputLog(std::string("RECOVERY input_channel_reset reason=") +
                   (reason ? reason : "unknown") +
                   " attempts=" + std::to_string(datachannel_open_attempts_));
}

void WebRtcSession::note_input_send_result(int sent, const char* label) {
    if (sent >= 0) {
        consecutive_input_send_failures_ = 0;
        return;
    }

    consecutive_input_send_failures_++;
    if (consecutive_input_send_failures_ < 3)
        return;

    // Do not keep acknowledging controller states locally while SCTP rejects
    // them. Pause briefly, then reactivate the negotiated reliable channel.
    input_ready_ = false;
    input_activation_due_ = std::chrono::steady_clock::now() +
                            std::chrono::milliseconds(250);
    AppendInputLog(std::string("RECOVERY input_send_failures label=") +
                   (label ? label : "unknown") +
                   " consecutive=" + std::to_string(consecutive_input_send_failures_) +
                   " action=reactivate");
    consecutive_input_send_failures_ = 0;
}

void WebRtcSession::maybe_activate_input() {
    if (input_ready_ || !datachannel_open_requested_ || !datachannel_opened_ || !pc_)
        return;

    if (input_activation_due_.time_since_epoch().count() == 0 ||
        std::chrono::steady_clock::now() < input_activation_due_)
        return;

    input_ready_ = true;
    input_protocol_version_ = 2;
    AppendStreamLog("DATA input_ready fallback=v2 controller=xbox sidReliable=0 sidFast=2");
    AppendInputLog("HANDSHAKE fallback protocol=2 inputReady=1 controller=xbox sid=0");
}

void WebRtcSession::send_datachannel_text(const std::string& label, const std::string& payload) {
    if (!pc_ || payload.empty())
        return;

    const int sent = peer_connection_datachannel_send(pc_, const_cast<char*>(payload.c_str()), payload.size());
    AppendStreamLog("DATA tx label=" + label +
                    " opened=" + std::to_string(datachannel_opened_ ? 1 : 0) +
                    " sent=" + std::to_string(sent) +
                    " bytes=" + std::to_string(payload.size()) +
                    " payload=" + PreviewText(payload, 220));
}

int WebRtcSession::send_datachannel_binary(uint16_t sid, const std::string& label, const uint8_t* payload, size_t size) {
    if (!pc_ || !payload || size == 0)
        return -1;

    const int sent = peer_connection_datachannel_send_binary_sid(
        pc_,
        const_cast<char*>(reinterpret_cast<const char*>(payload)),
        size,
        sid);
    note_input_send_result(sent, label.c_str());
    // Controller reports are sent every frame; logging each one would create
    // input latency and make the useful SCTP diagnostics unreadable.
    const bool high_frequency_input = label == "gamepad" || label == "input_heartbeat";
    if (!high_frequency_input || gamepad_tx_count_ < 5 || gamepad_tx_count_ % 120 == 0) {
        AppendStreamLog("DATA tx-binary label=" + label +
                        " sid=" + std::to_string(sid) +
                        " sent=" + std::to_string(sent) +
                        " bytes=" + std::to_string(size) +
                        " hex=" + HexPreview(payload, size, 16));
    }
    return sent;
}

void WebRtcSession::maybe_send_input_heartbeat() {
    if (!pc_ || !input_ready_ || !reliable_input_channel_requested_)
        return;

    const auto now = std::chrono::steady_clock::now();
    if (last_input_heartbeat_sent_.time_since_epoch().count() != 0 &&
        now - last_input_heartbeat_sent_ < std::chrono::seconds(2)) {
        return;
    }

    last_input_heartbeat_sent_ = now;
    input_heartbeat_tx_count_++;

    uint8_t heartbeat[4] = {2, 0, 0, 0}; // INPUT_HEARTBEAT, little-endian.
    const int sent = send_datachannel_binary(
        0, "input_heartbeat", heartbeat, sizeof(heartbeat));

    if (input_heartbeat_tx_count_ <= 5 || input_heartbeat_tx_count_ % 20 == 0) {
        AppendStreamLog("DATA input_heartbeat sent=" + std::to_string(sent) +
                        " count=" + std::to_string(input_heartbeat_tx_count_) +
                        " protocol=" + std::to_string(input_protocol_version_));
        AppendInputLog("HEARTBEAT tx count=" + std::to_string(input_heartbeat_tx_count_) +
                       " sent=" + std::to_string(sent) +
                       " protocol=" + std::to_string(input_protocol_version_));
    }
}

void WebRtcSession::maybe_send_startup_control_messages() {
    if (!pc_ || startup_control_sent_ || !datachannel_open_requested_)
        return;

    startup_control_sent_ = true;
    AppendStreamLog("DATA startup_channels requested reliable=" +
                    std::to_string(reliable_input_channel_requested_ ? 1 : 0) +
                    " partial=" + std::to_string(partial_input_channel_requested_ ? 1 : 0) +
                    " thresholdMs=" + std::to_string(partial_reliable_threshold_ms_));
}

void WebRtcSession::maybe_request_startup_keyframe_retry() {
    if (!pc_ || !peer_completed_seen_ || packets_received_.load() > 0)
        return;

    const PeerConnectionState state = peer_connection_get_state(pc_);
    if (state != PEER_CONNECTION_COMPLETED)
        return;

    const auto now = std::chrono::steady_clock::now();
    if (last_startup_keyframe_request_.time_since_epoch().count() != 0 &&
        now - last_startup_keyframe_request_ < std::chrono::seconds(3)) {
        return;
    }

    if (keyframe_request_attempts_ >= 5)
        return;

    last_startup_keyframe_request_ = now;
    request_keyframe("startup_no_video");
}

void WebRtcSession::maybe_recover_decode_stall() {
    const uint64_t last_decoded_us = last_decoded_frame_at_us_.load(std::memory_order_acquire);
    if (!pc_ || frames_decoded_.load() == 0 || last_decoded_us == 0) {
        return;
    }

    const auto now = std::chrono::steady_clock::now();
    const uint64_t now_us = NowUs();
    const uint64_t stalled_ms = now_us >= last_decoded_us ? (now_us - last_decoded_us) / 1000 : 0;
    if (stalled_ms < 1500)
        return;

    if (last_decode_stall_request_at_.time_since_epoch().count() != 0 &&
        now - last_decode_stall_request_at_ < std::chrono::seconds(2)) {
        return;
    }

    // A completed connection can still stop producing complete H.264 pictures.
    // Ask for a fresh IDR promptly, but never flood the signaling channel.
    if (keyframe_request_attempts_ >= 12)
        return;

    last_decode_stall_request_at_ = now;
    AppendStreamLog("DECODE timeoutMs=" + std::to_string(stalled_ms) +
                    " packets=" + std::to_string(packets_received_.load()) +
                    " frames=" + std::to_string(frames_decoded_.load()));
    {
        std::lock_guard<std::mutex> queue_lock(decoder_queue_mutex_);
        decoder_queue_drops_ += static_cast<int>(decoder_queue_.size());
        clear_decoder_queue_locked();
    }
    decoder_resync_required_.store(true);
    if (!decoder_ || !decoder_->uses_hardware_frames())
        decoder_reset_requested_.store(true);
    request_keyframe("decode_timeout");
}

void WebRtcSession::maybe_recover_rtp_damage() {
    if (!pc_)
        return;
    PeerVideoRtpStats stats {};
    if (peer_connection_get_video_rtp_stats(pc_, &stats) != 0 ||
        stats.access_units_dropped <= last_rtp_access_units_dropped_) {
        return;
    }

    const uint32_t newly_dropped = stats.access_units_dropped - last_rtp_access_units_dropped_;
    last_rtp_access_units_dropped_ = stats.access_units_dropped;
    const auto now = std::chrono::steady_clock::now();
    if (last_rtp_damage_request_at_.time_since_epoch().count() == 0 ||
        now - last_rtp_damage_request_at_ >= std::chrono::seconds(1)) {
        last_rtp_damage_request_at_ = now;
        AppendStreamLog("RTP damage auDroppedNew/total=" + std::to_string(newly_dropped) +
                        "/" + std::to_string(stats.access_units_dropped) +
                        " gaps=" + std::to_string(stats.sequence_gaps) +
                        " reordered=" + std::to_string(stats.reordered_packets) +
                        " late=" + std::to_string(stats.late_packets_dropped) +
                        " forced=" + std::to_string(stats.forced_sequence_skips));
    }
    // Once a reference picture is missing, subsequent P frames are not safe to
    // display on either backend. Hold the previous good frame and resume from
    // an IDR; only the software decoder is flushed because Deko3D can still own
    // references to NVDEC output surfaces.
    if (!decoder_resync_required_.exchange(true)) {
        std::lock_guard<std::mutex> queue_lock(decoder_queue_mutex_);
        decoder_queue_drops_ += static_cast<int>(decoder_queue_.size());
        clear_decoder_queue_locked();
        if (!decoder_ || !decoder_->uses_hardware_frames())
            decoder_reset_requested_.store(true);
        keyframe_needed_.store(true);
    }
    // Do not flush healthy queued frames or request an IDR for every damaged
    // access unit. Large IDRs amplify packet bursts and previously created a
    // self-sustaining PLI/loss loop. maybe_recover_decode_stall() remains the
    // authoritative recovery path when output actually stops.
}

void WebRtcSession::log_stream_summary(const char* reason) {
    const auto now = std::chrono::steady_clock::now();
    long long since_start_ms = -1;
    long long since_completed_ms = -1;

    if (session_started_at_.time_since_epoch().count() != 0) {
        since_start_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            now - session_started_at_).count();
    }
    if (peer_completed_at_.time_since_epoch().count() != 0) {
        since_completed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            now - peer_completed_at_).count();
    }

    const VideoPerformanceCounters video = get_video_performance();
    double metric_seconds = 0.0;
    if (last_video_metric_snapshot_.time_since_epoch().count() != 0) {
        metric_seconds = std::chrono::duration<double>(now - last_video_metric_snapshot_).count();
    }
    const auto rate = [metric_seconds](uint64_t current, uint64_t previous) {
        return metric_seconds > 0.0 && current >= previous
            ? static_cast<double>(current - previous) / metric_seconds
            : 0.0;
    };
    const double incoming_fps = rate(video.access_units, last_logged_video_performance_.access_units);
    const double decoded_fps = rate(video.decoded_frames, last_logged_video_performance_.decoded_frames);
    const double presented_fps = rate(video.presented_frames, last_logged_video_performance_.presented_frames);
    const double bitrate_mbps = rate(video.access_unit_bytes, last_logged_video_performance_.access_unit_bytes) * 8.0 / 1000000.0;
    last_video_metric_snapshot_ = now;
    last_logged_video_performance_ = video;

    int pair_total = 0;
    int pair_frozen = 0;
    int pair_checking = 0;
    int pair_succeeded = 0;
    int pair_failed = 0;
    PeerVideoRtpStats rtp_stats {};
    if (pc_) {
        peer_connection_get_ice_candidate_pair_stats(
            pc_, &pair_total, &pair_frozen, &pair_checking, &pair_succeeded, &pair_failed);
        (void)peer_connection_get_video_rtp_stats(pc_, &rtp_stats);
    }
    const auto& frame_holder = AVFrameHolder::instance();
    VideoRenderStats render_backend_stats {};
    if (renderer_) {
        if (const auto* stats = renderer_->video_render_stats())
            render_backend_stats = *stats;
    }

    AppendStreamLog("STREAM diag reason=" + std::string(reason ? reason : "unknown") +
                    " state=" + current_state_ +
                    " sinceStartMs=" + std::to_string(since_start_ms) +
                    " sinceCompletedMs=" + std::to_string(since_completed_ms) +
                    " signalingRx=" + std::to_string(signaling_rx_count_) +
                    " offers=" + std::to_string(offer_count_) +
                    " iceRemote=" + std::to_string(remote_ice_count_) +
                    " iceLocal=" + std::to_string(local_ice_count_) +
                    " icePairs=" + std::to_string(pair_total) +
                    "/" + std::to_string(pair_frozen) +
                    "/" + std::to_string(pair_checking) +
                    "/" + std::to_string(pair_succeeded) +
                    "/" + std::to_string(pair_failed) +
                    " hbRx=" + std::to_string(heartbeat_rx_count_) +
                    " hbTx=" + std::to_string(heartbeat_tx_count_) +
                    " dcOpen=" + std::to_string(datachannel_opened_ ? 1 : 0) +
                    " dcReq=" + std::to_string(datachannel_open_requested_ ? 1 : 0) +
                    " dcAttempts=" + std::to_string(datachannel_open_attempts_) +
                    " dcStartup=" + std::to_string(startup_control_sent_ ? 1 : 0) +
                    " rel=" + std::to_string(reliable_input_channel_requested_ ? 1 : 0) +
                    " pr=" + std::to_string(partial_input_channel_requested_ ? 1 : 0) +
                    " inputReady=" + std::to_string(input_ready_ ? 1 : 0) +
                    " inputProto=" + std::to_string(input_protocol_version_) +
                    " inputHbTx=" + std::to_string(input_heartbeat_tx_count_) +
                    " packets=" + std::to_string(packets_received_.load()) +
                    " frames=" + std::to_string(frames_decoded_.load()) +
                    " decodeErrors=" + std::to_string(decode_errors_.load()) +
                    " keyframes=" + std::to_string(keyframe_request_attempts_) +
                    " videoBackend=" + video_backend_name_ +
                    " in/dec/outFps=" + std::to_string(incoming_fps) + "/" +
                    std::to_string(decoded_fps) + "/" + std::to_string(presented_fps) +
                    " bitrateMbps=" + std::to_string(bitrate_mbps) +
                    " auBytesTotal/max=" + std::to_string(video.access_unit_bytes) + "/" +
                    std::to_string(video_access_unit_max_bytes_.load()) +
                    " decodeUsP95/max=" + std::to_string(video.decode_us_p95) + "/" +
                    std::to_string(video.decode_us_max) +
                    " queueWaitUsP95/max=" + std::to_string(video.queue_wait_us_p95) + "/" +
                    std::to_string(video.queue_wait_us_max) +
                    " renderUsP95/max=" + std::to_string(video.render_us_p95) + "/" +
                    std::to_string(video.render_us_max) +
                    " decodeQueue/current/high=" + std::to_string(video.decode_queue_size) + "/" +
                    std::to_string(video.decode_queue_high_water) +
                    " bufferPool reuse/alloc=" + std::to_string(decoder_buffer_reuses_.load()) + "/" +
                    std::to_string(decoder_buffer_allocations_.load()) +
                    " transport batches/datagrams/max=" +
                    std::to_string(transport_batches_.load()) + "/" +
                    std::to_string(transport_datagrams_.load()) + "/" +
                    std::to_string(transport_batch_high_water_.load()) +
                    " gpu retained/mappings=" +
                    std::to_string(render_backend_stats.retained_surfaces) + "/" +
                    std::to_string(render_backend_stats.surface_mappings) +
                    " rtp packets/gaps/reordered/late/forced/buffered=" +
                    std::to_string(rtp_stats.packets_received) + "/" +
                    std::to_string(rtp_stats.sequence_gaps) + "/" +
                    std::to_string(rtp_stats.reordered_packets) + "/" +
                    std::to_string(rtp_stats.late_packets_dropped) + "/" +
                    std::to_string(rtp_stats.forced_sequence_skips) + "/" +
                    std::to_string(rtp_stats.reorder_buffered_packets) +
                    " nack requests/packets=" +
                    std::to_string(rtp_stats.nack_requests) + "/" +
                    std::to_string(rtp_stats.nack_packets_requested) +
                    " rtpAu ok/drop=" + std::to_string(rtp_stats.access_units_completed) + "/" +
                    std::to_string(rtp_stats.access_units_dropped) +
                    " frameQueue size/dropTiming/dropOverflow/hold/reuse=" +
                    std::to_string(frame_holder.getFrameQueueSize()) + "/" +
                    std::to_string(frame_holder.getTimingDropStat()) + "/" +
                    std::to_string(frame_holder.getOverflowDropStat()) + "/" +
                    std::to_string(frame_holder.getTimingHoldStat()) + "/" +
                    std::to_string(frame_holder.getFakeFrameStat()));
}

void WebRtcSession::request_keyframe(const char* reason) {
    const auto now = std::chrono::steady_clock::now();
    constexpr auto kMinimumPliInterval = std::chrono::milliseconds(1500);
    if (last_keyframe_request_at_.time_since_epoch().count() != 0 &&
        now - last_keyframe_request_at_ < kMinimumPliInterval) {
        // Keep one coalesced request pending. poll() will retry it after the
        // cooldown instead of flooding the server with expensive IDR frames.
        if (decoder_resync_required_.load())
            keyframe_needed_.store(true);
        return;
    }

    last_keyframe_request_at_ = now;
    keyframe_request_attempts_++;
    const int pli_result = pc_ ? peer_connection_request_video_keyframe(pc_) : -1;
    AppendStreamLog("KEYFRAME request reason=" + std::string(reason ? reason : "switch_stream") +
                    " attempt=" + std::to_string(keyframe_request_attempts_) +
                    " rtcpPli=" + std::to_string(pli_result) +
                    " packets=" + std::to_string(packets_received_.load()) +
                    " frames=" + std::to_string(frames_decoded_.load()) +
                    " backlog=" + std::to_string(std::max(0, packets_received_.load() - frames_decoded_.load())));

    if (pli_result >= 0 || !signaling_client_ || remote_peer_id_ <= 0)
        return;

    // Keep the legacy signaling request as a fallback until the video SSRC is known.
    json_t* req = json_object();
    json_object_set_new(req, "type", json_string("request_keyframe"));
    json_object_set_new(req, "reason", json_string(reason ? reason : "switch_stream"));
    json_object_set_new(req, "backlogFrames", json_integer(std::max(0, packets_received_.load() - frames_decoded_.load())));
    json_object_set_new(req, "attempt", json_integer(keyframe_request_attempts_));
    send_peer_payload(req);
    json_decref(req);
}

void WebRtcSession::on_video_packet(const PeerVideoPacket& packet) {
    const uint8_t* data = packet.data;
    const size_t size = packet.size;
    last_video_packet_at_us_.store(NowUs(), std::memory_order_release);
    video_access_unit_bytes_.fetch_add(size, std::memory_order_relaxed);
    AtomicMax(video_access_unit_max_bytes_, static_cast<uint64_t>(size));
    video_ssrc_ = packet.ssrc;
    for (const auto& report : sender_reports_) {
        if (report.ssrc == video_ssrc_) {
            video_sr_ntp_us_ = report.ntp_us;
            video_sr_rtp_timestamp_ = report.rtp_timestamp;
            have_video_sender_report_ = true;
            break;
        }
    }
    const int packet_count = packets_received_.fetch_add(1) + 1;
    if (!first_video_packet_logged_) {
        first_video_packet_logged_ = true;
        AppendStreamLog("VIDEO first_access_unit size=" + std::to_string(size) +
                        " idr=" + std::to_string(ContainsH264Idr(data, size) ? 1 : 0) +
                        " preview=" + HexPreview(data, size));
        log_stream_summary("first_video_packet");
    } else if (packet_count - last_logged_packet_count_ >= 120) {
        AppendStreamLog("VIDEO access_unit_progress units=" + std::to_string(packet_count) +
                        " size=" + std::to_string(size) +
                        " frames=" + std::to_string(frames_decoded_.load()) +
                        " decodeErrors=" + std::to_string(decode_errors_.load()) +
                        " queueDrops=" + std::to_string(decoder_queue_drops_.load()));
        last_logged_packet_count_ = packet_count;
    }

    enqueue_decode_unit(data, size, packet.timestamp);
}

void WebRtcSession::on_audio_packet(const PeerAudioPacket& packet) {
    const bool new_audio_ssrc = audio_ssrc_ != packet.ssrc;
    audio_ssrc_ = packet.ssrc;
    if (audio_) {
        audio_->submit(packet);
        if (new_audio_ssrc) {
            for (const auto& report : sender_reports_) {
                if (report.ssrc == audio_ssrc_) {
                    audio_->set_sender_report(report.ssrc, report.ntp_us, report.rtp_timestamp);
                    break;
                }
            }
        }
    }
}

void WebRtcSession::on_rtp_sender_report(uint32_t ssrc, uint64_t ntp_us, uint32_t rtp_timestamp) {
    std::lock_guard<std::recursive_mutex> lock(peer_mutex_);
    auto found = std::find_if(sender_reports_.begin(), sender_reports_.end(), [ssrc](const SenderReport& item) {
        return item.ssrc == ssrc;
    });
    if (found == sender_reports_.end())
        sender_reports_.push_back({ssrc, ntp_us, rtp_timestamp});
    else
        *found = {ssrc, ntp_us, rtp_timestamp};

    if (ssrc == audio_ssrc_ && audio_)
        audio_->set_sender_report(ssrc, ntp_us, rtp_timestamp);
    if (ssrc == video_ssrc_) {
        video_sr_ntp_us_ = ntp_us;
        video_sr_rtp_timestamp_ = rtp_timestamp;
        have_video_sender_report_ = true;
    }
}

int64_t WebRtcSession::video_target_rtp_timestamp() const {
    std::lock_guard<std::recursive_mutex> lock(peer_mutex_);
    if (!audio_ || !have_video_sender_report_)
        return AV_NOPTS_VALUE;
    const int64_t audio_ntp = audio_->playback_ntp_us();
    if (audio_ntp < 0)
        return AV_NOPTS_VALUE;
    return opennow::audio::RtpTimestampAtNtp(
        video_sr_rtp_timestamp_, static_cast<int64_t>(video_sr_ntp_us_), audio_ntp, 90000);
}

VideoPerformanceCounters WebRtcSession::get_video_performance() const {
    VideoPerformanceCounters result;
    result.access_units = static_cast<uint64_t>(packets_received_.load(std::memory_order_relaxed));
    result.access_unit_bytes = video_access_unit_bytes_.load(std::memory_order_relaxed);
    result.decoded_frames = static_cast<uint64_t>(frames_decoded_.load(std::memory_order_relaxed));
    result.presented_frames = presented_frames_.load(std::memory_order_relaxed);
    result.decode_us_p95 = LatencyPercentile95(decode_latency_buckets_);
    result.decode_us_max = decode_us_max_.load(std::memory_order_relaxed);
    result.queue_wait_us_p95 = LatencyPercentile95(queue_wait_buckets_);
    result.queue_wait_us_max = queue_wait_us_max_.load(std::memory_order_relaxed);
    result.render_us_p95 = LatencyPercentile95(render_latency_buckets_);
    result.render_us_max = render_us_max_.load(std::memory_order_relaxed);
    result.decode_queue_high_water = decoder_queue_high_water_.load(std::memory_order_relaxed);
    {
        std::lock_guard<std::mutex> queue_lock(decoder_queue_mutex_);
        result.decode_queue_size = decoder_queue_.size();
    }
    return result;
}

StreamTransportHealth WebRtcSession::get_transport_health() const {
    StreamTransportHealth health;
    health.peer_completed = peer_ever_completed_.load(std::memory_order_acquire);
    health.peer_terminal = static_cast<opennow::PeerTerminalKind>(
        peer_terminal_kind_.load(std::memory_order_acquire));
    health.signaling_connected = signaling_client_ && signaling_client_->is_connected();
    const uint64_t last_packet_us = last_video_packet_at_us_.load(std::memory_order_acquire);
    health.video_started = last_packet_us != 0;
    if (last_packet_us != 0) {
        const uint64_t now_us = NowUs();
        health.video_idle = std::chrono::milliseconds(
            now_us > last_packet_us ? (now_us - last_packet_us) / 1000 : 0);
    }
    return health;
}

std::string WebRtcSession::get_debug_info() const {
    std::lock_guard<std::recursive_mutex> lock(peer_mutex_);
    char buf[1800];
    std::string msg_log;
    for (const auto& m : last_messages_) {
        msg_log += m + "\n";
    }
    int pair_total = 0;
    int pair_frozen = 0;
    int pair_checking = 0;
    int pair_succeeded = 0;
    int pair_failed = 0;
    if (pc_) {
        peer_connection_get_ice_candidate_pair_stats(
            pc_, &pair_total, &pair_frozen, &pair_checking, &pair_succeeded, &pair_failed);
    }
    size_t decode_queue_size = 0;
    size_t decode_pool_size = 0;
    {
        std::lock_guard<std::mutex> queue_lock(decoder_queue_mutex_);
        decode_queue_size = decoder_queue_.size();
        decode_pool_size = decoder_buffer_pool_.size();
    }
    PeerVideoRtpStats rtp_stats {};
    if (pc_)
        (void)peer_connection_get_video_rtp_stats(pc_, &rtp_stats);
    const auto& holder = AVFrameHolder::instance();
    const VideoPerformanceCounters video = get_video_performance();
    snprintf(buf, sizeof(buf),
             "URL: %s\nMedia: %s:%d\nICE servers: %zu\nPreset: %s %dx%d@%d %d kbps\nState: %s\nSignaling RX: %d  Offers: %d  ICE remote/local: %d/%d  HB rx/tx: %d/%d\nICE pairs total/frozen/checking/ok/failed: %d/%d/%d/%d/%d\nDataChannel SCTP/requested/attempts/start: %d/%d/%d/%d\nInput rel/pr/ready/proto/hb/threshold: %d/%d/%d/%d/%d/%d\nXbox reports/mouse clicks: %d/%d\nVideo backend: %s\nVideo access units/bytes/max: %d/%llu/%llu\nDecoded/presented/errors: %d/%llu/%d\nDecode us p95/max: %llu/%llu  queue wait p95/max: %llu/%llu\nRender us p95/max: %llu/%llu\nDecode queue/current/high/dropped: %zu/%zu/%d\nFrame queue: %zu  Dropped: %zu  Reused: %zu\n\nLast Messages:\n%s",
             signaling_url_.c_str(),
             media_ip_.empty() ? "(auto)" : media_ip_.c_str(),
             media_port_,
             ice_servers_.size(),
             settings_.label.c_str(),
             settings_.width,
             settings_.height,
             settings_.fps,
             settings_.bitrate_kbps,
             current_state_.c_str(),
             signaling_rx_count_,
             offer_count_,
             remote_ice_count_,
             local_ice_count_,
             heartbeat_rx_count_,
             heartbeat_tx_count_,
             pair_total,
             pair_frozen,
             pair_checking,
             pair_succeeded,
             pair_failed,
             datachannel_opened_ ? 1 : 0,
             datachannel_open_requested_ ? 1 : 0,
             datachannel_open_attempts_,
             startup_control_sent_ ? 1 : 0,
             reliable_input_channel_requested_ ? 1 : 0,
             partial_input_channel_requested_ ? 1 : 0,
             input_ready_ ? 1 : 0,
             input_protocol_version_,
             input_heartbeat_tx_count_,
             partial_reliable_threshold_ms_,
             gamepad_tx_count_,
             mouse_tx_count_,
             video_backend_name_.c_str(),
             packets_received_.load(),
             static_cast<unsigned long long>(video.access_unit_bytes),
             static_cast<unsigned long long>(video_access_unit_max_bytes_.load()),
             frames_decoded_.load(),
             static_cast<unsigned long long>(video.presented_frames),
             decode_errors_.load(),
             static_cast<unsigned long long>(video.decode_us_p95),
             static_cast<unsigned long long>(video.decode_us_max),
             static_cast<unsigned long long>(video.queue_wait_us_p95),
             static_cast<unsigned long long>(video.queue_wait_us_max),
             static_cast<unsigned long long>(video.render_us_p95),
             static_cast<unsigned long long>(video.render_us_max),
             decode_queue_size,
             video.decode_queue_high_water,
             decoder_queue_drops_.load(),
             holder.getFrameQueueSize(),
             holder.getFrameDropStat(),
             holder.getFakeFrameStat(),
             msg_log.c_str());
    std::string result(buf);
    result += "\nAU buffers pool/reuse/alloc: " + std::to_string(decode_pool_size) + "/" +
              std::to_string(decoder_buffer_reuses_.load()) + "/" +
              std::to_string(decoder_buffer_allocations_.load());
    result += "\nRTP packets/gaps/reordered/late/forced: " +
              std::to_string(rtp_stats.packets_received) + "/" +
              std::to_string(rtp_stats.sequence_gaps) + "/" +
              std::to_string(rtp_stats.reordered_packets) + "/" +
              std::to_string(rtp_stats.late_packets_dropped) + "/" +
              std::to_string(rtp_stats.forced_sequence_skips);
    result += "\nRTP access units ok/dropped: " +
              std::to_string(rtp_stats.access_units_completed) + "/" +
              std::to_string(rtp_stats.access_units_dropped);
    result += "\nRTCP NACK requests/packets: " +
              std::to_string(rtp_stats.nack_requests) + "/" +
              std::to_string(rtp_stats.nack_packets_requested);
    result += "\nFrame timing drop/overflow/hold: " +
              std::to_string(holder.getTimingDropStat()) + "/" +
              std::to_string(holder.getOverflowDropStat()) + "/" +
              std::to_string(holder.getTimingHoldStat());
    if (audio_) {
        result += "\n" + audio_->debug_info();
        const int64_t target = video_target_rtp_timestamp();
        const int64_t rendered = last_rendered_video_pts_.load();
        if (target != AV_NOPTS_VALUE && rendered != AV_NOPTS_VALUE) {
            const int32_t delta_ticks = static_cast<int32_t>(
                static_cast<uint32_t>(rendered) - static_cast<uint32_t>(target));
            result += "\nA/V delta: " + std::to_string(delta_ticks / 90) + " ms";
        } else {
            result += "\nA/V delta: waiting for RTCP SR";
        }
    }
    return result;
}
