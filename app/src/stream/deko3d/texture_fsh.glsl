#version 460

layout (location = 0) in vec2 vTextureCoord;
layout (location = 0) out vec4 outColor;

layout (binding = 0) uniform sampler2D plane0;
layout (binding = 1) uniform sampler2D plane1;

layout (std140, binding = 0) uniform Transformation
{
    mat3 yuvmat;
    vec3 offset;
    vec4 uv_data;
    vec4 quality;
} u;

void main()
{
    vec2 uv = (vTextureCoord - u.uv_data.xy) * u.uv_data.zw;
    float center = texture(plane0, uv).r;
    float luma = center;
    if (u.quality.z + u.quality.w > 0.0) {
        vec2 texel = u.quality.xy;
        float north = texture(plane0, uv - vec2(0.0, texel.y)).r;
        float south = texture(plane0, uv + vec2(0.0, texel.y)).r;
        float west = texture(plane0, uv - vec2(texel.x, 0.0)).r;
        float east = texture(plane0, uv + vec2(texel.x, 0.0)).r;
        float average = (north + south + west + east) * 0.25;
        float local_min = min(center, min(min(north, south), min(west, east)));
        float local_max = max(center, max(max(north, south), max(west, east)));
        float local_range = local_max - local_min;

        // Smooth low-contrast quantization blocks, then restore only bounded
        // high-contrast detail. This is spatial and adds no buffered frame.
        float flat_weight = 1.0 - smoothstep(0.025, 0.10, local_range);
        float cleaned = mix(center, average, u.quality.z * flat_weight);
        float edge_weight = smoothstep(0.045, 0.20, local_range);
        luma = cleaned + (cleaned - average) * u.quality.w * edge_weight;
        luma = clamp(luma, local_min - 0.008, local_max + 0.008);
    }
    vec3 yuv = vec3(luma, texture(plane1, uv).rg) - u.offset;
    outColor = vec4(clamp(u.yuvmat * yuv, 0.0, 1.0), 1.0);
}
