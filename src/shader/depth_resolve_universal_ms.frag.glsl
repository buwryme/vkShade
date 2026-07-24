#version 450

// Universal MSAA depth resolve shader (single sample) with comprehensive mode support
// Handles all common depth buffer encodings used by deferred renderers
//
// Uses texelFetch for per-sample access instead of texture sampling

layout(set = 0, binding = 0) uniform sampler2DMS depthTexture;

layout(location = 0) in vec2 textureCoord;
layout(location = 0) out float resolvedDepth;

// Push constants for runtime-configurable depth handling
layout(push_constant) uniform DepthParams {
    int  depthMode;      // Source channel/mode selection
    bool invertDepth;    // Invert after reading: depth = 1.0 - depth
    bool normalize;      // Normalize to [0,1] range if needed
} depthParams;

void main()
{
    vec4 texel = texelFetch(depthTexture, ivec2(gl_FragCoord.xy), 0);
    float depth = 0.0;

    // Extract depth based on selected source mode
    switch (depthParams.depthMode)
    {
        case 0: // Luminance/Red - Standard Vulkan depth
            depth = texel.r;
            break;
            
        case 1: // Alpha channel - Alpha-encoded depth
            depth = texel.a;
            break;
            
        case 2: // Packed RGB - Decode from RGB channels
            depth = dot(texel.rgb, vec3(0.299, 0.587, 0.114));
            break;
            
        case 3: // Logarithmic depth
            depth = exp(texel.r * 20.0) - 1.0;
            depth = clamp(depth, 0.0, 1.0);
            break;
            
        case 4: // View-space Z
            depth = texel.r;
            if (depthParams.normalize)
            {
                depth = abs(depth);
                depth = clamp(depth / 1000.0, 0.0, 1.0);
            }
            break;
            
        case 5: // Normalized Device Coordinates (NDC)
            depth = texel.r;
            if (depthParams.normalize)
            {
                depth = depth * 0.5 + 0.5;
            }
            break;
            
        case 6: // Reversed-Z
            depth = texel.r;
            break;
            
        default:
            depth = texel.r;
            break;
    }

    // Apply inversion if enabled
    if (depthParams.invertDepth && depthParams.depthMode != 6)
    {
        depth = 1.0 - depth;
    }

    resolvedDepth = depth;
}
