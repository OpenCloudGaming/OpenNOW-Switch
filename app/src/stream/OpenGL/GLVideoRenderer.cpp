#ifdef USE_GL_RENDERER

#include "GLVideoRenderer.hpp"
#include "../../stream_diagnostics.hpp"

// TODO: rework logging with callbacks
#ifndef _WIN32
#include "borealis.hpp"
#endif

#include <algorithm>
#include <chrono>
#include <cstdarg>
#include <cstdint>
#include <cstdio>

static inline uint64_t LiGetMillis() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()
    ).count();
}

#include "GLShaders.hpp"

#ifdef __SWITCH__
#include <sys/stat.h>
#endif

static void render_diag_log(const char* fmt, ...) {
    if (!opennow::StreamDiagnosticsEnabled())
        return;
    FILE* file = fopen("sdmc:/switch/SwitchNOW/signaling.log", "a");
    FILE* trace = fopen("sdmc:/switch/SwitchNOW/stream_trace.log", "a");

    va_list args;
    va_start(args, fmt);
    if (file) {
        fputs("RENDER ", file);
        va_list copy;
        va_copy(copy, args);
        vfprintf(file, fmt, copy);
        va_end(copy);
        fputc('\n', file);
        fclose(file);
    }
    if (trace) {
        fputs("RENDER ", trace);
        vfprintf(trace, fmt, args);
        fputc('\n', trace);
        fclose(trace);
    }
    va_end(args);
}

static const char* gl_string_or_unknown(GLenum name) {
    const GLubyte* value = glGetString(name);
    return value ? reinterpret_cast<const char*>(value) : "unknown";
}

static GLenum gl_drain_error() {
    GLenum last = GL_NO_ERROR;
    for (;;) {
        GLenum err = glGetError();
        if (err == GL_NO_ERROR)
            return last;
        last = err;
    }
}

#if defined(OPENNOW_ENABLE_FRAME_PROBE)
static uint8_t clamp_u8(int value) {
    return static_cast<uint8_t>(std::max(0, std::min(255, value)));
}

static int read_plane8(const AVFrame* frame, int plane, int x, int y) {
    if (!frame || !frame->data[plane])
        return 128;
    return frame->data[plane][y * frame->linesize[plane] + x];
}

static int read_plane16_as8(const AVFrame* frame, int plane, int x, int y, int component = 0) {
    if (!frame || !frame->data[plane])
        return 128;
    const uint8_t* row = frame->data[plane] + y * frame->linesize[plane];
    const uint16_t* pixel = reinterpret_cast<const uint16_t*>(row) + x + component;
    return static_cast<int>((*pixel) >> 8);
}

static bool sample_yuv8(const AVFrame* frame, int sx, int sy, int& y, int& u, int& v) {
    if (!frame || !frame->data[0])
        return false;

    switch (frame->format) {
    case AV_PIX_FMT_YUV420P:
    case AV_PIX_FMT_YUVJ420P:
        y = read_plane8(frame, 0, sx, sy);
        u = read_plane8(frame, 1, sx / 2, sy / 2);
        v = read_plane8(frame, 2, sx / 2, sy / 2);
        return true;
    case AV_PIX_FMT_NV12: {
        y = read_plane8(frame, 0, sx, sy);
        if (!frame->data[1]) {
            u = 128;
            v = 128;
            return true;
        }
        const uint8_t* uv = frame->data[1] + (sy / 2) * frame->linesize[1] + (sx / 2) * 2;
        u = uv[0];
        v = uv[1];
        return true;
    }
    case AV_PIX_FMT_P010: {
        y = read_plane16_as8(frame, 0, sx, sy);
        if (!frame->data[1]) {
            u = 128;
            v = 128;
            return true;
        }
        const uint8_t* row = frame->data[1] + (sy / 2) * frame->linesize[1];
        const uint16_t* uv = reinterpret_cast<const uint16_t*>(row) + (sx / 2) * 2;
        u = static_cast<int>(uv[0] >> 8);
        v = static_cast<int>(uv[1] >> 8);
        return true;
    }
    default:
        return false;
    }
}

