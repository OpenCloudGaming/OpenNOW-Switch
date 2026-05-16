#include "opennow/media/NativeMediaPipeline.hpp"

#include "opennow/gfn/Sdp.hpp"
#include "opennow/media/GfnWebRtcClient.hpp"
#include "opennow/media/VideoFrameStore.hpp"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <mutex>
#include <sstream>
#include <thread>
#include <utility>
#include <vector>

#if defined(OPENNOW_HAS_FFMPEG_DECODER)
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/channel_layout.h>
#include <libavutil/error.h>
#include <libavutil/frame.h>
#include <libavutil/hwcontext.h>
#include <libavutil/imgutils.h>
#include <libavutil/mem.h>
#include <libswresample/swresample.h>
#include <libswscale/swscale.h>
}
#endif

#if defined(OPENNOW_PLATFORM_SWITCH)
#include <malloc.h>
#include <sys/stat.h>
#include <switch.h>
#endif

namespace opennow::media {
namespace {

#if defined(OPENNOW_HAS_FFMPEG_DECODER)
std::string avError(int code) {
    char buffer[AV_ERROR_MAX_STRING_SIZE] = {};
    av_strerror(code, buffer, sizeof(buffer));
    return buffer;
}
#endif

void defaultLog(const std::string& message) {
    std::fprintf(stdout, "%s\n", message.c_str());
    std::fflush(stdout);
    std::fprintf(stderr, "%s\n", message.c_str());
    std::fflush(stderr);
#if defined(OPENNOW_PLATFORM_SWITCH)
    (void)::mkdir("sdmc:/switch", 0777);
    (void)::mkdir("sdmc:/switch/OpenNOW", 0777);
    FILE* file = std::fopen("sdmc:/switch/OpenNOW/media.log", "ab");
    if (file) {
        std::fprintf(file, "%s\n", message.c_str());
        std::fflush(file);
        std::fclose(file);
    }
#endif
}

#if defined(OPENNOW_HAS_FFMPEG_DECODER)
class H264FrameDecoder {
public:
    ~H264FrameDecoder() {
        close();
    }

    PipelineStatus open(bool preferNvtegra) {
        close();
        const AVCodec* codec = nullptr;
        if (preferNvtegra) {
            codec = avcodec_find_decoder_by_name("h264_nvtegra");
        }
        if (!codec) {
            codec = avcodec_find_decoder(AV_CODEC_ID_H264);
        }
        if (!codec) {
            return {false, "FFmpeg H264 decoder is not available."};
        }

        parser_ = av_parser_init(AV_CODEC_ID_H264);
        if (!parser_) {
            return {false, "FFmpeg H264 parser init failed."};
        }

        codecCtx_ = avcodec_alloc_context3(codec);
        packet_ = av_packet_alloc();
        decoded_ = av_frame_alloc();
        transferred_ = av_frame_alloc();
        if (!codecCtx_ || !packet_ || !decoded_ || !transferred_) {
            return {false, "FFmpeg H264 allocation failed."};
        }

        codecCtx_->flags2 |= AV_CODEC_FLAG2_FAST;
        codecCtx_->thread_count = 2;

#if defined(OPENNOW_HAS_FFMPEG_NVTEGRA)
        if (preferNvtegra && av_hwdevice_ctx_create(&hwDevice_, AV_HWDEVICE_TYPE_NVTEGRA, nullptr, nullptr, 0) == 0) {
            codecCtx_->hw_device_ctx = av_buffer_ref(hwDevice_);
            usingHardware_ = codecCtx_->hw_device_ctx != nullptr;
        }
#else
        (void)preferNvtegra;
#endif

        const int opened = avcodec_open2(codecCtx_, codec, nullptr);
        if (opened < 0) {
            return {false, "FFmpeg H264 decoder open failed: " + avError(opened)};
        }
        return {true, std::string("H264 decoder opened with ") + codec->name + (usingHardware_ ? " + NVTEGRA." : ".")};
    }

