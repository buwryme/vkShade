#version 450

// MSAA depth resolve fallback for depth images in GENERAL layout, where the
// depth-stencil resolve subpass path cannot be used (it requires attachment-
// optimal layouts). Manually averages all samples via texelFetch. When the
// device does not advertise VK_RESOLVE_MODE_AVERAGE_BIT, the caller selects
// sample 0 instead, which this shader also implements via a uniform flag.
layout(set = 0, binding = 0) uniform sampler2DMS depthTexture;

layout(location = 0) in vec2 textureCoord;
layout(location = 0) out float resolvedDepth;

layout(constant_id = 0) const int NUM_SAMPLES = 1;
layout(constant_id = 1) const int AVERAGE_MODE = 1;  // 1 = average, 0 = sample 0

void main()
{
    ivec2 coords = ivec2(gl_FragCoord.xy);

    if (AVERAGE_MODE == 1)
    {
        float sum = 0.0;
        for (int i = 0; i < NUM_SAMPLES; ++i)
            sum += texelFetch(depthTexture, coords, i).r;
        resolvedDepth = sum / float(NUM_SAMPLES);
    }
    else
    {
        resolvedDepth = texelFetch(depthTexture, coords, 0).r;
    }
}