static void yuv_to_rgb(const AVFrame* frame, int y, int u, int v, uint8_t& r, uint8_t& g, uint8_t& b) {
    const bool full_range = frame && frame->color_range == AVCOL_RANGE_JPEG;
    if (full_range) {
        const int d = u - 128;
        const int e = v - 128;
        r = clamp_u8(y + ((359 * e) >> 8));
        g = clamp_u8(y - ((88 * d + 183 * e) >> 8));
        b = clamp_u8(y + ((454 * d) >> 8));
        return;
    }

    const int c = std::max(0, y - 16);
    const int d = u - 128;
    const int e = v - 128;
    r = clamp_u8((298 * c + 409 * e + 128) >> 8);
    g = clamp_u8((298 * c - 100 * d - 208 * e + 128) >> 8);
    b = clamp_u8((298 * c + 516 * d + 128) >> 8);
}

static void dump_first_frame_probe(const AVFrame* frame) {
    if (!opennow::StreamDiagnosticsEnabled())
        return;
    if (!frame || frame->width <= 0 || frame->height <= 0 || !frame->data[0])
        return;

#ifdef __SWITCH__
    mkdir("sdmc:/switch", 0777);
    mkdir("sdmc:/switch/SwitchNOW", 0777);
#endif

    const char* path = "sdmc:/switch/SwitchNOW/first_frame.ppm";
    FILE* out = fopen(path, "wb");
    if (!out) {
        render_diag_log("frame_probe failed_open path=%s", path);
        return;
    }

    const int out_w = std::min(320, frame->width);
    const int out_h = std::max(1, std::min(180, frame->height * out_w / std::max(1, frame->width)));
    fprintf(out, "P6\n%d %d\n255\n", out_w, out_h);

    int y_min = 255;
    int y_max = 0;
    long long y_sum = 0;
    int samples = 0;
    int center_y = 0;
    int center_u = 0;
    int center_v = 0;

    for (int oy = 0; oy < out_h; ++oy) {
        const int sy = std::min(frame->height - 1, oy * frame->height / out_h);
        for (int ox = 0; ox < out_w; ++ox) {
            const int sx = std::min(frame->width - 1, ox * frame->width / out_w);
            int y = 0;
            int u = 128;
            int v = 128;
            if (!sample_yuv8(frame, sx, sy, y, u, v)) {
                fclose(out);
                render_diag_log("frame_probe unsupported_format format=%d", frame->format);
                return;
            }

            y_min = std::min(y_min, y);
            y_max = std::max(y_max, y);
            y_sum += y;
            samples++;

            if (ox == out_w / 2 && oy == out_h / 2) {
                center_y = y;
                center_u = u;
                center_v = v;
            }

            uint8_t rgb[3];
            yuv_to_rgb(frame, y, u, v, rgb[0], rgb[1], rgb[2]);
            fwrite(rgb, 1, sizeof(rgb), out);
        }
    }

    fclose(out);
    render_diag_log("frame_probe path=%s size=%dx%d yMin=%d yMax=%d yAvg=%lld centerYUV=%d/%d/%d format=%d",
                    path,
                    out_w,
                    out_h,
                    y_min,
                    y_max,
                    samples ? y_sum / samples : 0,
                    center_y,
                    center_u,
                    center_v,
                    frame->format);
}
#endif

// tex width | frame width | frame height | from color space | to color space
static const int nv12Planes[][5] = {
    {1, 1, 1, GL_R8, GL_RED},  // Y
    {2, 2, 2, GL_RG8, GL_RG},  // UV
    {0, 0, 0, 0, 0},           // NOT EXISTS
};

static const int yuv420Planes[][5] = {
    {1, 1, 1, GL_R8, GL_RED},  // Y
    {1, 2, 2, GL_R8, GL_RED},  // U
    {1, 2, 2, GL_R8, GL_RED},  // V
};

static const int p010Planes[][5] = {
    {2, 1, 2, GL_R16, GL_RED},  // Y
    {4, 2, 4, GL_RG16, GL_RG},  // UV
    {0, 0, 0, 0, 0},            // NOT EXISTS
};

static const float vertices[] = {-1.0f, -1.0f, 1.0f, -1.0f,
                                 -1.0f, 1.0f,  1.0f, 1.0f};

static const char* texture_mappings[] = {"plane0", "plane1", "plane2"};

static const float* gl_color_offset(bool color_full) {
    static const float limitedOffsets[] = {16.0f / 255.0f, 128.0f / 255.0f,
                                           128.0f / 255.0f};
    static const float fullOffsets[] = {0.0f, 128.0f / 255.0f, 128.0f / 255.0f};
    return color_full ? fullOffsets : limitedOffsets;
}