    void submit(const std::uint8_t* data, std::size_t size) {
        if (!codecCtx_ || !parser_ || !packet_ || !data || size == 0) {
            return;
        }

        const auto* cursor = data;
        auto remaining = static_cast<int>(size);
        while (remaining > 0) {
            std::uint8_t* parsedData = nullptr;
            int parsedSize = 0;
            const int consumed = av_parser_parse2(
                parser_,
                codecCtx_,
                &parsedData,
                &parsedSize,
                cursor,
                remaining,
                AV_NOPTS_VALUE,
                AV_NOPTS_VALUE,
                0);
            if (consumed < 0) {
                return;
            }
            cursor += consumed;
            remaining -= consumed;
            if (parsedSize > 0) {
                packet_->data = parsedData;
                packet_->size = parsedSize;
                decodePacket(packet_);
                packet_->data = nullptr;
                packet_->size = 0;
            }
            if (consumed == 0) {
                break;
            }
        }
    }

    void close() {
        if (sws_) {
            sws_freeContext(sws_);
            sws_ = nullptr;
        }
        if (parser_) {
            av_parser_close(parser_);
            parser_ = nullptr;
        }
        if (codecCtx_) {
            avcodec_free_context(&codecCtx_);
        }
        if (packet_) {
            av_packet_free(&packet_);
        }
        if (decoded_) {
            av_frame_free(&decoded_);
        }
        if (transferred_) {
            av_frame_free(&transferred_);
        }
        if (hwDevice_) {
            av_buffer_unref(&hwDevice_);
        }
        rgba_.clear();
        usingHardware_ = false;
    }

private:
    void decodePacket(AVPacket* packet) {
        if (avcodec_send_packet(codecCtx_, packet) < 0) {
            return;
        }
        while (true) {
            const int received = avcodec_receive_frame(codecCtx_, decoded_);
            if (received == AVERROR(EAGAIN) || received == AVERROR_EOF) {
                break;
            }
            if (received < 0) {
                break;
            }
            publishDecodedFrame(decoded_);
            av_frame_unref(decoded_);
        }
    }

    void publishDecodedFrame(AVFrame* frame) {
        if (hasPendingVideoFrame()) {
            av_frame_unref(transferred_);
            return;
        }

        AVFrame* source = frame;
        if ((frame->format == AV_PIX_FMT_DRM_PRIME || usingHardware_) && av_hwframe_transfer_data(transferred_, frame, 0) == 0) {
            source = transferred_;
        }
        if (source->width <= 0 || source->height <= 0) {
            av_frame_unref(transferred_);
            return;
        }

        const auto srcFormat = static_cast<AVPixelFormat>(source->format);
        sws_ = sws_getCachedContext(
            sws_,
            source->width,
            source->height,
            srcFormat,
            source->width,
            source->height,
            AV_PIX_FMT_RGBA,
            SWS_FAST_BILINEAR,
            nullptr,
            nullptr,
            nullptr);
        if (!sws_) {
            av_frame_unref(transferred_);
            return;
        }

        const std::size_t required = static_cast<std::size_t>(source->width) * source->height * 4;
        rgba_.resize(required);
        std::uint8_t* dstData[4] = {rgba_.data(), nullptr, nullptr, nullptr};
        int dstLinesize[4] = {source->width * 4, 0, 0, 0};
        sws_scale(sws_, source->data, source->linesize, 0, source->height, dstData, dstLinesize);
        publishVideoFrame(static_cast<std::uint32_t>(source->width), static_cast<std::uint32_t>(source->height), rgba_.data(), rgba_.size());
        av_frame_unref(transferred_);
    }

    AVCodecParserContext* parser_ = nullptr;
    AVCodecContext* codecCtx_ = nullptr;
    AVPacket* packet_ = nullptr;
    AVFrame* decoded_ = nullptr;
    AVFrame* transferred_ = nullptr;
    AVBufferRef* hwDevice_ = nullptr;
    SwsContext* sws_ = nullptr;
    std::vector<std::uint8_t> rgba_;
    bool usingHardware_ = false;
};

class OpusDecoder {
public:
    using PcmCallback = std::function<void(const std::int16_t*, std::size_t)>;

