#ifndef SETTINGS_MANAGER_HPP_INCLUDED
#define SETTINGS_MANAGER_HPP_INCLUDED

#include <string>
#include <algorithm>

#include "config_serializer.hpp"

namespace vkShade
{
    // Single source of truth for all vkShade settings.
    // Similar to EffectRegistry for effect parameters.
    //
    // Usage:
    // - Call initialize() once at startup to load from config
    // - Read/write settings directly via getters/setters
    // - Call save() to persist changes to vkShade.conf
    class SettingsManager
    {
    public:
        // Initialize from vkShade.conf (call once at startup)
        void initialize();

        // Check if already initialized
        bool isInitialized() const { return initialized; }

        // Save current settings to vkShade.conf
        bool save();

        // Getters
        int getMaxEffects() const { return std::clamp(settings.maxEffects, 1, 200); }
        bool getOverlayBlockInput() const { return settings.overlayBlockInput; }
        const std::string& getToggleKey() const { return settings.toggleKey; }
        const std::string& getReloadKey() const { return settings.reloadKey; }
        const std::string& getOverlayKey() const { return settings.overlayKey; }
        bool getEnableOnLaunch() const { return settings.enableOnLaunch; }
        bool getDepthCapture() const { return settings.depthCapture; }
        bool getAutoApply() const { return settings.autoApply; }
        int getAutoApplyDelay() const { return settings.autoApplyDelay; }
        bool getShowDebugWindow() const { return settings.showDebugWindow; }
        bool getSafeAntiCheat() const { return safeAntiCheat; }
        int getDepthResolveMode() const { return settings.depthResolveMode; }
        const std::string& getDepthManualPin() const { return settings.depthManualPin; }
        bool getDepthTransientWorkaround() const { return settings.depthTransientWorkaround; }
        int getDepthCaptureMethod() const { return std::clamp(settings.depthCaptureMethod, 0, 2); }
        int getDepthSourceChannel() const { return std::clamp(settings.depthSourceChannel, 0, 6); }
        bool getDepthInvert() const { return settings.depthInvert; }

        // Setters (update in-memory state, call save() to persist)
        void setMaxEffects(int value) { settings.maxEffects = std::clamp(value, 1, 200); }
        void setOverlayBlockInput(bool value) { settings.overlayBlockInput = value; }
        void setToggleKey(const std::string& value) { settings.toggleKey = value; }
        void setReloadKey(const std::string& value) { settings.reloadKey = value; }
        void setOverlayKey(const std::string& value) { settings.overlayKey = value; }
        void setEnableOnLaunch(bool value) { settings.enableOnLaunch = value; }
        void setDepthCapture(bool value) { settings.depthCapture = value; }
        void setAutoApply(bool value) { settings.autoApply = value; }
        void setAutoApplyDelay(int value) { settings.autoApplyDelay = value; }
        void setShowDebugWindow(bool value) { settings.showDebugWindow = value; }
        void setSafeAntiCheat(bool value) { safeAntiCheat = value; }
        void setDepthResolveMode(int value) { settings.depthResolveMode = value; }
        void setDepthManualPin(const std::string& value) { settings.depthManualPin = value; }
        void setDepthTransientWorkaround(bool value) { settings.depthTransientWorkaround = value; }
        void setDepthCaptureMethod(int value) { settings.depthCaptureMethod = std::clamp(value, 0, 2); }
        void setDepthSourceChannel(int value) { settings.depthSourceChannel = std::clamp(value, 0, 6); }
        void setDepthInvert(bool value) { settings.depthInvert = value; }

        // Get raw settings struct (for bulk operations)
        const VkBasaltSettings& getSettings() const { return settings; }

    private:
        VkBasaltSettings settings;
        bool initialized = false;
        bool safeAntiCheat = false; // Runtime flag: layer hiding active
    };

    // Global settings manager instance (like effectRegistry)
    extern SettingsManager settingsManager;

} // namespace vkShade

#endif // SETTINGS_MANAGER_HPP_INCLUDED
