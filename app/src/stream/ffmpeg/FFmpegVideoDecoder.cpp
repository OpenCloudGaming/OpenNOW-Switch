#include "FFmpegVideoDecoder.hpp"
#include "AVFrameHolder.hpp"
#include "borealis.hpp"
#include "../../stream_settings.hpp"
#include "../../video_quality_policy.hpp"
#ifdef PLATFORM_APPLE
extern "C" {
#include <libavcodec/videotoolbox.h>
}
#endif

// Disables the deblocking filter at the cost of image quality
#define DISABLE_LOOP_FILTER 0x1
// Uses the low latency decode flag (disables multithreading)
#define LOW_LATENCY_DECODE 0x2

//#if defined(PLATFORM_TVOS)
//#define DECODER_BUFFER_SIZE 92 * 1024 * 4
//#else
//#define DECODER_BUFFER_SIZE 92 * 1024 * 2
//#endif
#define DECODER_BUFFER_SIZE (1024 * 1024)

#if defined(PLATFORM_ANDROID)
#include <jni.h>
#include <libavcodec/jni.h>
#include <libavutil/hwcontext_mediacodec.h>

//static JavaVM *mJavaVM = NULL;
//JNIEXPORT jint JNICALL JNI_OnLoad(JavaVM *vm, void *reserved)
//{
////    av_jni_set_java_vm(vm, NULL);
//    return JNI_VERSION_1_4;
//}
#endif

FFmpegVideoDecoder::FFmpegVideoDecoder() {
//    AVBufferRef* deviceRef = av_hwdevice_ctx_alloc(AV_HWDEVICE_TYPE_MEDIACODEC);
//    AVHWDeviceContext* ctx = (AVHWDeviceContext*)deviceRef->data;
//    AVMediaCodecDeviceContext* hwctx = (AVMediaCodecDeviceContext*)ctx->hwctx;
////    hwctx->surface = ;
//    av_hwdevice_ctx_init(deviceRef);
}

FFmpegVideoDecoder::~FFmpegVideoDecoder() = default;

void ffmpegLog(void* ptr, int level, const char* fmt, va_list vargs) {
    std::string message;
    va_list ap_copy;
    va_copy(ap_copy, vargs);
    size_t len = vsnprintf(nullptr, 0, fmt, ap_copy);
    message.resize(len + 1);  // need space for NUL
    vsnprintf(&message[0], len + 1,fmt, vargs);
    message.resize(len);  // remove the NUL
    brls::Logger::debug("FFmpeg [LOG]: {}", message.c_str());
}

