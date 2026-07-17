#version 450

// MSAA depth resolve shader (single sample) with inversion support
// Reads depth from luminance (R channel) with optional inversion via push constant

layout(set = 0, binding = 0) uniform sampler2DMS depthTexture;

layout(location = 0) in vec2 textureCoord;
layout(location = 0) out float resolvedDepth;

// Push constant for runtime-configurable inversion
layout(push_constant) uniform DepthParams {
    bool invertDepth;
} depthParams;

void main()
{
    float depth = texelFetch(depthTexture, ivec2(gl_FragCoord.xy), 0).r;
    
    // Apply inversion if enabled: depth = 1.0 - depth
    if (depthParams.invertDepth)
    {
        depth = 1.0 - depth;
    }
    
    resolvedDepth = depth;
}
