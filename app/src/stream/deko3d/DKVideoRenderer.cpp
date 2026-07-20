#if defined(PLATFORM_SWITCH) && defined(BOREALIS_USE_DEKO3D)

#include "DKVideoRenderer.hpp"
#include "SoftwareYuvUpload.hpp"
#include "../../stream_settings.hpp"
#include "../../video_quality_policy.hpp"

#include <borealis/platforms/switch/switch_platform.hpp>

extern "C" {
#include <libavutil/hwcontext_nvtegra.h>
}

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstring>

namespace {

constexpr unsigned kStaticCommandBytes = 0x10000;
constexpr unsigned kUpdateCommandSliceBytes = 0x1000;
constexpr unsigned kUpdateCommandSlices = 8;
constexpr size_t kSoftwareFrameSlots = 4;

uint64_t monotonic_millis()
{
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count());
}

struct Transformation {
    alignas(16) float matrix_column_0[4];
    alignas(16) float matrix_column_1[4];
    alignas(16) float matrix_column_2[4];
    alignas(16) float offset[4];
    alignas(16) float uv[4];
    alignas(16) float quality[4];
};

static_assert(sizeof(Transformation) == 96);
static_assert(offsetof(Transformation, offset) == 48);
static_assert(offsetof(Transformation, uv) == 64);
static_assert(offsetof(Transformation, quality) == 80);

struct Vertex {
    float position[3];
    float uv[2];
};

constexpr std::array kVertexAttributes = {
    DkVtxAttribState{0, 0, offsetof(Vertex, position), DkVtxAttribSize_3x32,
                     DkVtxAttribType_Float, 0},
    DkVtxAttribState{0, 0, offsetof(Vertex, uv), DkVtxAttribSize_2x32,
                     DkVtxAttribType_Float, 0},
};

constexpr std::array kVertexBuffers = {
    DkVtxBufferState{sizeof(Vertex), 0},
};

constexpr std::array kQuadVertices = {
    Vertex{{-1.0f, +1.0f, 0.0f}, {0.0f, 0.0f}},
    Vertex{{-1.0f, -1.0f, 0.0f}, {0.0f, 1.0f}},
    Vertex{{+1.0f, -1.0f, 0.0f}, {1.0f, 1.0f}},
    Vertex{{+1.0f, +1.0f, 0.0f}, {1.0f, 0.0f}},
};

void set_color_transform(Transformation& transform, AVColorSpace color_space, bool full_range)
{
    static constexpr float bt601_limited[9] = {
        1.1644f, 1.1644f, 1.1644f, 0.0f, -0.3917f, 2.0172f, 1.5960f, -0.8129f, 0.0f};
    static constexpr float bt601_full[9] = {
        1.0f, 1.0f, 1.0f, 0.0f, -0.3441f, 1.7720f, 1.4020f, -0.7141f, 0.0f};
    static constexpr float bt709_limited[9] = {
        1.1644f, 1.1644f, 1.1644f, 0.0f, -0.2132f, 2.1124f, 1.7927f, -0.5329f, 0.0f};
    static constexpr float bt709_full[9] = {
        1.0f, 1.0f, 1.0f, 0.0f, -0.1873f, 1.8556f, 1.5748f, -0.4681f, 0.0f};
    static constexpr float bt2020_limited[9] = {
        1.1644f, 1.1644f, 1.1644f, 0.0f, -0.1874f, 2.1418f, 1.6781f, -0.6505f, 0.0f};
    static constexpr float bt2020_full[9] = {
        1.0f, 1.0f, 1.0f, 0.0f, -0.1646f, 1.8814f, 1.4746f, -0.5714f, 0.0f};

    const float* matrix = full_range ? bt601_full : bt601_limited;
    if (color_space == AVCOL_SPC_BT709)
        matrix = full_range ? bt709_full : bt709_limited;
    else if (color_space == AVCOL_SPC_BT2020_NCL || color_space == AVCOL_SPC_BT2020_CL)
        matrix = full_range ? bt2020_full : bt2020_limited;

    std::memcpy(transform.matrix_column_0, matrix, 3 * sizeof(float));
    std::memcpy(transform.matrix_column_1, matrix + 3, 3 * sizeof(float));
    std::memcpy(transform.matrix_column_2, matrix + 6, 3 * sizeof(float));
    transform.offset[0] = full_range ? 0.0f : 16.0f / 255.0f;
    transform.offset[1] = 128.0f / 255.0f;
    transform.offset[2] = 128.0f / 255.0f;
}

} // namespace