int FFmpegVideoDecoder::setup(int video_format, int width, int height,
                              int redraw_rate, void* context, int dr_flags) {
    m_stream_fps = redraw_rate;
    m_uses_hardware_frames =
        (dr_flags & VIDEO_DECODER_PREFER_HARDWARE) != 0 &&
        (dr_flags & VIDEO_DECODER_FORCE_SOFTWARE) == 0;

    std::string format;
    switch (video_format) {
        case VIDEO_FORMAT_H264:
            format = "H264";
            break;
        case VIDEO_FORMAT_H265:
            format = "HEVC";
            break;
        case VIDEO_FORMAT_H265_MAIN10:
            format = "HEVC HDR";
            break;
        case VIDEO_FORMAT_AV1_MAIN8:
            format = "AV1";
            break;
        case VIDEO_FORMAT_AV1_MAIN10:
            format = "AV1 HDR";
            break;
        default:
            format = "UNKNOWN";
            break;
    }
    brls::Logger::debug("FFMpeg's AVCodec version: {}.{}.{}", AV_VERSION_MAJOR(avcodec_version()), AV_VERSION_MINOR(avcodec_version()), AV_VERSION_MICRO(avcodec_version()));
    brls::Logger::info(
        "FFmpeg: Setup with format: {}, width: {}, height: {}, fps: {}", format, width, height, redraw_rate);

    av_log_set_level(AV_LOG_WARNING);
    // av_log_set_callback(&ffmpegLog); // Uncomment to see FFMpeg logs
#if LIBAVCODEC_VERSION_INT < AV_VERSION_INT(58, 10, 100)
    avcodec_register_all();
#endif

    m_packet = av_packet_alloc();

    int perf_lvl = LOW_LATENCY_DECODE;
#if defined(PLATFORM_SWITCH)
    // H.264 deblocking is one of the largest software decode costs on Tegra X1.
    // The cloud stream is already filtered by the encoder, so trading a small
    // amount of edge quality for stable frame time is preferable to frame loss.
    if (!m_uses_hardware_frames)
        perf_lvl |= DISABLE_LOOP_FILTER;
#endif

#ifdef PLATFORM_ANDROID
    if (video_format & VIDEO_FORMAT_MASK_H264) {
        m_decoder = avcodec_find_decoder_by_name("h264_mediacodec");
    } else if (video_format & VIDEO_FORMAT_MASK_H265) {
        m_decoder = avcodec_find_decoder_by_name("hevc_mediacodec");
    } else {
        // Unsupported decoder type
    }
#else
    if (video_format & VIDEO_FORMAT_MASK_H264) {
#if defined(OPENNOW_ENABLE_NVDEC)
        m_decoder = m_uses_hardware_frames
            ? avcodec_find_decoder_by_name("h264_nvtegra")
            : avcodec_find_decoder(AV_CODEC_ID_H264);
#else
        m_uses_hardware_frames = false;
        m_decoder = avcodec_find_decoder(AV_CODEC_ID_H264);
#endif
    } else if (video_format & VIDEO_FORMAT_MASK_H265) {
        m_decoder = avcodec_find_decoder(AV_CODEC_ID_HEVC);
    } else {
        // Unsupported decoder type
    }
#endif

    if (m_decoder == nullptr) {
        brls::Logger::error("FFmpeg: Couldn't find decoder");
        return -1;
    }

    m_decoder_context = avcodec_alloc_context3(m_decoder);
    if (m_decoder_context == nullptr) {
        brls::Logger::error("FFmpeg: Couldn't allocate context");
        return -1;
    }

    if (perf_lvl & DISABLE_LOOP_FILTER) {
        const auto settings = opennow::LoadStreamSettings();
        const auto tuning = opennow::video::ResolveQualityTuning(settings.image_quality_mode);
        // Keep deblocking on reference pictures in enhanced modes. This removes
        // persistent block edges while still skipping work on disposable frames.
        m_decoder_context->skip_loop_filter = tuning.preserve_reference_deblocking
            ? AVDISCARD_NONREF : AVDISCARD_ALL;
    }

    if (perf_lvl & LOW_LATENCY_DECODE)
        // Use low delay single threaded encoding
        m_decoder_context->flags |= AV_CODEC_FLAG_LOW_DELAY;

    // Do not present corrupt or pre-keyframe pictures. RTP recovery now keeps
    // complete access units, so these permissive flags only expose reference
    // damage as visible macroblocks instead of holding the previous good frame.
    m_decoder_context->flags2 |= AV_CODEC_FLAG2_FAST;

    // NVDEC submits asynchronously and does not benefit from CPU frame
    // threads. The software fallback keeps one Tegra X1 core for UI/network.
    const int decoder_threads = m_uses_hardware_frames ? 1 : 3;
    m_decoder_context->thread_type = FF_THREAD_FRAME;
    m_decoder_context->thread_count = decoder_threads;

    m_decoder_context->width = width;
    m_decoder_context->height = height;

#if defined(PLATFORM_SWITCH) && defined(BOREALIS_USE_DEKO3D)
        AVHWDeviceType hwType = m_uses_hardware_frames
            ? AV_HWDEVICE_TYPE_NVTEGRA : AV_HWDEVICE_TYPE_NONE;
#elif defined(PLATFORM_ANDROID)
        AVHWDeviceType hwType = AV_HWDEVICE_TYPE_MEDIACODEC;
#elif defined(PLATFORM_APPLE)
        AVHWDeviceType hwType = AV_HWDEVICE_TYPE_VIDEOTOOLBOX;
#else
        AVHWDeviceType hwType = AV_HWDEVICE_TYPE_NONE;
#endif

    int err = 0;
    if (hwType != AV_HWDEVICE_TYPE_NONE) {
        if ((err = av_hwdevice_ctx_create(&hw_device_ctx, hwType, nullptr, nullptr, 0)) < 0) {
            char error[512];
            av_strerror(err, error, sizeof(error));
            brls::Logger::error("FFmpeg: Error initializing hardware decoder - {}", error);
            return -1;
        }
        m_decoder_context->hw_device_ctx = av_buffer_ref(hw_device_ctx);
    } else {
        brls::Logger::info("FFmpeg: using software frames for OpenGL renderer");
    }

    if (m_uses_hardware_frames)
        m_decoder_context->pix_fmt = AV_PIX_FMT_NVTEGRA;

    err = avcodec_open2(m_decoder_context, m_decoder, nullptr);
    if (err < 0) {
        char error[512];
        av_strerror(err, error, sizeof(error));
        brls::Logger::error("FFmpeg: Couldn't open codec - {}", error);
        return err;
    }

    AVFrameHolder::instance().prepare(m_uses_hardware_frames ? 3 : 4);

    // One extra frame for decoder output while the frame holder owns deep copies.
    m_frames_size = 5 + 1;
    m_frames = new AVFrame*[m_frames_size];

    tmp_frame = av_frame_alloc();
    if (!tmp_frame) {
        brls::Logger::error("FFmpeg: Couldn't allocate temp frame");
        return -1;
    }

    for (int i = 0; i < m_frames_size; i++) {
        auto& frame = m_frames[i];
        frame = av_frame_alloc();
        if (frame == nullptr) {
            brls::Logger::error("FFmpeg: Couldn't allocate frame");
            return -1;
        }
    }

    m_ffmpeg_buffer =
        (char*)malloc(DECODER_BUFFER_SIZE + AV_INPUT_BUFFER_PADDING_SIZE);
    if (m_ffmpeg_buffer == nullptr) {
        brls::Logger::error("FFmpeg: Not enough memory");
        cleanup();
        return -1;
    }

    brls::Logger::info("FFmpeg: Setup done with {} frame threads", decoder_threads);
    return 0;
}

