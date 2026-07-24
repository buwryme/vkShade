#ifndef CONFIG_SERIALIZER_HPP_INCLUDED
#define CONFIG_SERIALIZER_HPP_INCLUDED

#include <string>
#include <vector>
#include <map>

#include "effects/effect_config.hpp"

namespace vkShade
{
    // Serialized parameter format for config files
    struct ConfigParam
    {
        std::string effectName;
        std::string paramName;
        std::string value;
    };

    // Global vkShade settings (from vkShade.conf)
    struct VkBasaltSettings
    {
        int maxEffects = 10;
        bool overlayBlockInput = true;
        std::string toggleKey = "End";
        std::string reloadKey = "F10";
        std::string overlayKey = "Home";
        bool enableOnLaunch = true;
        bool depthCapture = false;
        bool autoApply = true;  // Auto-apply changes without clicking Apply
        int autoApplyDelay = 200;  // ms delay before auto-applying changes
        bool showDebugWindow = false;  // Show debug window with raw effect registry data
        // Depth buffer resolve mode for MSAA sources (Advanced tab).
        // 0 = auto (prefer average, fall back to sample-zero),
        // 1 = sample-zero, 2 = average. Only applied when supported by the device.
        int depthResolveMode = 0;
        // Optional manual depth buffer pin (Advanced tab). When non-empty, the
        // layer pins depth capture to this tracked image view key instead of
        // using the best-candidate heuristic. "" = auto-promotion.
        std::string depthManualPin;
        // Workaround for transient multisampled depth attachments (Advanced tab).
        // When true, vkShade injects its own depth resolve attachment so the
        // resolved depth survives past the render pass for effects to sample.
        bool depthTransientWorkaround = false;
        // Depth capture method for persistent storage path (Advanced tab).
        // 0 = off (use legacy per-swapchain resolve only),
        // 1 = Option A: RenderPassEnd hook (recommended, blit inline at CmdEndRenderPass),
        // 2 = Option B: QueueSubmit interception (robust fallback, inject blit at submit).
        int depthCaptureMethod = 0;

        // --- Alternative depth buffer handling ---
        // Depth source channel/mode selection (Advanced tab).
        // Comprehensive support for deferred rendering depth encodings:
        //   0 = Luminance/Red (standard Vulkan depth from R channel)
        //   1 = Alpha (alpha-encoded depth)
        //   2 = Packed RGB (depth distributed across RGB channels)
        //   3 = Logarithmic (log-encoded depth, needs linearization)
        //   4 = View-space Z (raw view-space Z value)
        //   5 = Normalized Device Coordinates / NDC
        //   6 = Reversed-Z (inverted depth range for precision)
        int depthSourceChannel = 0;
        // Invert depth values after reading from source.
        // When enabled, depth = 1.0 - depth (flips near/far planes).
        bool depthInvert = false;
    };

    // Per-profile settings (stored in per-game .conf files)
    struct ProfileSettings
    {
        // Profile settings (safeAntiCheat removed)
    };

    // Shader Manager configuration (from shader_manager.conf)
    struct ShaderManagerConfig
    {
        std::vector<std::string> parentDirectories;       // User-added parent dirs to scan
        std::vector<std::string> discoveredShaderPaths;   // Auto-discovered Shaders/ dirs
        std::vector<std::string> discoveredTexturePaths;  // Auto-discovered Textures/ dirs
    };

    class ConfigSerializer
    {
    public:
        // Save a game-specific config to ~/.config/vkShade/configs/<name>.conf
        // effects: all effects in the list (enabled + disabled)
        // disabledEffects: effects that are unchecked (won't be rendered)
        // params: all effect parameters
        // effectPaths: map of effect name to shader file path (for ReShade effects with custom names)
        // preprocessorDefs: preprocessor definitions to save (format: effectName#MACRO = value)
        static bool saveConfig(
            const std::string& configName,
            const std::vector<std::string>& effects,
            const std::vector<std::string>& disabledEffects,
            const std::vector<ConfigParam>& params,
            const std::map<std::string, std::string>& effectPaths = {},
            const std::vector<PreprocessorDefinition>& preprocessorDefs = {});

        // Get the base config directory path (~/.config/vkShade/)
        static std::string getBaseConfigDir();

        // Get the configs directory path (~/.config/vkShade/configs/)
        static std::string getConfigsDir();

        // List available config files
        static std::vector<std::string> listConfigs();

        // Delete a config file
        static bool deleteConfig(const std::string& configName);

        // Default config management
        static bool setDefaultConfig(const std::string& configName);
        static std::string getDefaultConfig();
        static std::string getDefaultConfigPath();

        // Global settings management (vkShade.conf)
        static VkBasaltSettings loadSettings();
        static bool saveSettings(const VkBasaltSettings& settings);

        // Shader Manager config (shader_manager.conf)
        static ShaderManagerConfig loadShaderManagerConfig();
        static bool saveShaderManagerConfig(const ShaderManagerConfig& config);

        // Ensure vkShade.conf exists with defaults (call early at startup)
        static void ensureConfigExists();

        // Detect the game executable name from /proc/self/exe
        static std::string detectGameName();

        // Check if a per-game config exists and return the game name if so.
        // Returns empty string if no per-game config was found.
        static std::string autoDetectConfig();

        // --- Per-app profile system ---

        // Get the full path for a game profile.
        // profileName "default" or "" → configs/<gameName>.conf
        // profileName "foo"           → configs/<gameName>@foo.conf
        static std::string getProfilePath(const std::string& gameName,
                                          const std::string& profileName = "");

        // Ensure the default profile for a game exists (creates with empty
        // effects list if missing). Returns the profile path.
        static std::string ensureGameProfile(const std::string& gameName);

        // List all profile names for a game ("default", "performance", etc.)
        static std::vector<std::string> listProfilesForGame(const std::string& gameName);

        // Get/set the active profile name for a game (persisted in .active_profiles)
        static std::string getActiveProfile(const std::string& gameName);
        static void setActiveProfile(const std::string& gameName,
                                     const std::string& profileName);

        // Create a new named profile for a game (copies from source or empty)
        static bool createProfile(const std::string& gameName,
                                  const std::string& profileName,
                                  const std::string& copyFromProfile = "");

        // Delete a named profile (cannot delete "default")
        static bool deleteProfile(const std::string& gameName,
                                  const std::string& profileName);

        // Load per-profile settings from a config file
        static ProfileSettings loadProfileSettings(const std::string& filePath);

        // Save directly to a profile path (bypasses config name lookup)
        static bool saveToPath(
            const std::string& filePath,
            const std::vector<std::string>& effects,
            const std::vector<std::string>& disabledEffects,
            const std::vector<ConfigParam>& params,
            const std::map<std::string, std::string>& effectPaths = {},
            const std::vector<PreprocessorDefinition>& preprocessorDefs = {},
            const ProfileSettings& profileSettings = {});
    };

} // namespace vkShade

#endif // CONFIG_SERIALIZER_HPP_INCLUDED
