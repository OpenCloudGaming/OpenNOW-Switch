#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <span>
#include <string>
#include <vector>

#include "opennow/core/Settings.hpp"
#include "opennow/gfn/GfnTypes.hpp"
#include "opennow/gfn/InputEncoder.hpp"
#include "opennow/net/HttpClient.hpp"

namespace opennow::media {

struct PipelineStatus {
    bool ready = false;
    std::string message;
};

struct NativeMediaCapabilities {
    bool wssSignaling = false;
    bool sdpNegotiation = true;
    bool ice = false;
    bool dtlsSrtp = false;
    bool rtp = false;
    bool h264Decode = false;
    bool videoRender = false;
    bool audio = false;
    std::string message;

    bool ready() const;
    bool transportReady() const;
};

struct EncodedVideoFrame {
    std::vector<std::uint8_t> bytes;
    std::uint64_t timestampUs = 0;
};

struct PcmAudioFrame {
    std::vector<std::int16_t> samples;
    std::uint32_t sampleRate = 48000;
    std::uint8_t channels = 2;
};

class VideoDecoder {
public:
    virtual ~VideoDecoder() = default;
    virtual PipelineStatus open(const StreamSettings& settings) = 0;
    virtual PipelineStatus submit(const EncodedVideoFrame& frame) = 0;
};

class VideoRenderer {
public:
    virtual ~VideoRenderer() = default;
    virtual PipelineStatus open(const StreamSettings& settings) = 0;
    virtual PipelineStatus present() = 0;
};

class AudioRenderer {
public:
    virtual ~AudioRenderer() = default;
    virtual PipelineStatus open(const StreamSettings& settings) = 0;
    virtual PipelineStatus submit(const PcmAudioFrame& frame) = 0;
};

class NativeMediaPipeline {
public:
    NativeMediaPipeline();
    ~NativeMediaPipeline();

    NativeMediaPipeline(const NativeMediaPipeline&) = delete;
    NativeMediaPipeline& operator=(const NativeMediaPipeline&) = delete;

    PipelineStatus open(
        const StreamSettings& settings,
        const SessionInfo& session,
        net::HttpClient* http = nullptr,
        std::function<void(const std::string&)> log = {});
    PipelineStatus close();
    std::string connectionState() const;
    bool mediaConnected() const;
    bool inputReady() const;
    bool sendGamepadInput(const gfn::GamepadInput& input, std::uint16_t connectedBitmap = 0x0101);
    std::size_t videoBytesReceived() const;
    std::size_t audioBytesReceived() const;

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

NativeMediaCapabilities nativeMediaCapabilities();

} // namespace opennow::media