void FFmpegVideoDecoder::cleanup() {
    brls::Logger::info("FFmpeg: Cleanup...");

    av_packet_free(&m_packet);

    if (hw_device_ctx) {
        av_buffer_unref(&hw_device_ctx);
    }

    if (m_decoder_context) {
        avcodec_free_context(&m_decoder_context);
    }

    if (m_frames) {
        for (int i = 0; i < m_frames_size; i++) {
            av_frame_free(&m_frames[i]);
        }

        delete[] m_frames;
        m_frames = nullptr;
        m_frames_size = 0;
    }

    if (tmp_frame) {
        av_frame_free(&tmp_frame);
    }

    if (m_ffmpeg_buffer) {
        free(m_ffmpeg_buffer);
        m_ffmpeg_buffer = nullptr;
    }

    AVFrameHolder::instance().cleanup();

    brls::Logger::info("FFmpeg: Cleanup done!");
}

int FFmpegVideoDecoder::submit_decode_unit(uint8_t* indata, int inlen, int64_t pts) {
    if (!indata || inlen <= 0)
        return 0;

    if (inlen > DECODER_BUFFER_SIZE) {
        brls::Logger::error("FFmpeg: Big buffer to decode...");
        return -1;
    }

    m_frames_in++;

    int decoded_frames = 0;
    auto drain_frames = [&]() {
        while ((m_frame = get_frame(true)) != nullptr) {
            if ((m_frame->flags & AV_FRAME_FLAG_CORRUPT) != 0 ||
                m_frame->decode_error_flags != 0) {
                m_corrupt_frames_dropped++;
                if (m_corrupt_frames_dropped <= 5 || m_corrupt_frames_dropped % 60 == 0) {
                    brls::Logger::warning(
                        "FFmpeg: dropping corrupt frame flags={} decodeErrors={} total={}",
                        m_frame->flags, m_frame->decode_error_flags,
                        m_corrupt_frames_dropped);
                }
                av_frame_unref(m_frame);
                continue;
            }
            m_frames_out++;
            decoded_frames++;
            AVFrameHolder::instance().push(m_frame);
        }
    };

    int decode_result = decode(reinterpret_cast<char*>(indata), inlen, pts);
    if (decode_result == AVERROR(EAGAIN)) {
        drain_frames();
        decode_result = decode(reinterpret_cast<char*>(indata), inlen, pts);
    }

    if (decode_result < 0)
        return decode_result;

    drain_frames();

    return decoded_frames;
}