    ~OpusDecoder() {
        close();
    }

    PipelineStatus open(PcmCallback callback) {
        close();
        callback_ = std::move(callback);
        const AVCodec* codec = avcodec_find_decoder(AV_CODEC_ID_OPUS);
        if (!codec) {
            return {false, "FFmpeg Opus decoder is not available."};
        }

        codecCtx_ = avcodec_alloc_context3(codec);
        packet_ = av_packet_alloc();
        decoded_ = av_frame_alloc();
        if (!codecCtx_ || !packet_ || !decoded_) {
            return {false, "FFmpeg Opus allocation failed."};
        }

        codecCtx_->sample_rate = 48000;
        av_channel_layout_default(&codecCtx_->ch_layout, 2);
        const int opened = avcodec_open2(codecCtx_, codec, nullptr);
        if (opened < 0) {
            return {false, "FFmpeg Opus decoder open failed: " + avError(opened)};
        }
        return {true, "Opus decoder opened."};
    }

    void submit(const std::uint8_t* data, std::size_t size) {
        if (!codecCtx_ || !packet_ || !data || size == 0) {
            return;
        }
        av_packet_unref(packet_);
        packet_->data = const_cast<std::uint8_t*>(data);
        packet_->size = static_cast<int>(size);
        if (avcodec_send_packet(codecCtx_, packet_) < 0) {
            packet_->data = nullptr;
            packet_->size = 0;
            return;
        }
        packet_->data = nullptr;
        packet_->size = 0;

        while (true) {
            const int received = avcodec_receive_frame(codecCtx_, decoded_);
            if (received == AVERROR(EAGAIN) || received == AVERROR_EOF) {
                break;
            }
            if (received < 0) {
                break;
            }
            publishPcm(decoded_);
            av_frame_unref(decoded_);
        }
    }

    void close() {
        if (swr_) {
            swr_free(&swr_);
        }
        if (codecCtx_) {
            avcodec_free_context(&codecCtx_);
        }
        if (packet_) {
            av_packet_free(&packet_);
        }
        if (decoded_) {
            av_frame_free(&decoded_);
        }
        pcm_.clear();
        callback_ = nullptr;
    }

private:
    void publishPcm(AVFrame* frame) {
        if (!frame || !callback_) {
            return;
        }
        AVChannelLayout stereo;
        av_channel_layout_default(&stereo, 2);
        if (!swr_) {
            const int configured = swr_alloc_set_opts2(
                &swr_,
                &stereo,
                AV_SAMPLE_FMT_S16,
                48000,
                &frame->ch_layout,
                static_cast<AVSampleFormat>(frame->format),
                frame->sample_rate > 0 ? frame->sample_rate : 48000,
                0,
                nullptr);
            if (configured < 0 || swr_init(swr_) < 0) {
                av_channel_layout_uninit(&stereo);
                return;
            }
        }

        const int sourceRate = frame->sample_rate > 0 ? frame->sample_rate : 48000;
        const int capacity = static_cast<int>(av_rescale_rnd(swr_get_delay(swr_, sourceRate) + frame->nb_samples, 48000, sourceRate, AV_ROUND_UP));
        if (capacity <= 0) {
            av_channel_layout_uninit(&stereo);
            return;
        }
        pcm_.resize(static_cast<std::size_t>(capacity) * 2);
        std::uint8_t* out[] = {reinterpret_cast<std::uint8_t*>(pcm_.data())};
        const int converted = swr_convert(swr_, out, capacity, const_cast<const std::uint8_t**>(frame->extended_data), frame->nb_samples);
        if (converted > 0) {
            callback_(pcm_.data(), static_cast<std::size_t>(converted) * 2);
        }
        av_channel_layout_uninit(&stereo);
    }