DKVideoRenderer::~DKVideoRenderer()
{
    if (video_context_)
        queue_.waitIdle();
    while (!submitted_frames_.empty()) {
        AVFrame* frame = submitted_frames_.front();
        submitted_frames_.pop_front();
        av_frame_free(&frame);
    }
    frame_mappings_.clear();
    releaseSoftwareSlots();
    update_cmd_memory_.destroy();
    vertex_buffer_.destroy();
    transform_buffer_.destroy();
    if (video_context_) {
        if (luma_texture_id_ > 0)
            video_context_->freeImageIndex(luma_texture_id_);
        if (chroma_texture_id_ > 0)
            video_context_->freeImageIndex(chroma_texture_id_);
    }
}

void DKVideoRenderer::releaseSoftwareSlots()
{
    for (auto& slot : software_slots_) {
        slot.luma_memory.destroy();
        slot.chroma_memory.destroy();
        slot.luma_upload.destroy();
        slot.chroma_upload.destroy();
    }
    software_slots_.clear();
}

void DKVideoRenderer::retainSubmittedFrame(AVFrame* frame)
{
    AVFrame* reference = frame ? av_frame_clone(frame) : nullptr;
    if (!reference) {
        brls::Logger::error("Deko3D renderer failed to retain NVDEC surface");
        return;
    }

    submitted_frames_.push_back(reference);
    // Keep substantially more surfaces than the three display framebuffers.
    // This gives Deko3D ~133 ms at 60 fps to finish sampling without blocking
    // the UI thread or allowing NVDEC to immediately recycle the memory.
    constexpr size_t kGpuFramesInFlight = 8;
    if (submitted_frames_.size() <= kGpuFramesInFlight)
        return;

    // Queue submission is ordered and the oldest reference is several display
    // cycles behind. Never call waitIdle() here: an NVDEC/Deko dependency can
    // otherwise block the Borealis render thread indefinitely.
    AVFrame* completed = submitted_frames_.front();
    submitted_frames_.pop_front();
    av_frame_free(&completed);
}

int DKVideoRenderer::getFrameColorspace(const AVFrame* frame)
{
    if (!frame)
        return COLORSPACE_REC_601;
    switch (frame->colorspace) {
    case AVCOL_SPC_BT709:
        return COLORSPACE_REC_709;
    case AVCOL_SPC_BT2020_NCL:
    case AVCOL_SPC_BT2020_CL:
        return COLORSPACE_REC_2020;
    default:
        return COLORSPACE_REC_601;
    }
}

bool DKVideoRenderer::isFrameFullRange(const AVFrame* frame)
{
    return frame && frame->color_range == AVCOL_RANGE_JPEG;
}