void FFmpegVideoDecoder::reset_stream() {
    if (m_decoder_context)
        avcodec_flush_buffers(m_decoder_context);
}

int FFmpegVideoDecoder::capabilities() const {
    return 0;
}

int FFmpegVideoDecoder::decode(char* indata, int inlen, int64_t pts) {
    m_packet->data = (uint8_t*)indata;
    m_packet->size = inlen;
    m_packet->pts = pts;
    m_packet->dts = pts;

#if !defined(PLATFORM_SWITCH)
    int policy;
    sched_param params{};
    pthread_getschedparam(pthread_self(), &policy, &params);
    params.sched_priority = sched_get_priority_max(policy);
    pthread_setschedparam(pthread_self(), policy, &params);
#endif

//    m_decoder_context->skip_frame = AVDISCARD_ALL;

    int err = avcodec_send_packet(m_decoder_context, m_packet);
    if (err == AVERROR(EAGAIN)) {
        brls::Logger::debug("FFmpeg: decoder wants frame drain before accepting more data");
        return err;
    }

    if (err != 0) {
        char error[512];
        av_strerror(err, error, sizeof(error));
        brls::Logger::error("FFmpeg: Decode failed - {}", error);
        return err;
    }

    return 0;
}

AVFrame* FFmpegVideoDecoder::get_frame(bool native_frame) {
    if (!m_decoder_context || !m_frames || m_frames_size <= 0)
        return nullptr;

    int err;
    AVFrame* resultFrame = m_frames[m_next_frame];
    AVFrame* decodeFrame = resultFrame;
    av_frame_unref(resultFrame);

#if !defined(PLATFORM_ANDROID) && !defined(BOREALIS_USE_DEKO3D) && !defined(USE_METAL_RENDERER)
    if (hw_device_ctx) {
        // For HW->SW transfer path we decode into a temporary hardware frame.
        av_frame_unref(tmp_frame);
        decodeFrame = tmp_frame;
    }
#endif

    if ((err = avcodec_receive_frame(m_decoder_context, decodeFrame)) < 0) {
        if (err == AVERROR(EAGAIN)) {
            return nullptr;
        }

        if (err == AVERROR_EOF) {
            return nullptr;
        }

        char a[AV_ERROR_MAX_STRING_SIZE] = { 0 };
        brls::Logger::error("FFmpeg: Error receiving frame with error {}",  av_make_error_string(a, AV_ERROR_MAX_STRING_SIZE, err));
        return nullptr;
    }

    if (hw_device_ctx) {
#if defined(BOREALIS_USE_DEKO3D) || defined(PLATFORM_ANDROID) || defined(USE_METAL_RENDERER)
        // Keep hardware-backed frame references per queue slot.
        resultFrame = decodeFrame;
#else

#if defined(PLATFORM_SWITCH) && !defined(BOREALIS_USE_DEKO3D)
        for (int i = 0; i < 2; ++i) {
            if (((uintptr_t)resultFrame->data[i] & 0xff) || (resultFrame->linesize[i] & 0xff)) {
                brls::Logger::error("Frame address/pitch not aligned to 256, falling back to cpu transfer");
                break;
            }
        }
#endif

        // Copy hardware frame into software frame
        av_frame_unref(resultFrame);
        if ((err = av_hwframe_transfer_data(resultFrame, decodeFrame, 0)) < 0) {
            char a[AV_ERROR_MAX_STRING_SIZE] = { 0 };
            brls::Logger::error("FFmpeg: Error transferring the data to system memory with error {}",  av_make_error_string(a, AV_ERROR_MAX_STRING_SIZE, err));
            return nullptr;
        }

        av_frame_copy_props(resultFrame, decodeFrame);
#endif
    } else {
        resultFrame = decodeFrame;
    }

    m_current_frame = m_next_frame;
    m_next_frame = (m_current_frame + 1) % m_frames_size;
    if (native_frame)
        return resultFrame;

    return nullptr;
}

VideoDecodeStats* FFmpegVideoDecoder::video_decode_stats() {
    return (VideoDecodeStats*)&m_video_decode_stats_cache;
}
