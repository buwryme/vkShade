#version 450

// Universal depth resolve shader with comprehensive mode support
// Handles all common depth buffer encodings used by deferred renderers
//
// Modes:
//   0 = Luminance/Red (standard Vulkan depth)
//   1 = Alpha channel (alpha-encoded depth)
//   2 = Packed RGB (depth distributed across RGB, e.g. RGB24/RGB32)
//   3 = Logarithmic (log-encoded depth)
//   4 = View-space Z (raw view-space Z, typically negative)
//   5 = Normalized Device Coordinates (NDC, post-projection [-1,1] or [0,1])
//   6 = Reversed-Z (inverted depth range for precision)

layout(set = 0, binding = 0) uniform sampler2D depthTexture;

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
    vec4 texel = textureLod(depthTexture, textureCoord, 0.0);
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
            // Common packing: R=high bits, G=mid bits, B=low bits
            // Or could be normalized across all channels
            depth = dot(texel.rgb, vec3(0.299, 0.587, 0.114)); // Luminance fallback
            // Alternative: treat as packed 24-bit value
            // depth = (texel.r * 65536.0 + texel.g * 256.0 + texel.b) / 16777215.0;
            break;
            
        case 3: // Logarithmic depth - exp() to linearize
            // log_depth = log(z/z_near) / log(z_far/z_near)
            // z = z_near * exp(log_depth * log(z_far/z_near))
            // For now, assume standard log encoding in R channel
            depth = exp(texel.r * 20.0) - 1.0; // Approximate linearization
            depth = clamp(depth, 0.0, 1.0);
            break;
            
        case 4: // View-space Z (typically negative in OpenGL convention)
            depth = texel.r;
            // Convert negative view-space Z to [0,1] depth
            if (depthParams.normalize)
            {
                depth = abs(depth); // Take absolute value
                // Normalize assuming typical view range of 0-1000 units
                depth = clamp(depth / 1000.0, 0.0, 1.0);
            }
            break;
            
        case 5: // Normalized Device Coordinates (NDC)
            depth = texel.r;
            // NDC can be [-1, 1] or [0, 1] depending on API/convention
            if (depthParams.normalize)
            {
                // Map from [-1, 1] to [0, 1]
                depth = depth * 0.5 + 0.5;
            }
            break;
            
        case 6: // Reversed-Z (already inverted at source level)
            depth = texel.r;
            // Reversed-Z is already "inverted" relative to standard depth
            // Don't apply additional inversion unless explicitly requested
            break;
            
        default: // Fallback to luminance
            depth = texel.r;
            break;
    }

    // Apply inversion if enabled (except reversed-z which is already inverted)
    if (depthParams.invertDepth && depthParams.depthMode != 6)
    {
        depth = 1.0 - depth;
    }

    resolvedDepth = depth;
}