    AVCodecContext* codecCtx_ = nullptr;
    AVPacket* packet_ = nullptr;
    AVFrame* decoded_ = nullptr;
    SwrContext* swr_ = nullptr;
    std::vector<std::int16_t> pcm_;
    PcmCallback callback_;
};
#endif

class SwitchAudioSink {
public:
    ~SwitchAudioSink() {
        close();
    }

    PipelineStatus open() {
#if defined(OPENNOW_PLATFORM_SWITCH) && defined(OPENNOW_HAS_NATIVE_AUDIO)
        close();
        Result rc = audoutInitialize();
        if (R_FAILED(rc)) {
            return {false, "audoutInitialize failed: 0x" + hexResult(rc)};
        }
        initialized_ = true;

        const u32 rate = audoutGetSampleRate();
        const u32 channels = audoutGetChannelCount();
        const PcmFormat format = audoutGetPcmFormat();
        if (rate != 48000 || channels != 2 || format != PcmFormat_Int16) {
            close();
            return {false, "audout returned unsupported audio format."};
        }
        buffers_.resize(kBufferCount);
        free_.assign(kBufferCount, true);
        queued_ = 0;
        failed_ = false;
        for (std::size_t i = 0; i < buffers_.size(); ++i) {
            buffers_[i].storage = static_cast<std::uint8_t*>(memalign(0x1000, kBufferBytes));
            if (!buffers_[i].storage) {
                close();
                return {false, "audio output buffer allocation failed."};
            }
            std::memset(buffers_[i].storage, 0, kBufferBytes);
            buffers_[i].buffer.next = nullptr;
            buffers_[i].buffer.buffer = buffers_[i].storage;
            buffers_[i].buffer.buffer_size = kBufferBytes;
            buffers_[i].buffer.data_size = 0;
            buffers_[i].buffer.data_offset = 0;
        }
        return {true, "Switch audout opened at 48 kHz stereo S16 with jitter prebuffer."};
#else
        return {true, "Audio sink disabled on this platform."};
#endif
    }

    void submit(const std::int16_t* samples, std::size_t sampleCount) {
#if defined(OPENNOW_PLATFORM_SWITCH) && defined(OPENNOW_HAS_NATIVE_AUDIO)
        if (!initialized_ || failed_ || !samples || sampleCount == 0) {
            return;
        }
        reclaim();
        const auto* bytes = reinterpret_cast<const std::uint8_t*>(samples);
        std::size_t remaining = sampleCount * sizeof(std::int16_t);
        while (remaining > 0) {
            const auto index = acquireFreeBuffer();
            if (index >= buffers_.size()) {
                return;
            }
            const std::size_t chunk = std::min<std::size_t>(remaining, kBufferBytes);
            std::memcpy(buffers_[index].storage, bytes, chunk);
            armDCacheFlush(buffers_[index].storage, chunk);
            buffers_[index].buffer.data_size = chunk;
            const Result rc = audoutAppendAudioOutBuffer(&buffers_[index].buffer);
            Result appendRc = rc;
            if (R_FAILED(appendRc) && !started_) {
                const Result startRc = audoutStartAudioOut();
                if (R_SUCCEEDED(startRc)) {
                    started_ = true;
                    appendRc = audoutAppendAudioOutBuffer(&buffers_[index].buffer);
                }
            }
            if (R_FAILED(appendRc)) {
                free_[index] = true;
                failed_ = true;
                return;
            }
            free_[index] = false;
            ++queued_;
            ensureStarted();
            bytes += chunk;
            remaining -= chunk;
        }
#else
        (void)samples;
        (void)sampleCount;
#endif
    }