void DKVideoRenderer::checkAndInitialize(int width, int height, AVFrame* frame)
{
    if (initialized_ || !frame)
        return;
    hardware_frames_ = frame->format == AV_PIX_FMT_NVTEGRA;
    const bool software_frame = frame->format == AV_PIX_FMT_YUV420P ||
                                frame->format == AV_PIX_FMT_YUVJ420P ||
                                frame->format == AV_PIX_FMT_NV12;
    if (!hardware_frames_ && !software_frame) {
        brls::Logger::error("Deko3D video renderer unsupported frame format {}", frame->format);
        return;
    }

    frame_width_ = frame->width;
    frame_height_ = frame->height;
    screen_width_ = width;
    screen_height_ = height;
    video_context_ = static_cast<brls::SwitchVideoContext*>(
        brls::Application::getPlatform()->getVideoContext());
    device_ = video_context_->getDeko3dDevice();
    queue_ = video_context_->getQueue();

    code_pool_.emplace(device_,
        DkMemBlockFlags_CpuUncached | DkMemBlockFlags_GpuCached | DkMemBlockFlags_Code,
        128 * 1024);
    data_pool_.emplace(device_, DkMemBlockFlags_CpuUncached | DkMemBlockFlags_GpuCached,
                       1 * 1024 * 1024);

    static_cmd_buf_ = dk::CmdBufMaker{device_}.create();
    CMemPool::Handle command_memory = data_pool_->allocate(kStaticCommandBytes);
    static_cmd_buf_.addMemory(command_memory.getMemBlock(), command_memory.getOffset(),
                              command_memory.getSize());
    update_cmd_buf_ = dk::CmdBufMaker{device_}.create();
    update_cmd_memory_ = data_pool_->allocate(
        kUpdateCommandSliceBytes * kUpdateCommandSlices, DK_CMDMEM_ALIGNMENT);
    vertex_shader_.load(*code_pool_, "romfs:/shaders/opennow_video_vsh.dksh");
    fragment_shader_.load(*code_pool_, "romfs:/shaders/opennow_video_fsh.dksh");
    vertex_buffer_ = data_pool_->allocate(sizeof(kQuadVertices), alignof(Vertex));
    std::memcpy(vertex_buffer_.getCpuAddr(), kQuadVertices.data(), vertex_buffer_.getSize());
    transform_buffer_ = code_pool_->allocate(sizeof(Transformation), DK_UNIFORM_BUF_ALIGNMENT);

    Transformation transform {};
    bool full_range = isFrameFullRange(frame);
    // CloudMatch negotiates limited range; some NVTEGRA frames incorrectly report JPEG.
    if (frame->color_range == AVCOL_RANGE_JPEG)
        full_range = false;
    set_color_transform(transform, frame->colorspace, full_range);

    const float frame_aspect = static_cast<float>(frame_height_) / frame_width_;
    const float screen_aspect = static_cast<float>(screen_height_) / screen_width_;
    if (frame_aspect > screen_aspect) {
        const float multiplier = frame_aspect / screen_aspect;
        transform.uv[0] = 0.5f - 0.5f / multiplier;
        transform.uv[1] = 0.0f;
        transform.uv[2] = multiplier;
        transform.uv[3] = 1.0f;
    } else {
        const float multiplier = screen_aspect / frame_aspect;
        transform.uv[0] = 0.0f;
        transform.uv[1] = 0.5f - 0.5f / multiplier;
        transform.uv[2] = 1.0f;
        transform.uv[3] = multiplier;
    }

    const auto quality_settings = opennow::LoadStreamSettings();
    const auto tuning = opennow::video::ResolveQualityTuning(
        quality_settings.image_quality_mode);
    transform.quality[0] = 1.0f / static_cast<float>(frame_width_);
    transform.quality[1] = 1.0f / static_cast<float>(frame_height_);
    transform.quality[2] = tuning.denoise_strength;
    transform.quality[3] = tuning.sharpen_strength;

    luma_texture_id_ = video_context_->allocateImageIndex();
    chroma_texture_id_ = video_context_->allocateImageIndex();
    dk::ImageLayoutMaker{device_}
        .setType(DkImageType_2D).setFormat(DkImageFormat_R8_Unorm)
        .setDimensions(frame_width_, frame_height_, 1)
        .setFlags(DkImageFlags_UsageLoadStore | DkImageFlags_Usage2DEngine |
                  DkImageFlags_UsageVideo)
        .initialize(luma_layout_);
    dk::ImageLayoutMaker{device_}
        .setType(DkImageType_2D).setFormat(DkImageFormat_RG8_Unorm)
        .setDimensions(frame_width_ / 2, frame_height_ / 2, 1)
        .setFlags(DkImageFlags_UsageLoadStore | DkImageFlags_Usage2DEngine |
                  DkImageFlags_UsageVideo)
        .initialize(chroma_layout_);

    if (!hardware_frames_) {
        image_pool_.emplace(
            device_, DkMemBlockFlags_CpuUncached | DkMemBlockFlags_GpuCached |
                         DkMemBlockFlags_Image,
            16 * 1024 * 1024);
        upload_pool_.emplace(
            device_, DkMemBlockFlags_CpuUncached | DkMemBlockFlags_GpuCached,
            16 * 1024 * 1024);
        software_slots_.reserve(kSoftwareFrameSlots);
        for (size_t i = 0; i < kSoftwareFrameSlots; ++i) {
            SoftwareFrameSlot slot;
            slot.luma_memory = image_pool_->allocate(
                luma_layout_.getSize(), luma_layout_.getAlignment());
            slot.chroma_memory = image_pool_->allocate(
                chroma_layout_.getSize(), chroma_layout_.getAlignment());
            slot.luma_upload = upload_pool_->allocate(
                static_cast<uint32_t>(frame_width_ * frame_height_),
                DK_IMAGE_LINEAR_STRIDE_ALIGNMENT);
            slot.chroma_upload = upload_pool_->allocate(
                static_cast<uint32_t>(frame_width_ * frame_height_ / 2),
                DK_IMAGE_LINEAR_STRIDE_ALIGNMENT);
            if (!slot.luma_memory || !slot.chroma_memory ||
                !slot.luma_upload || !slot.chroma_upload) {
                brls::Logger::error("Deko3D software upload allocation failed at slot {}", i);
                slot.luma_memory.destroy();
                slot.chroma_memory.destroy();
                slot.luma_upload.destroy();
                slot.chroma_upload.destroy();
                releaseSoftwareSlots();
                return;
            }
            slot.luma.initialize(
                luma_layout_, slot.luma_memory.getMemBlock(), slot.luma_memory.getOffset());
            slot.chroma.initialize(
                chroma_layout_, slot.chroma_memory.getMemBlock(), slot.chroma_memory.getOffset());
            slot.luma_descriptor.initialize(slot.luma);
            slot.chroma_descriptor.initialize(slot.chroma);
            software_slots_.emplace_back(std::move(slot));
        }
    }

    dk::RasterizerState rasterizer;
    dk::ColorState color;
    dk::ColorWriteState color_write;
    static_cmd_buf_.clear();
    static_cmd_buf_.clearColor(0, DkColorMask_RGBA, 0.0f, 0.0f, 0.0f, 1.0f);
    static_cmd_buf_.bindShaders(DkStageFlag_GraphicsMask, {vertex_shader_, fragment_shader_});
    static_cmd_buf_.bindTextures(DkStage_Fragment, 0,
                                 dkMakeTextureHandle(luma_texture_id_, 0));
    static_cmd_buf_.bindTextures(DkStage_Fragment, 1,
                                 dkMakeTextureHandle(chroma_texture_id_, 0));
    static_cmd_buf_.bindUniformBuffer(DkStage_Fragment, 0, transform_buffer_.getGpuAddr(),
                                      transform_buffer_.getSize());
    static_cmd_buf_.pushConstants(transform_buffer_.getGpuAddr(), transform_buffer_.getSize(),
                                  0, sizeof(transform), &transform);
    static_cmd_buf_.bindRasterizerState(rasterizer);
    static_cmd_buf_.bindColorState(color);
    static_cmd_buf_.bindColorWriteState(color_write);
    static_cmd_buf_.bindVtxBuffer(0, vertex_buffer_.getGpuAddr(), vertex_buffer_.getSize());
    static_cmd_buf_.bindVtxAttribState(kVertexAttributes);
    static_cmd_buf_.bindVtxBufferState(kVertexBuffers);
    static_cmd_buf_.draw(DkPrimitive_Quads, kQuadVertices.size(), 1, 0, 0);
    static_cmd_list_ = static_cmd_buf_.finishList();
    initialized_ = true;
    brls::Logger::info(
        "Deko3D {} renderer initialized {}x{} quality={}",
        hardware_frames_ ? "NVTEGRA zero-copy" : "software NV12 upload",
        frame_width_, frame_height_, quality_settings.image_quality_mode);
}

