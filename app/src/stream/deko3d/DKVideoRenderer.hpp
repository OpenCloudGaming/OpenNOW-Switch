#if defined(__SWITCH__) && defined(BOREALIS_USE_DEKO3D)

#pragma once

#include "../IVideoRenderer.hpp"

#include <borealis.hpp>
#include <borealis/platforms/switch/switch_video.hpp>
#include <deko3d.hpp>
#include <nanovg/framework/CShader.h>

#include <optional>
#include <deque>
#include <vector>

class DKVideoRenderer : public IVideoRenderer {
public:
    DKVideoRenderer() = default;
    ~DKVideoRenderer() override;

    void draw(NVGcontext* vg, int width, int height, AVFrame* frame, int imageFormat) override;
    void drawLatest(NVGcontext* vg, int width, int height, AVFrame* frame,
                    int imageFormat, uint64_t generation) override;
    VideoRenderStats* video_render_stats() override;
    int getFrameColorspace(const AVFrame* frame) override;
    bool isFrameFullRange(const AVFrame* frame) override;

private:
    void checkAndInitialize(int width, int height, AVFrame* frame);
    bool updateFrameMapping(AVFrame* frame, uint64_t generation);
    bool updateSoftwareFrame(AVFrame* frame);
    void bindDescriptors(const dk::ImageDescriptor& luma,
                         const dk::ImageDescriptor& chroma);
    void releaseSoftwareSlots();
    void retainSubmittedFrame(AVFrame* frame);

    bool initialized_ = false;
    int frame_width_ = 0;
    int frame_height_ = 0;
    int screen_width_ = 0;
    int screen_height_ = 0;

    brls::SwitchVideoContext* video_context_ = nullptr;
    dk::Device device_;
    dk::Queue queue_;
    std::optional<CMemPool> code_pool_;
    std::optional<CMemPool> data_pool_;
    dk::UniqueCmdBuf static_cmd_buf_;
    dk::UniqueCmdBuf update_cmd_buf_;
    CMemPool::Handle update_cmd_memory_;
    uint32_t update_cmd_slice_ = 0;
    DkCmdList static_cmd_list_ = 0;
    CShader vertex_shader_;
    CShader fragment_shader_;
    CMemPool::Handle vertex_buffer_;
    CMemPool::Handle transform_buffer_;
    dk::ImageLayout luma_layout_;
    dk::ImageLayout chroma_layout_;
    bool hardware_frames_ = false;

    struct FrameMapping {
        uint32_t handle = 0;
        void* cpu_address = nullptr;
        uint32_t size = 0;
        uint32_t chroma_offset = 0;
        uint64_t last_used_generation = 0;
        dk::UniqueMemBlock memory;
        dk::Image luma;
        dk::Image chroma;
        dk::ImageDescriptor luma_descriptor;
        dk::ImageDescriptor chroma_descriptor;
    };

    std::vector<FrameMapping> frame_mappings_;

    struct SoftwareFrameSlot {
        CMemPool::Handle luma_memory;
        CMemPool::Handle chroma_memory;
        CMemPool::Handle luma_upload;
        CMemPool::Handle chroma_upload;
        dk::Image luma;
        dk::Image chroma;
        dk::ImageDescriptor luma_descriptor;
        dk::ImageDescriptor chroma_descriptor;
    };

    std::optional<CMemPool> image_pool_;
    std::optional<CMemPool> upload_pool_;
    std::vector<SoftwareFrameSlot> software_slots_;
    size_t software_slot_cursor_ = 0;
    int current_mapping_ = -1;
    int luma_texture_id_ = 0;
    int chroma_texture_id_ = 0;
    uint64_t rendered_generation_ = 0;
    std::deque<AVFrame*> submitted_frames_;
    VideoRenderStats render_stats_ {};
};

#endif
