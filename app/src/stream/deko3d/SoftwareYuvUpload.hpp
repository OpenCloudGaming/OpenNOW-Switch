#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>

namespace opennow::video
{

inline bool CopyYuv420ToNv12(
    int width, int height,
    const uint8_t* y_plane, ptrdiff_t y_stride,
    const uint8_t* u_or_uv_plane, ptrdiff_t u_or_uv_stride,
    const uint8_t* v_plane, ptrdiff_t v_stride,
    bool source_is_nv12,
    uint8_t* output_y, uint8_t* output_uv)
{
    if (width <= 0 || height <= 0 || (width & 1) != 0 || (height & 1) != 0 ||
        !y_plane || !u_or_uv_plane || !output_y || !output_uv ||
        (!source_is_nv12 && !v_plane)) {
        return false;
    }

    for (int row = 0; row < height; ++row) {
        std::memcpy(output_y + static_cast<size_t>(row) * width,
                    y_plane + static_cast<ptrdiff_t>(row) * y_stride,
                    static_cast<size_t>(width));
    }

    const int chroma_height = height / 2;
    if (source_is_nv12) {
        for (int row = 0; row < chroma_height; ++row) {
            std::memcpy(output_uv + static_cast<size_t>(row) * width,
                        u_or_uv_plane + static_cast<ptrdiff_t>(row) * u_or_uv_stride,
                        static_cast<size_t>(width));
        }
        return true;
    }

    for (int row = 0; row < chroma_height; ++row) {
        const uint8_t* u = u_or_uv_plane + static_cast<ptrdiff_t>(row) * u_or_uv_stride;
        const uint8_t* v = v_plane + static_cast<ptrdiff_t>(row) * v_stride;
        uint8_t* uv = output_uv + static_cast<size_t>(row) * width;
        for (int column = 0; column < width / 2; ++column) {
            uv[column * 2] = u[column];
            uv[column * 2 + 1] = v[column];
        }
    }
    return true;
}

} // namespace opennow::video