void DKVideoRenderer::bindDescriptors(
    const dk::ImageDescriptor& luma, const dk::ImageDescriptor& chroma)
{
    update_cmd_buf_.clear();
    update_cmd_buf_.addMemory(
        update_cmd_memory_.getMemBlock(),
        update_cmd_memory_.getOffset() + update_cmd_slice_ * kUpdateCommandSliceBytes,
        kUpdateCommandSliceBytes);
    update_cmd_slice_ = (update_cmd_slice_ + 1) % kUpdateCommandSlices;
    video_context_->updateImageDescriptor(update_cmd_buf_, luma_texture_id_, luma);
    video_context_->updateImageDescriptor(update_cmd_buf_, chroma_texture_id_, chroma);
}

bool DKVideoRenderer::updateSoftwareFrame(AVFrame* frame)
{
    if (!frame || hardware_frames_ || software_slots_.empty() ||
        !frame->data[0] || !frame->data[1])
        return false;

    auto& slot = software_slots_[software_slot_cursor_];
    software_slot_cursor_ = (software_slot_cursor_ + 1) % software_slots_.size();
    auto* luma = static_cast<uint8_t*>(slot.luma_upload.getCpuAddr());
    auto* chroma = static_cast<uint8_t*>(slot.chroma_upload.getCpuAddr());

    const int chroma_height = frame_height_ / 2;
    if (!opennow::video::CopyYuv420ToNv12(
            frame_width_, frame_height_,
            frame->data[0], frame->linesize[0],
            frame->data[1], frame->linesize[1],
            frame->data[2], frame->linesize[2],
            frame->format == AV_PIX_FMT_NV12,
            luma, chroma))
        return false;

    bindDescriptors(slot.luma_descriptor, slot.chroma_descriptor);
    dk::ImageView luma_view{slot.luma};
    dk::ImageView chroma_view{slot.chroma};
    update_cmd_buf_.copyBufferToImage(
        {slot.luma_upload.getGpuAddr(), 0, 0}, luma_view,
        {0, 0, 0, static_cast<uint32_t>(frame_width_),
         static_cast<uint32_t>(frame_height_), 1});
    update_cmd_buf_.copyBufferToImage(
        {slot.chroma_upload.getGpuAddr(), 0, 0}, chroma_view,
        {0, 0, 0, static_cast<uint32_t>(frame_width_ / 2),
         static_cast<uint32_t>(chroma_height), 1});
    queue_.submitCommands(update_cmd_buf_.finishList());
    return true;
}

