#if defined(__SWITCH__) && defined(BOREALIS_USE_DEKO3D)

#pragma once

#include "../IVideoRenderer.hpp"
#include "GpuConfigurationQueue.hpp"
#include "GpuFrameQueue.hpp"

#include <borealis.hpp>
#include <borealis/platforms/switch/switch_video.hpp>
#include <deko3d.hpp>
#include <nanovg/framework/CShader.h>

#include <optional>
#include <array>
#include <memory>
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
    struct Configuration {
        ~Configuration();
        bool initialize(int width, int height, AVFrame* frame, uint64_t generation);
        bool matches(int width, int height, const AVFrame* frame) const;
        bool prepareFrame(AVFrame* frame, uint64_t generation);
        bool drawLatest(AVFrame* frame, uint64_t generation);
        bool updateFrameMapping(AVFrame* frame);
        bool updateSoftwareFrame(AVFrame* frame);
        void bindDescriptors(const dk::ImageDescriptor& luma,
                             const dk::ImageDescriptor& chroma);
        void releaseSoftwareSlots();

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
        std::array<dk::Fence, 8> update_cmd_fences_ {};
        DkCmdList static_cmd_list_ = 0;
        CShader vertex_shader_;
        CShader fragment_shader_;
        CMemPool::Handle vertex_buffer_;
        CMemPool::Handle transform_buffer_;
        dk::ImageLayout luma_layout_;
        dk::ImageLayout chroma_layout_;
        bool hardware_frames_ = false;
        bool full_range_ = false;

        struct BufferDeleter {
            void operator()(AVBufferRef* buffer) const { av_buffer_unref(&buffer); }
        };

        struct FrameMapping {
            uint32_t handle = 0;
            void* cpu_address = nullptr;
            uint32_t size = 0;
            uint32_t chroma_offset = 0;
            std::unique_ptr<AVBufferRef, BufferDeleter> storage;
            dk::UniqueMemBlock memory;
            dk::Fence last_use_fence {};
            dk::Image luma;
            dk::Image chroma;
            dk::ImageDescriptor luma_descriptor;
            dk::ImageDescriptor chroma_descriptor;
        };

        std::vector<std::unique_ptr<FrameMapping>> frame_mappings_;

        struct SoftwareFrameSlot {
            dk::Fence last_use_fence {};
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
        int current_software_slot_ = -1;
        int current_mapping_ = -1;
        int luma_texture_id_ = -1;
        int chroma_texture_id_ = -1;
        uint64_t rendered_generation_ = 0;
        bool frame_update_pending_ = false;
        dk::Fence last_use_fence_ {};
        opennow::video::GpuFrameQueue<dk::Fence, 8> submitted_frames_;
    };

    opennow::video::GpuConfigurationQueue<Configuration> configurations_;
    VideoRenderStats render_stats_ {};
};

#endif