    void close() {
#if defined(OPENNOW_PLATFORM_SWITCH) && defined(OPENNOW_HAS_NATIVE_AUDIO)
        if (initialized_ && started_) {
            bool flushed = false;
            (void)audoutFlushAudioOutBuffers(&flushed);
            (void)audoutStopAudioOut();
            started_ = false;
        }
        for (auto& slot : buffers_) {
            std::free(slot.storage);
            slot.storage = nullptr;
        }
        buffers_.clear();
        free_.clear();
        if (initialized_) {
            audoutExit();
            initialized_ = false;
        }
        queued_ = 0;
        failed_ = false;
#endif
    }

private:
#if defined(OPENNOW_PLATFORM_SWITCH) && defined(OPENNOW_HAS_NATIVE_AUDIO)
    struct Slot {
        AudioOutBuffer buffer{};
        std::uint8_t* storage = nullptr;
    };

    static constexpr std::size_t kBufferCount = 24;
    static constexpr std::size_t kBufferBytes = 0x4000;
    static constexpr std::size_t kStartBufferCount = 4;

    static std::string hexResult(Result result) {
        std::ostringstream out;
        out << std::hex << static_cast<std::uint32_t>(result);
        return out.str();
    }

    void reclaim() {
        AudioOutBuffer* released = nullptr;
        u32 releasedCount = 0;
        while (R_SUCCEEDED(audoutGetReleasedAudioOutBuffer(&released, &releasedCount)) && released && releasedCount > 0) {
            for (std::size_t i = 0; i < buffers_.size(); ++i) {
                if (&buffers_[i].buffer == released) {
                    if (!free_[i] && queued_ > 0) {
                        --queued_;
                    }
                    free_[i] = true;
                    break;
                }
            }
            released = nullptr;
            releasedCount = 0;
        }
    }

    void ensureStarted() {
        if (started_ || queued_ < kStartBufferCount) {
            return;
        }
        const Result rc = audoutStartAudioOut();
        if (R_FAILED(rc)) {
            failed_ = true;
            return;
        }
        started_ = true;
    }

    std::size_t acquireFreeBuffer() {
        reclaim();
        for (std::size_t i = 0; i < free_.size(); ++i) {
            if (free_[i]) {
                return i;
            }
        }
        return buffers_.size();
    }

    bool initialized_ = false;
    bool started_ = false;
    bool failed_ = false;
    std::size_t queued_ = 0;
    std::vector<Slot> buffers_;
    std::vector<bool> free_;
#endif
};

} // namespace

bool NativeMediaCapabilities::ready() const {
    return wssSignaling && sdpNegotiation && ice && dtlsSrtp && rtp && h264Decode && videoRender && audio;
}

bool NativeMediaCapabilities::transportReady() const {
    return wssSignaling && sdpNegotiation && ice && dtlsSrtp && rtp;
}

NativeMediaCapabilities nativeMediaCapabilities() {
    NativeMediaCapabilities caps;
#if defined(OPENNOW_HAS_NATIVE_WSS_SIGNALING)
    caps.wssSignaling = true;
#elif defined(OPENNOW_HAS_MBEDTLS_TLS)
    caps.wssSignaling = true;
#endif
#if defined(OPENNOW_HAS_NATIVE_ICE)
    caps.ice = true;
#endif
#if defined(OPENNOW_HAS_NATIVE_DTLS_SRTP)
    caps.dtlsSrtp = true;
#endif
#if defined(OPENNOW_HAS_NATIVE_RTP)
    caps.rtp = true;
#endif
#if defined(OPENNOW_HAS_FFMPEG_DECODER)
    caps.h264Decode = true;
#endif
#if defined(OPENNOW_HAS_NATIVE_VIDEO_RENDERER)
    caps.videoRender = true;
#endif
#if defined(OPENNOW_HAS_NATIVE_AUDIO)
    caps.audio = true;
#endif

    if (caps.ready()) {
        caps.message = "Native GFN media stack is linked.";
    } else if (caps.transportReady()) {
        caps.message =
            "Native WebRTC signaling/ICE/DTLS-SRTP/RTP transport is linked. Remaining blockers are Switch H264 decode, video render, and audio output sinks.";
    } else {
#if defined(OPENNOW_HAS_LIBPEER)
        caps.message =
            "Native WebRTC transport is partially linked with libpeer/usrsctp/libsrtp/mbedTLS, but the full signaling/ICE/DTLS-SRTP/RTP boundary is not available.";
#else
        caps.message =
            "Native media stack is not linked: missing WSS signaling, ICE, DTLS-SRTP, RTP receive, H264 decode, video render, or audio output.";
#endif
    }
    return caps;
}

class NativeMediaPipeline::Impl {
public:
    ~Impl() {
        close();
    }