bool DKVideoRenderer::updateFrameMapping(AVFrame* frame, uint64_t generation)
{
    if (!frame || frame->format != AV_PIX_FMT_NVTEGRA || !frame->buf[0])
        return false;
    AVNVTegraMap* map = av_nvtegra_frame_get_fbuf_map(frame);
    if (!map || !frame->data[0] || !frame->data[1])
        return false;

    const uint32_t handle = av_nvtegra_map_get_handle(map);
    void* cpu_address = av_nvtegra_map_get_addr(map);
    const uint32_t size = av_nvtegra_map_get_size(map);
    const uint32_t chroma_offset = static_cast<uint32_t>(frame->data[1] - frame->data[0]);
    int index = -1;
    for (size_t i = 0; i < frame_mappings_.size(); ++i) {
        const auto& candidate = frame_mappings_[i];
        if (candidate.handle == handle && candidate.cpu_address == cpu_address &&
            candidate.size == size && candidate.chroma_offset == chroma_offset) {
            index = static_cast<int>(i);
            break;
        }
    }

    if (index < 0) {
        constexpr size_t kMappingTarget = 16;
        constexpr size_t kMappingHardLimit = 32;
        constexpr uint64_t kSafeGenerationAge = 8;
        while (frame_mappings_.size() >= kMappingTarget) {
            size_t stale = frame_mappings_.size();
            for (size_t i = 0; i < frame_mappings_.size(); ++i) {
                if (static_cast<int>(i) == current_mapping_)
                    continue;
                const uint64_t last_used = frame_mappings_[i].last_used_generation;
                if (generation > last_used && generation - last_used > kSafeGenerationAge) {
                    stale = i;
                    break;
                }
            }
            if (stale == frame_mappings_.size())
                break;
            frame_mappings_.erase(frame_mappings_.begin() + static_cast<long>(stale));
            if (current_mapping_ > static_cast<int>(stale))
                current_mapping_--;
        }
        if (frame_mappings_.size() >= kMappingHardLimit) {
            brls::Logger::error("Deko3D mapping cache exhausted; retaining previous good frame");
            return false;
        }

        FrameMapping mapping;
        mapping.handle = handle;
        mapping.cpu_address = cpu_address;
        mapping.size = size;
        mapping.chroma_offset = chroma_offset;
        mapping.last_used_generation = generation;
        mapping.memory = dk::MemBlockMaker{device_, size}
            .setFlags(DkMemBlockFlags_CpuUncached | DkMemBlockFlags_GpuCached |
                      DkMemBlockFlags_Image)
            .setStorage(cpu_address).create();
        mapping.luma.initialize(luma_layout_, mapping.memory, 0);
        mapping.chroma.initialize(chroma_layout_, mapping.memory, chroma_offset);
        mapping.luma_descriptor.initialize(mapping.luma);
        mapping.chroma_descriptor.initialize(mapping.chroma);
        frame_mappings_.emplace_back(std::move(mapping));
        index = static_cast<int>(frame_mappings_.size()) - 1;
    }

    frame_mappings_[index].last_used_generation = generation;

    if (index == current_mapping_)
        return true;
    auto& active = frame_mappings_[index];
    bindDescriptors(active.luma_descriptor, active.chroma_descriptor);
    queue_.submitCommands(update_cmd_buf_.finishList());
    current_mapping_ = index;
    return true;
}

