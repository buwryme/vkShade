#version 450

// Alpha-based MSAA depth resolve shader (multi-sample average)
// Reads depth from the alpha channel instead of luminance (R channel)
// Supports optional depth inversion via push constant
//
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

// Push constant for runtime-configurable inversion
layout(push_constant) uniform DepthParams {
    bool invertDepth;
} depthParams;

void main()
{
    ivec2 coords = ivec2(gl_FragCoord.xy);
    float depth;

    if (AVERAGE_MODE == 1)
    {
        float sum = 0.0;
        for (int i = 0; i < NUM_SAMPLES; ++i)
            sum += texelFetch(depthTexture, coords, i).a;
        depth = sum / float(NUM_SAMPLES);
    }
    else
    {
        depth = texelFetch(depthTexture, coords, 0).a;
    }
    
    // Apply inversion if enabled: depth = 1.0 - depth
    if (depthParams.invertDepth)
    {
        depth = 1.0 - depth;
    }
    
    resolvedDepth = depth;
}