static const float* gl_color_matrix(enum AVColorSpace color_space,
                                    bool color_full) {
    static const float bt601Lim[] = {1.1644f, 1.1644f, 1.1644f,  0.0f, -0.3917f,
                                     2.0172f, 1.5960f, -0.8129f, 0.0f};
    static const float bt601Full[] = {
        1.0f, 1.0f, 1.0f, 0.0f, -0.3441f, 1.7720f, 1.4020f, -0.7141f, 0.0f};
    static const float bt709Lim[] = {1.1644f, 1.1644f, 1.1644f,  0.0f, -0.2132f,
                                     2.1124f, 1.7927f, -0.5329f, 0.0f};
    static const float bt709Full[] = {
        1.0f, 1.0f, 1.0f, 0.0f, -0.1873f, 1.8556f, 1.5748f, -0.4681f, 0.0f};
    static const float bt2020Lim[] = {1.1644f, 1.1644f,  1.1644f,
                                      0.0f,    -0.1874f, 2.1418f,
                                      1.6781f, -0.6505f, 0.0f};
    static const float bt2020Full[] = {
        1.0f, 1.0f, 1.0f, 0.0f, -0.1646f, 1.8814f, 1.4746f, -0.5714f, 0.0f};

    switch (color_space) {
    case AVCOL_SPC_SMPTE170M:
    case AVCOL_SPC_BT470BG:
        return color_full ? bt601Full : bt601Lim;
    case AVCOL_SPC_BT709:
        return color_full ? bt709Full : bt709Lim;
    case AVCOL_SPC_BT2020_NCL:
    case AVCOL_SPC_BT2020_CL:
        return color_full ? bt2020Full : bt2020Lim;
    default:
        return bt601Lim;
    }
}

static void check_shader(GLuint handle) {
    GLint success = 0;
    glGetShaderiv(handle, GL_COMPILE_STATUS, &success);

#ifndef _WIN32
    brls::Logger::info("GL: GL_COMPILE_STATUS: {}", success);
#endif

    if (!success) {
        GLint length = 0;
        glGetShaderiv(handle, GL_INFO_LOG_LENGTH, &length);

        char* buffer = (char*)malloc(length);

        glGetShaderInfoLog(handle, length, &length, buffer);

#ifndef _WIN32
        brls::Logger::error("GL: Compile shader error: {}", buffer);
#endif

        free(buffer);
    }
}

static bool use_core_shaders() {
    char* version = (char*)glGetString(GL_SHADING_LANGUAGE_VERSION);
    if (!version)
        return false;
    return version[0] == '3' || version[0] == '4';
}

GLVideoRenderer::~GLVideoRenderer() {

#ifndef _WIN32
    brls::Logger::info("GL: Cleanup...");
#endif

    releaseGlResources();

#ifndef _WIN32
    brls::Logger::info("GL: Cleanup done!");
#endif
}

void GLVideoRenderer::releaseGlResources() {
    if (m_shader_program) {
        glDeleteProgram(m_shader_program);
        m_shader_program = 0;
    }

    if (m_vbo) {
        glDeleteBuffers(1, &m_vbo);
        m_vbo = 0;
    }

    if (m_vao) {
        glDeleteVertexArrays(1, &m_vao);
        m_vao = 0;
    }

    for (int i = 0; i < PLANES_NUM_MAX; i++) {
        if (m_texture_id[i]) {
            glDeleteTextures(1, &m_texture_id[i]);
            m_texture_id[i] = 0;
        }
        m_texture_uniform[i] = -1;
    }

    m_is_initialized = false;
    m_frame_format = -1;
    m_frame_width = 0;
    m_frame_height = 0;
    m_screen_width = 0;
    m_screen_height = 0;
    m_attrib_logged = false;
    currentFrameTypePlanesNum = 0;
}