    PipelineStatus open(const StreamSettings& settings, const SessionInfo& session, net::HttpClient* http, std::function<void(const std::string&)> logCallback) {
        close();
        clearVideoFrames();
        log = std::move(logCallback);
        if (!log) {
            log = defaultLog;
        }

        const auto caps = nativeMediaCapabilities();
        if (!caps.ready()) {
            return {false, caps.message};
        }
        if (session.sessionId.empty()) {
            return {false, "Native media pipeline cannot open without a CloudMatch session id."};
        }
        if (session.signalingUrl.empty() || session.serverIp.empty()) {
            return {false, "CloudMatch is not ready yet: missing signaling server coordinates."};
        }
        if (session.mediaConnectionInfo.ip.empty() || session.mediaConnectionInfo.port == 0) {
            return {false, "CloudMatch is ready, but media RTP coordinates are missing."};
        }

#if defined(OPENNOW_HAS_FFMPEG_DECODER)
        auto videoStatus = video.open(true);
        if (!videoStatus.ready) {
            return videoStatus;
        }
        log("[INFO][MEDIA] " + videoStatus.message);

        auto audioStatus = audio.open();
        if (!audioStatus.ready) {
            log("[WARN][AUDIO] " + audioStatus.message + " Continuing with video-only output.");
        } else {
            log("[INFO][AUDIO] " + audioStatus.message);
        }

        auto opusStatus = opus.open([this](const std::int16_t* samples, std::size_t sampleCount) {
            audio.submit(samples, sampleCount);
        });
        if (!opusStatus.ready) {
            return opusStatus;
        }
        log("[INFO][AUDIO] " + opusStatus.message);
#else
        (void)settings;
        (void)http;
        return {false, "OpenNOW was built without FFmpeg decode support."};
#endif

        std::function<void(const std::uint8_t*, std::size_t)> videoCallback;
        std::function<void(const std::uint8_t*, std::size_t)> audioCallback;
#if defined(OPENNOW_HAS_FFMPEG_DECODER)
        videoCallback = [this](const std::uint8_t* bytes, std::size_t size) {
            std::lock_guard<std::mutex> lock(videoDecodeMutex);
            video.submit(bytes, size);
        };
        audioCallback = [this](const std::uint8_t* bytes, std::size_t size) {
            std::lock_guard<std::mutex> lock(audioDecodeMutex);
            opus.submit(bytes, size);
        };
#endif

        auto signalingResult = webrtc.connectWithGfnSignaling({
            .session = session,
            .settings = settings,
            .http = http,
            .log = log,
            .videoNal = std::move(videoCallback),
            .opusPacket = std::move(audioCallback),
            .timeoutMs = 45000,
        });
        if (!signalingResult.ok) {
            close();
            return {false, "GFN WebRTC signaling failed: " + signalingResult.error};
        }

        stop.store(false);
        pumpThread = std::thread([this]() {
            std::string lastState;
            auto nextStats = std::chrono::steady_clock::now() + std::chrono::seconds(5);
            while (!stop.load()) {
                webrtc.pumpOnce();
                const auto state = webrtc.connectionState();
                if (state != lastState) {
                    lastState = state;
                    if (log) {
                        log("[INFO][MEDIA] WebRTC state: " + state + ".");
                    }
                }
                if (webrtc.mediaConnected()) {
                    (void)webrtc.openInputChannels(16);
                }
                if (std::chrono::steady_clock::now() >= nextStats) {
                    if (log) {
                        log("[INFO][MEDIA] RTP received video=" + std::to_string(webrtc.videoBytesReceived())
                            + " bytes audio=" + std::to_string(webrtc.audioBytesReceived()) + " bytes.");
                    }
                    nextStats = std::chrono::steady_clock::now() + std::chrono::seconds(5);
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
        });

        std::ostringstream message;
        message
            << "Native GFN WebRTC media is active for " << describe(settings)
            << ". Sent answer to peer " << signalingResult.remotePeerId
            << "; decoding H264 video frames and Opus audio packets.";
        return {true, message.str()};
    }

    PipelineStatus close() {
        stop.store(true);
        if (pumpThread.joinable()) {
            pumpThread.join();
        }
        webrtc.close();
#if defined(OPENNOW_HAS_FFMPEG_DECODER)
        opus.close();
        video.close();
#endif
        audio.close();
        return {true, "Native media pipeline closed."};
    }

    std::string connectionState() const {
        return webrtc.connectionState();
    }

    bool mediaConnected() const {
        return webrtc.mediaConnected();
    }

    bool inputReady() const {
        return webrtc.inputReady();
    }

    bool sendGamepadInput(const gfn::GamepadInput& input, std::uint16_t connectedBitmap) {
        return webrtc.sendGamepadInput(input, connectedBitmap);
    }

    bool sendMouseMove(const gfn::MouseMovePayload& input) {
        return webrtc.sendMouseMove(input);
    }

    bool sendMouseButton(const gfn::MouseButtonPayload& input, bool pressed) {
        return webrtc.sendMouseButton(input, pressed);
    }

    std::size_t videoBytesReceived() const {
        return webrtc.videoBytesReceived();
    }

    std::size_t audioBytesReceived() const {
        return webrtc.audioBytesReceived();
    }

    GfnWebRtcClient webrtc;
    std::atomic_bool stop{false};
    std::thread pumpThread;
    std::function<void(const std::string&)> log;
#if defined(OPENNOW_HAS_FFMPEG_DECODER)
    H264FrameDecoder video;
    OpusDecoder opus;
    std::mutex videoDecodeMutex;
    std::mutex audioDecodeMutex;
#endif
    SwitchAudioSink audio;
};

NativeMediaPipeline::NativeMediaPipeline() : impl_(std::make_unique<Impl>()) {}

NativeMediaPipeline::~NativeMediaPipeline() = default;

PipelineStatus NativeMediaPipeline::open(
    const StreamSettings& settings,
    const SessionInfo& session,
    net::HttpClient* http,
    std::function<void(const std::string&)> log) {
    return impl_->open(settings, session, http, std::move(log));
}

PipelineStatus NativeMediaPipeline::close() {
    return impl_->close();
}

std::string NativeMediaPipeline::connectionState() const {
    return impl_->connectionState();
}

bool NativeMediaPipeline::mediaConnected() const {
    return impl_->mediaConnected();
}

bool NativeMediaPipeline::inputReady() const {
    return impl_->inputReady();
}

bool NativeMediaPipeline::sendGamepadInput(const gfn::GamepadInput& input, std::uint16_t connectedBitmap) {
    return impl_->sendGamepadInput(input, connectedBitmap);
}

bool NativeMediaPipeline::sendMouseMove(const gfn::MouseMovePayload& input) {
    return impl_->sendMouseMove(input);
}

bool NativeMediaPipeline::sendMouseButton(const gfn::MouseButtonPayload& input, bool pressed) {
    return impl_->sendMouseButton(input, pressed);
}

std::size_t NativeMediaPipeline::videoBytesReceived() const {
    return impl_->videoBytesReceived();
}

std::size_t NativeMediaPipeline::audioBytesReceived() const {
    return impl_->audioBytesReceived();
}

} // namespace opennow::media
