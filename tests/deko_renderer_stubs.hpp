#pragma once

#include "stream/deko3d/GpuConfigurationQueue.hpp"
#include "stream/deko3d/GpuFrameQueue.hpp"
#include "stream/deko3d/SoftwareYuvUpload.hpp"
#include "stream_settings.hpp"
#include "video_quality_policy.hpp"

#include <algorithm>
#include <array>
#include <cassert>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <functional>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <vector>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/mem.h>
}

#define AV_PIX_FMT_NVTEGRA 10000

struct NVGcontext {};
struct AVNVTegraMap { uint32_t handle; uint8_t* address; uint32_t size; };
struct AVNVTegraFrame { AVBufferRef* map_ref; };

inline AVNVTegraMap* av_nvtegra_frame_get_fbuf_map(AVFrame* frame) {
    auto* descriptor = reinterpret_cast<AVNVTegraFrame*>(frame->buf[0]->data);
    return reinterpret_cast<AVNVTegraMap*>(descriptor->map_ref->data);
}
inline uint32_t av_nvtegra_map_get_handle(AVNVTegraMap* map) { return map->handle; }
inline void* av_nvtegra_map_get_addr(AVNVTegraMap* map) { return map->address; }
inline uint32_t av_nvtegra_map_get_size(AVNVTegraMap* map) { return map->size; }

enum {
    DkResult_Success, DkResult_Timeout, DkMemBlockFlags_CpuUncached = 1,
    DkMemBlockFlags_GpuCached = 2, DkMemBlockFlags_Code = 4, DkMemBlockFlags_Image = 8,
    DkImageType_2D, DkImageFormat_R8_Unorm, DkImageFormat_RG8_Unorm,
    DkImageFlags_UsageLoadStore = 1, DkImageFlags_Usage2DEngine = 2,
    DkImageFlags_UsageVideo = 4, DkVtxAttribSize_3x32, DkVtxAttribSize_2x32,
    DkVtxAttribType_Float, DkColorMask_RGBA, DkStageFlag_GraphicsMask,
    DkStage_Fragment, DkPrimitive_Quads, DK_CMDMEM_ALIGNMENT = 16,
    DK_UNIFORM_BUF_ALIGNMENT = 16, DK_IMAGE_LINEAR_STRIDE_ALIGNMENT = 16
};
struct DkVtxAttribState { int buffer, unused; size_t offset; int size, type, last; };
struct DkVtxBufferState { size_t stride; int divisor; };
inline int dkMakeTextureHandle(int image, int) { return image; }

namespace deko_test {

inline int allocation = 0;
inline int fail_at = -1;
inline bool throw_allocation_failure = false;
inline uint64_t submitted = 0;
inline uint64_t completed = 0;
inline int wait_idle = 0;
inline int submissions = 0;
inline int hardware_storage_freed = 0;
inline std::set<int> descriptors;
inline std::map<int, uint64_t> descriptor_use;
inline std::map<int, int> descriptor_memory;
inline std::vector<int> queued_memory;
inline std::vector<int> queued_descriptors;
inline std::array<float, 24> last_transform {};
inline int last_luma_bytes = 0;
inline int draws = 0;

inline bool allocate() {
    if (++allocation != fail_at)
        return true;
    if (throw_allocation_failure)
        throw std::bad_alloc();
    return false;
}

struct Memory {
    static inline int next_id = 0;
    static inline std::map<int, Memory*> live;
    int id = ++next_id;
    std::vector<uint8_t> bytes;
    void* external = nullptr;
    uint64_t last_use = 0;
    explicit Memory(size_t size, void* storage = nullptr) : bytes(size), external(storage) {
        live[id] = this;
    }
    ~Memory() {
        assert(last_use <= completed);
        live.erase(id);
    }
};

inline int memory_id(uint64_t address) { return static_cast<int>(address >> 32); }
inline uint64_t address(int id) { return static_cast<uint64_t>(id) << 32; }
inline void* cpu(int id) {
    auto* memory = Memory::live.at(id);
    assert(memory->last_use <= completed);
    return memory->external ? memory->external : memory->bytes.data();
}

struct Commands {
    std::vector<int> memory;
    std::vector<int> textures;
    std::vector<std::function<void()>> operations;
    bool draw = false;
};

inline void reset() {
    assert(Memory::live.empty() && descriptors.empty());
    allocation = 0;
    fail_at = -1;
    throw_allocation_failure = false;
    submitted = completed = 0;
    wait_idle = submissions = hardware_storage_freed = draws = 0;
    queued_memory.clear();
    queued_descriptors.clear();
    descriptor_memory.clear();
    descriptor_use.clear();
}

}