void GLVideoRenderer::initialize(AVFrame* frame) {
    render_diag_log("gl_info version=%s glsl=%s renderer=%s",
                    gl_string_or_unknown(GL_VERSION),
                    gl_string_or_unknown(GL_SHADING_LANGUAGE_VERSION),
                    gl_string_or_unknown(GL_RENDERER));

    m_shader_program = glCreateProgram();
    GLuint vert = glCreateShader(GL_VERTEX_SHADER);
    GLuint frag = glCreateShader(GL_FRAGMENT_SHADER);

    bool use_gl_core = use_core_shaders();

    glShaderSource(vert, 1,
                   use_gl_core ? &vertex_shader_string_core
                               : &vertex_shader_string,
                   nullptr);
    glCompileShader(vert);
    check_shader(vert);

    switch (frame->format) {
        case AV_PIX_FMT_YUV420P:
        case AV_PIX_FMT_YUVJ420P:
            currentFrameTypePlanesNum = 3;
            currentPlanes = yuv420Planes;
            currentFormat = GL_UNSIGNED_BYTE;

            glShaderSource(frag, 1,
                   use_gl_core ? &fragment_three_planes_shader_string_core
                               : &fragment_three_planes_shader_string, nullptr);
            break;
        case AV_PIX_FMT_NV12:
            currentFrameTypePlanesNum = 2;
            currentPlanes = nv12Planes;
            currentFormat = GL_UNSIGNED_BYTE;

            glShaderSource(frag, 1,
                   use_gl_core ? &fragment_two_planes_shader_string_core
                               : &fragment_two_planes_shader_string, nullptr);
            break;
        case AV_PIX_FMT_P010:
            currentFrameTypePlanesNum = 2;
            currentPlanes = p010Planes;
            currentFormat = GL_UNSIGNED_SHORT;

            glShaderSource(frag, 1,
                   use_gl_core ? &fragment_two_planes_shader_string_core
                               : &fragment_two_planes_shader_string, nullptr);
            break;
        default:
            brls::Logger::info("GL: Unknown frame format! - {}", frame->format);
            m_is_initialized = false;
            return;
    }

    glCompileShader(frag);
    check_shader(frag);

    glAttachShader(m_shader_program, vert);
    glAttachShader(m_shader_program, frag);
    glBindAttribLocation(m_shader_program, 0, "position");

    glLinkProgram(m_shader_program);
    GLint link_success = 0;
    glGetProgramiv(m_shader_program, GL_LINK_STATUS, &link_success);
    render_diag_log("gl_program_link status=%d frameFormat=%d", link_success, frame ? frame->format : -1);
    if (!link_success) {
        GLint length = 0;
        glGetProgramiv(m_shader_program, GL_INFO_LOG_LENGTH, &length);
        if (length > 0) {
            char* buffer = (char*)malloc(length);
            if (buffer) {
                glGetProgramInfoLog(m_shader_program, length, &length, buffer);
                render_diag_log("gl_program_link_error %s", buffer);
                free(buffer);
            }
        }
    }

    glDeleteShader(vert);
    glDeleteShader(frag);

    glGenBuffers(1, &m_vbo);
    glGenVertexArrays(1, &m_vao);

    for (int i = 0; i < currentFrameTypePlanesNum; i++) {
        m_texture_uniform[i] =
            glGetUniformLocation(m_shader_program, texture_mappings[i]);
    }

    m_yuvmat_location = glGetUniformLocation(m_shader_program, "yuvmat");
    m_offset_location = glGetUniformLocation(m_shader_program, "offset");
    m_uv_data_location = glGetUniformLocation(m_shader_program, "uv_data");
    m_frame_format = frame->format;
    render_diag_log("gl_uniforms plane0=%d plane1=%d plane2=%d yuvmat=%d offset=%d uv=%d err=0x%x",
                    m_texture_uniform[0],
                    m_texture_uniform[1],
                    m_texture_uniform[2],
                    m_yuvmat_location,
                    m_offset_location,
                    m_uv_data_location,
                    gl_drain_error());
}

void GLVideoRenderer::bindTexture(int id) {
    float borderColorInternal[] = {borderColor[id], 0.0f, 0.0f, 1.0f};
    glBindTexture(GL_TEXTURE_2D, m_texture_id[id]);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexParameterfv(GL_TEXTURE_2D, GL_TEXTURE_BORDER_COLOR, borderColorInternal);
    textureWidth[id] = m_frame_width / currentPlanes[id][1];
    textureHeight[id] = m_frame_height / currentPlanes[id][2];
    glTexImage2D(GL_TEXTURE_2D, 0, currentPlanes[id][3], textureWidth[id], textureHeight[id],
                 0, currentPlanes[id][4], currentFormat, nullptr);
    glUniform1i(m_texture_uniform[id], id);
}

void GLVideoRenderer::checkAndInitialize(int width, int height,
                                         AVFrame* frame) {
    if (m_is_initialized && frame && frame->format != m_frame_format) {
        brls::Logger::info("GL: frame format changed from {} to {}, rebuilding renderer", m_frame_format, frame->format);
        releaseGlResources();
    }

    if (!m_is_initialized) {
#ifndef _WIN32
//        brls::Logger::info("GL: GL: {}, GLSL: {}", glGetString(GL_VERSION),
//                           glGetString(GL_SHADING_LANGUAGE_VERSION));
        brls::Logger::info("GL: Init with width: {}, height: {}", width,
                           height);
#endif

        m_is_initialized = true;
        initialize(frame);

#ifndef _WIN32
        brls::Logger::info("GL: Init done");
#endif
    }
}