void DKVideoRenderer::draw(NVGcontext* vg, int width, int height, AVFrame* frame, int image_format)
{
    drawLatest(vg, width, height, frame, image_format, rendered_generation_ + 1);
}

void DKVideoRenderer::drawLatest(NVGcontext* vg, int width, int height, AVFrame* frame,
                                 int image_format, uint64_t generation)
{
    (void)vg;
    (void)image_format;
    const uint64_t started = monotonic_millis();
    checkAndInitialize(width, height, frame);
    if (!initialized_)
        return;
    if (generation != rendered_generation_) {
        const bool updated = hardware_frames_
            ? updateFrameMapping(frame, generation)
            : updateSoftwareFrame(frame);
        if (!updated)
            return;
        if (hardware_frames_)
            retainSubmittedFrame(frame);
        rendered_generation_ = generation;
    }
    if (static_cmd_list_)
        queue_.submitCommands(static_cmd_list_);
    if (render_stats_.rendered_frames == 0)
        render_stats_.measurement_start_timestamp = started;
    render_stats_.total_render_time += monotonic_millis() - started;
    render_stats_.rendered_frames++;
}

VideoRenderStats* DKVideoRenderer::video_render_stats()
{
    render_stats_.retained_surfaces = static_cast<uint32_t>(submitted_frames_.size());
    render_stats_.surface_mappings = hardware_frames_
        ? static_cast<uint32_t>(frame_mappings_.size())
        : static_cast<uint32_t>(software_slots_.size());
    const uint64_t elapsed = monotonic_millis() - render_stats_.measurement_start_timestamp;
    render_stats_.rendered_fps = elapsed > 0
        ? static_cast<float>(render_stats_.rendered_frames) * 1000.0f / elapsed : 0.0f;
    render_stats_.rendering_time = render_stats_.rendered_frames > 0
        ? static_cast<float>(render_stats_.total_render_time) / render_stats_.rendered_frames : 0.0f;
    return &render_stats_;
}

#endif