using DkCmdList = deko_test::Commands*;

namespace dk {

struct Device {};
struct Fence {
    uint64_t sequence = 0;
    int wait(int timeout) {
        assert(timeout == 0);
        return sequence <= deko_test::completed ? DkResult_Success : DkResult_Timeout;
    }
};
struct MemBlock { int id = 0; };
struct UniqueMemBlock : MemBlock {
    std::shared_ptr<deko_test::Memory> owned;
    explicit operator bool() const { return static_cast<bool>(owned); }
};
struct MemBlockMaker {
    size_t size;
    void* storage = nullptr;
    MemBlockMaker(Device, size_t bytes) : size(bytes) {}
    MemBlockMaker& setFlags(int) { return *this; }
    MemBlockMaker& setStorage(void* value) { storage = value; return *this; }
    UniqueMemBlock create() {
        if (!deko_test::allocate())
            return {};
        auto memory = std::make_shared<deko_test::Memory>(size, storage);
        return {{memory->id}, memory};
    }
};
struct ImageLayout {
    size_t size = 0;
    size_t getSize() const { return size; }
    size_t getAlignment() const { return 16; }
};
struct ImageLayoutMaker {
    int format = 0;
    size_t size = 0;
    explicit ImageLayoutMaker(Device) {}
    ImageLayoutMaker& setType(int) { return *this; }
    ImageLayoutMaker& setFormat(int value) { format = value; return *this; }
    ImageLayoutMaker& setFlags(int) { return *this; }
    ImageLayoutMaker& setDimensions(int width, int height, int) {
        size = width * height * (format == DkImageFormat_RG8_Unorm ? 2 : 1);
        return *this;
    }
    void initialize(ImageLayout& layout) { layout.size = size; }
};
struct Image {
    int memory = 0;
    size_t size = 0;
    void initialize(ImageLayout layout, MemBlock block, size_t offset) {
        memory = block.id;
        size = layout.size;
        assert(offset + size <= deko_test::Memory::live.at(memory)->bytes.size());
    }
};
struct ImageView { Image image; };
struct ImageDescriptor {
    int memory = 0;
    void initialize(Image image) { memory = image.memory; }
};
struct RasterizerState {};
struct ColorState {};
struct ColorWriteState {};
struct Shader { int memory = 0; };
struct BufferImageCopy { uint64_t address; int row, height; };
struct ImageRect { uint32_t x, y, z, width, height, depth; };

struct UniqueCmdBuf {
    std::shared_ptr<deko_test::Commands> commands;
    int command_memory = 0;
    explicit operator bool() const { return static_cast<bool>(commands); }
    void clear() {
        *commands = {};
        if (command_memory)
            commands->memory.push_back(command_memory);
    }
    void addMemory(MemBlock block, size_t, size_t) {
        command_memory = block.id;
        commands->memory.push_back(block.id);
    }
    void clearColor(int, int, float, float, float, float) {}
    void bindShaders(int, std::initializer_list<Shader> shaders) {
        for (auto shader : shaders)
            commands->memory.push_back(shader.memory);
    }
    void bindTextures(int, int, int texture) { commands->textures.push_back(texture); }
    void bindUniformBuffer(int, int, uint64_t address, size_t) {
        commands->memory.push_back(deko_test::memory_id(address));
    }
    void pushConstants(uint64_t, size_t, size_t, size_t bytes, const void* value) {
        std::array<float, 24> transform;
        assert(bytes == sizeof(transform));
        std::memcpy(transform.data(), value, bytes);
        commands->operations.push_back([transform] { deko_test::last_transform = transform; });
    }
    void bindRasterizerState(RasterizerState) {}
    void bindColorState(ColorState) {}
    void bindColorWriteState(ColorWriteState) {}
    void bindVtxBuffer(int, uint64_t address, size_t) {
        commands->memory.push_back(deko_test::memory_id(address));
    }
    template <typename T> void bindVtxAttribState(const T&) {}
    template <typename T> void bindVtxBufferState(const T&) {}
    void draw(int, size_t, int, int, int) { commands->draw = true; }
    void copyBufferToImage(BufferImageCopy source, ImageView target, ImageRect) {
        commands->memory.push_back(deko_test::memory_id(source.address));
        commands->memory.push_back(target.image.memory);
    }
    DkCmdList finishList() { return commands.get(); }
};
struct CmdBufMaker {
    explicit CmdBufMaker(Device) {}
    UniqueCmdBuf create() {
        return deko_test::allocate() ? UniqueCmdBuf{std::make_shared<deko_test::Commands>()}
                                     : UniqueCmdBuf{};
    }
};
struct Queue {
    void submitCommands(DkCmdList commands) {
        assert(commands);
        ++deko_test::submissions;
        for (auto& operation : commands->operations)
            operation();
        for (int memory : commands->memory) {
            assert(deko_test::Memory::live.contains(memory));
            deko_test::queued_memory.push_back(memory);
        }
        for (int texture : commands->textures) {
            assert(deko_test::descriptors.contains(texture));
            int memory = deko_test::descriptor_memory.at(texture);
            assert(deko_test::Memory::live.contains(memory));
            deko_test::queued_memory.push_back(memory);
            deko_test::queued_descriptors.push_back(texture);
        }
        if (commands->draw) {
            ++deko_test::draws;
            int memory = deko_test::descriptor_memory.at(commands->textures.front());
            deko_test::last_luma_bytes = deko_test::Memory::live.at(memory)->bytes.size();
        }
    }
    void signalFence(Fence& fence, bool flush) {
        assert(flush);
        fence.sequence = ++deko_test::submitted;
        for (int memory : deko_test::queued_memory)
            deko_test::Memory::live.at(memory)->last_use = fence.sequence;
        for (int descriptor : deko_test::queued_descriptors)
            deko_test::descriptor_use[descriptor] = fence.sequence;
        deko_test::queued_memory.clear();
        deko_test::queued_descriptors.clear();
    }
    void waitIdle() {
        ++deko_test::wait_idle;
        deko_test::completed = deko_test::submitted;
    }
};

}