void GLVideoRenderer::checkAndUpdateScale(int width, int height,
                                          AVFrame* frame) {
    if ((m_frame_width != frame->width) || (m_frame_height != frame->height) ||
        (m_screen_width != width) || (m_screen_height != height) ||
        !use_core_shaders()) // Dirty fix for GLES, need to investigate the source of issue
    {

        m_frame_width = frame->width;
        m_frame_height = frame->height;

        m_screen_width = width;
        m_screen_height = height;

        glBindBuffer(GL_ARRAY_BUFFER, m_vbo);
        glBufferData(GL_ARRAY_BUFFER, sizeof(vertices), vertices,
                     GL_STATIC_DRAW);

        int positionLocation =
            glGetAttribLocation(m_shader_program, "position");
        if (positionLocation >= 0) {
            glEnableVertexAttribArray(0);
            glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, nullptr);
            if (!m_attrib_logged) {
                m_attrib_logged = true;
                render_diag_log("gl_attrib position=%d bound_to=0 err=0x%x", positionLocation, gl_drain_error());
            }
        } else if (!m_attrib_logged) {
            m_attrib_logged = true;
            render_diag_log("gl_attrib missing position=%d err=0x%x", positionLocation, gl_drain_error());
        }

        for (int i = 0; i < currentFrameTypePlanesNum; i++) {
            if (m_texture_id[i]) {
                glDeleteTextures(1, &m_texture_id[i]);
            }
        }

        glGenTextures(currentFrameTypePlanesNum, m_texture_id);

        for (int i = 0; i < currentFrameTypePlanesNum; i++) {
            bindTexture(i);
        }

        bool colorFull = frame->color_range == AVCOL_RANGE_JPEG;

        glUniform3fv(m_offset_location, 1, gl_color_offset(colorFull));
        glUniformMatrix3fv(m_yuvmat_location, 1, GL_FALSE,
                           gl_color_matrix(frame->colorspace, colorFull));

        float frameAspect = ((float)m_frame_height / (float)m_frame_width);
        float screenAspect = ((float)m_screen_height / (float)m_screen_width);

        if (frameAspect > screenAspect) {
            float multiplier = frameAspect / screenAspect;
            glUniform4f(m_uv_data_location, 0.5f - 0.5f * (1.0f / multiplier),
                        0.0f, multiplier, 1.0f);
        } else {
            float multiplier = screenAspect / frameAspect;
            glUniform4f(m_uv_data_location, 0.0f,
                        0.5f - 0.5f * (1.0f / multiplier), 1.0f, multiplier);
        }
    }
}

void GLVideoRenderer::draw(NVGcontext* vg, int width, int height,
                           AVFrame* frame, int imageFormat) {
    drawLatest(vg, width, height, frame, imageFormat, m_uploaded_generation + 1);
}

