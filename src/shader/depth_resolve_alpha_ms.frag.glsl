#version 450

// Alpha-based MSAA depth resolve shader (single sample)
// Reads depth from the alpha channel instead of luminance (R channel)
// Supports optional depth inversion via push constant

layout(set = 0, binding = 0) uniform sampler2DMS depthTexture;

layout(location = 0) in vec2 textureCoord;
layout(location = 0) out float resolvedDepth;

// Push constant for runtime-configurable inversion
layout(push_constant) uniform DepthParams {
    bool invertDepth;
} depthParams;

void main()
{
    float depth = texelFetch(depthTexture, ivec2(gl_FragCoord.xy), 0).a;
    
    // Apply inversion if enabled: depth = 1.0 - depth
    if (depthParams.invertDepth)
    {
        depth = 1.0 - depth;
    }
    
    resolvedDepth = depth;
}