class CMemPool {
    std::map<int, std::shared_ptr<deko_test::Memory>> memory_;
public:
    CMemPool(dk::Device, int, size_t) {}
    struct Handle {
        CMemPool* pool = nullptr;
        int id = 0;
        explicit operator bool() const { return pool != nullptr; }
        void destroy() { if (pool) pool->memory_.erase(id); pool = nullptr; }
        void* getCpuAddr() const { return deko_test::cpu(id); }
        uint64_t getGpuAddr() const { return deko_test::address(id); }
        dk::MemBlock getMemBlock() const { return {id}; }
        size_t getOffset() const { return 0; }
        size_t getSize() const { return deko_test::Memory::live.at(id)->bytes.size(); }
    };
    Handle allocate(size_t size, size_t = 16) {
        if (!deko_test::allocate())
            return {};
        auto memory = std::make_shared<deko_test::Memory>(size);
        memory_[memory->id] = memory;
        return {this, memory->id};
    }
};

class CShader {
    CMemPool::Handle memory_;
public:
    ~CShader() { memory_.destroy(); }
    bool load(CMemPool& pool, const char*) { memory_ = pool.allocate(64); return bool(memory_); }
    operator dk::Shader() const { return {memory_.id}; }
};

namespace brls {

struct SwitchVideoContext {
    dk::Device getDeko3dDevice() { return {}; }
    dk::Queue getQueue() { return {}; }
    int allocateImageIndex() {
        if (!deko_test::allocate())
            return -1;
        int index = 0;
        while (deko_test::descriptors.contains(index))
            ++index;
        deko_test::descriptors.insert(index);
        return index;
    }
    void freeImageIndex(int index) {
        assert(deko_test::descriptor_use[index] <= deko_test::completed);
        assert(deko_test::descriptors.erase(index) == 1);
    }
    void updateImageDescriptor(dk::UniqueCmdBuf& commands, int index, dk::ImageDescriptor image) {
        commands.commands->operations.push_back([index, image] {
            assert(deko_test::descriptors.contains(index));
            deko_test::descriptor_memory[index] = image.memory;
        });
    }
};
struct Platform {
    SwitchVideoContext context;
    SwitchVideoContext* getVideoContext() { return &context; }
};
struct Application {
    static Platform* getPlatform() { static Platform platform; return &platform; }
};
struct Logger {
    template <typename... T> static void info(const char*, T&&...) {}
    template <typename... T> static void error(const char*, T&&...) {}
};

}

namespace opennow {
inline StreamSettings LoadStreamSettings() { return {}; }
}