void GLVideoRenderer::drawLatest(NVGcontext* vg, int width, int height,
                                 AVFrame* frame, int imageFormat, uint64_t generation) {
    (void)vg;
    (void)imageFormat;

    if (!m_video_render_stats_progress.rendered_frames) {
        m_video_render_stats_progress.measurement_start_timestamp = LiGetMillis();
    }

    uint64_t before_render = LiGetMillis();

    checkAndInitialize(width, height, frame);
    if (!m_is_initialized || !m_shader_program || currentFrameTypePlanesNum <= 0)
        return;

    if (!m_first_frame_logged) {
        m_first_frame_logged = true;
        render_diag_log("first_frame width=%d height=%d format=%d linesize=%d/%d/%d colorRange=%d colorSpace=%d",
                        frame ? frame->width : -1,
                        frame ? frame->height : -1,
                        frame ? frame->format : -1,
                        frame ? frame->linesize[0] : 0,
                        frame ? frame->linesize[1] : 0,
                        frame ? frame->linesize[2] : 0,
                        frame ? frame->color_range : -1,
                        frame ? frame->colorspace : -1);
    }
    if (!m_frame_probe_dumped) {
        m_frame_probe_dumped = true;
#if defined(OPENNOW_ENABLE_FRAME_PROBE)
        dump_first_frame_probe(frame);
#endif
    }

    glViewport(0, 0, width, height);
    glDisable(GL_SCISSOR_TEST);
    glDisable(GL_STENCIL_TEST);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_CULL_FACE);
    glDisable(GL_BLEND);

    glBindVertexArray(m_vao);

    glUseProgram(m_shader_program);
    checkAndUpdateScale(width, height, frame);

    glClearColor(0, 0, 0, 1);
    glClear(GL_COLOR_BUFFER_BIT);

    const bool upload_frame = generation != 0 && generation != m_uploaded_generation;
    for (int i = 0; i < currentFrameTypePlanesNum; i++) {
        glActiveTexture(GL_TEXTURE0 + i);
        glBindTexture(GL_TEXTURE_2D, m_texture_id[i]);
        if (upload_frame) {
            uint8_t* image = frame->data[i];
            if (!image)
                continue;

            int real_width = frame->linesize[i] / currentPlanes[i][0];
            glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
            glPixelStorei(GL_UNPACK_ROW_LENGTH, real_width);
            glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, textureWidth[i],
                            textureHeight[i], currentPlanes[i][4], currentFormat, image);
        }
    }
    glActiveTexture(GL_TEXTURE0);
    if (upload_frame) {
        m_uploaded_generation = generation;
        m_unique_frame_count++;
    }
    glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 4);

    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    m_rendered_frame_count++;
    if (!m_first_draw_logged) {
        m_first_draw_logged = true;
        const GLenum draw_error = gl_drain_error();
        render_diag_log("first_draw screen=%dx%d frame=%dx%d planes=%d tex0=%dx%d tex1=%dx%d tex2=%dx%d err=0x%x",
                        width,
                        height,
                        m_frame_width,
                        m_frame_height,
                        currentFrameTypePlanesNum,
                        textureWidth[0],
                        textureHeight[0],
                        textureWidth[1],
                        textureHeight[1],
                        textureWidth[2],
                        textureHeight[2],
                        draw_error);
    } else if (m_rendered_frame_count % 600 == 0) {
        const GLenum draw_error = gl_drain_error();
        render_diag_log("draw_progress rendered=%llu unique=%llu generation=%llu screen=%dx%d frame=%dx%d err=0x%x",
                        static_cast<unsigned long long>(m_rendered_frame_count),
                        static_cast<unsigned long long>(m_unique_frame_count),
                        static_cast<unsigned long long>(m_uploaded_generation),
                        width,
                        height,
                        m_frame_width,
                        m_frame_height,
                        draw_error);
    }

    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, 0);
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    glBindVertexArray(0);
    glUseProgram(0);
    glEnable(GL_BLEND);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_CULL_FACE);

    auto render_time = LiGetMillis() - before_render;
    timeCount += render_time;

    m_video_render_stats_progress.total_render_time += render_time;
    m_video_render_stats_progress.rendered_frames++;

    const int time_interval = 200;
    if (timeCount >= time_interval) {
        // brls::Logger::debug("FPS: {}", frames / 5.0f);
        m_video_render_stats_cache = m_video_render_stats_progress;
        m_video_render_stats_progress = {};

        uint64_t now = LiGetMillis();
        m_video_render_stats_cache.rendered_fps = (float) m_video_render_stats_cache.rendered_frames /
                ((float)(now - m_video_render_stats_cache.measurement_start_timestamp) / 1000);

        m_video_render_stats_cache.rendering_time = (float)m_video_render_stats_cache.total_render_time /
                (float) m_video_render_stats_cache.rendered_frames;

        timeCount -= time_interval;
    }

//    auto code = glGetError();
//    brls::Logger::error("OpenGL error: {}\n", code);
}

VideoRenderStats* GLVideoRenderer::video_render_stats() {
    return &m_video_render_stats_cache;
}

int GLVideoRenderer::getFrameColorspace(const AVFrame* frame) {
    if (!frame) return COLORSPACE_REC_601;
    switch (frame->colorspace) {
    case AVCOL_SPC_SMPTE170M:
    case AVCOL_SPC_BT470BG:
        return COLORSPACE_REC_601;
    case AVCOL_SPC_BT709:
        return COLORSPACE_REC_709;
    case AVCOL_SPC_BT2020_NCL:
    case AVCOL_SPC_BT2020_CL:
        return COLORSPACE_REC_2020;
    default:
        return getDecoderColorspace();
    }
}

bool GLVideoRenderer::isFrameFullRange(const AVFrame* frame) {
    if (!frame) return false;
    return frame->color_range == AVCOL_RANGE_JPEG;
}

#endif // USE_GL_RENDERER
