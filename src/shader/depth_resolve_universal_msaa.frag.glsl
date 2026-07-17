#version 450

// Universal MSAA depth resolve shader (multi-sample average) with comprehensive mode support
// Handles all common depth buffer encodings used by deferred renderers
//
// Averages all samples via texelFetch. When the device does not advertise
// VK_RESOLVE_MODE_AVERAGE_BIT, the caller selects sample 0 instead.
layout(set = 0, binding = 0) uniform sampler2DMS depthTexture;

layout(location = 0) in vec2 textureCoord;
layout(location = 0) out float resolvedDepth;

layout(constant_id = 0) const int NUM_SAMPLES = 1;
layout(constant_id = 1) const int AVERAGE_MODE = 1;  // 1 = average, 0 = sample 0

// Push constants for runtime-configurable depth handling
layout(push_constant) uniform DepthParams {
    int  depthMode;      // Source channel/mode selection
    bool invertDepth;    // Invert after reading: depth = 1.0 - depth
    bool normalize;      // Normalize to [0,1] range if needed
} depthParams;

float extractDepth(vec4 texel)
{
    float depth = 0.0;
    
    switch (depthParams.depthMode)
    {
        case 0: // Luminance/Red
            depth = texel.r;
            break;
            
        case 1: // Alpha channel
            depth = texel.a;
            break;
            
        case 2: // Packed RGB
            depth = dot(texel.rgb, vec3(0.299, 0.587, 0.114));
            break;
            
        case 3: // Logarithmic
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
            
        case 5: // NDC
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
    
    return depth;
}

void main()
{
    ivec2 coords = ivec2(gl_FragCoord.xy);
    float depth;

    if (AVERAGE_MODE == 1)
    {
        float sum = 0.0;
        for (int i = 0; i < NUM_SAMPLES; ++i)
        {
            vec4 texel = texelFetch(depthTexture, coords, i);
            sum += extractDepth(texel);
        }
        depth = sum / float(NUM_SAMPLES);
    }
    else
    {
        vec4 texel = texelFetch(depthTexture, coords, 0);
        depth = extractDepth(texel);
    }

    // Apply inversion if enabled
    if (depthParams.invertDepth && depthParams.depthMode != 6)
    {
        depth = 1.0 - depth;
    }

    resolvedDepth = depth;
}
