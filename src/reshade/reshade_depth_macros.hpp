#ifndef VKSHADE_RESHADE_DEPTH_MACROS_HPP_INCLUDED
#define VKSHADE_RESHADE_DEPTH_MACROS_HPP_INCLUDED

#include "reshade/effect_preprocessor.hpp"

namespace vkShade
{
    // Add ReShade preprocessor macros for depth buffer handling.
    // These macros tell ReShade effects how to interpret the depth values
    // that vkShade passes to them. Proper configuration is critical for
    // flawless depth-based effect rendering (DOF, SSAO, etc.)
    //
    // Parameters:
    //   pp            - ReShade preprocessor to add macros to
    //   depthMode     - Current depth source mode (0-6, see below)
    //   invertDepth   - Whether depth values are inverted
    
    inline void addReshadeDepthMacros(reshadefx::preprocessor& pp, 
                                       int depthMode = 0, 
                                       bool invertDepth = false)
    {
        // Depth mode meanings:
        // 0 = Luminance/Red (standard Vulkan depth)
        // 1 = Alpha (alpha-encoded depth)
        // 2 = Packed RGB (depth in RGB channels)
        // 3 = Logarithmic (log-encoded depth)
        // 4 = View-space Z (raw view-space Z)
        // 5 = NDC (Normalized Device Coordinates)
        // 6 = Reversed-Z (inverted for precision)

        // --- Orientation flags ---
        
        // Is the depth buffer upside down (OpenGL-style)?
        // Vulkan uses top-left origin, so this is typically 0
        pp.add_macro_definition("RESHADE_DEPTH_INPUT_IS_UPSIDE_DOWN", "0");

        // Is the depth input reversed (larger value = closer)?
        // Set based on our depth mode and inversion setting
        const bool isReversed = (depthMode == 6) || invertDepth; // Reversed-Z or manually inverted
        pp.add_macro_definition("RESHADE_DEPTH_INPUT_IS_REVERSED", isReversed ? "1" : "0");

        // Is the depth logarithmically encoded?
        // Set based on depth mode - critical for proper linearization in effects
        const bool isLogarithmic = (depthMode == 3);
        pp.add_macro_definition("RESHADE_DEPTH_INPUT_IS_LOGARITHMIC", isLogarithmic ? "1" : "0");

        // --- Coordinate transformation ---
        
        // Scale factors for depth texture coordinates
        // Typically 1.0 unless the depth buffer is a different resolution
        pp.add_macro_definition("RESHADE_DEPTH_INPUT_X_SCALE", "1.0");
        pp.add_macro_definition("RESHADE_DEPTH_INPUT_Y_SCALE", "1.0");
        
        // Offset for depth texture coordinates
        // Used when depth buffer has padding or different origin
        pp.add_macro_definition("RESHADE_DEPTH_INPUT_X_OFFSET", "0.0");
        pp.add_macro_definition("RESHADE_DEPTH_INPUT_Y_OFFSET", "0.0");
        
        // Pixel offsets for sub-pixel accurate depth sampling
        pp.add_macro_definition("RESHADE_DEPTH_INPUT_X_PIXEL_OFFSET", "0");
        pp.add_macro_definition("RESHADE_DEPTH_INPUT_Y_PIXEL_OFFSET", "0");

        // --- Linearization parameters ---
        
        // Far plane distance used for linearizing non-linear depth formats
        // This should match the application's far clipping plane
        // Effects use this to convert [0,1] depth to linear view-space Z
        //
        // For different depth modes, we may need different defaults:
        // - Standard/NDC: Use app's far plane (default 1000.0)
        // - View-space Z: Already linear, but may need scaling
        // - Logarithmic: Linearized by shader, so standard far plane works
        float farPlane = 1000.0f;
        switch (depthMode)
        {
            case 4: // View-space Z - already linear, use larger range
                farPlane = 10000.0f;
                break;
            case 3: // Logarithmic - after exp() linearization
                farPlane = 1000.0f;
                break;
            case 5: // NDC - post-projection, standard range
                farPlane = 1000.0f;
                break;
            default:
                farPlane = 1000.0f;
                break;
        }
        pp.add_macro_definition("RESHADE_DEPTH_LINEARIZATION_FAR_PLANE", std::to_string(farPlane));

        // Multiplier applied to depth values before passing to effects
        // Used to scale depth to expected range
        // Most modes output [0,1] so multiplier is 1.0, but some modes
        // may need adjustment (e.g., View-space Z after normalization)
        float multiplier = 1.0f;
        switch (depthMode)
        {
            case 4: // View-space Z - may need scaling depending on normalization
                multiplier = 1.0f; // Shader normalizes to [0,1] / 1000.0
                break;
            case 2: // Packed RGB - should be normalized to [0,1] by shader
                multiplier = 1.0f;
                break;
            default:
                multiplier = 1.0f;
                break;
        }
        pp.add_macro_definition("RESHADE_DEPTH_MULTIPLIER", std::to_string(multiplier));

        // Mix stage depth map flag
        // 0 = don't mix, 1 = blend with existing depth
        // For vkShade's resolved depth, we always provide clean depth
        pp.add_macro_definition("RESHADE_MIX_STAGE_DEPTH_MAP", "0");
    }

} // namespace vkShade

#endif // VKSHADE_RESHADE_DEPTH_MACROS_HPP_INCLUDED
