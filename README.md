# vkShade

[![License](https://img.shields.io/badge/license-zlib-green)](./LICENSE)
[![NixOS](https://img.shields.io/badge/NixOS-unstable-78C0E8?logo=nixos&logoColor=white)](https://nixos.org)

A Vulkan post-processing layer with an in-game ImGui overlay for real-time effect configuration. Works on both **X11** and **Wayland**. Based off of vkShade, and adds:
- Parallel depth buffer tracking
- Automatic preference of depth buffers that match the current window size
- More resolve ways to handle depth buffers
- A nicer-ish UI
And all of these help form a post effects processing experience just like ReShade, but accessible to Linux.

**If depth-dependent effects look wrong, please go into Advanced and choose a depth buffer that matches your game window,**
**And make sure your depth resolve mode is set to Reverse-Z.**


**Use at your own risk**: unstable shaders or extreme GPU load can still crash or freeze games.
In theory, this wouldn't get you banned in most games since it's driver-level, but..
You still hold responsibility for moderation you get from using vkShade.

To set it up for Sober, run ./setup_sober.sh

## Features

The base project required editing config files and restarting. vkShade adds:

- **In-game overlay** (`Home` key) with dockable/undockable tab windows
- **Add/remove/reorder effects** without restart (drag to reorder)
- **Parameter sliders** for all types (float, int, uint, bool, vectors)
- **Preprocessor definitions** editor for ReShade `#define` values
- **Multiple effect instances** (e.g., cas, cas.1, cas.2)
- **Per-game profiles** with auto-detection and profile switching
- **Save/load named configs**
- **Shader manager** — browse directories, discover and load ReShade shaders
- **Diagnostics** — FPS, frame time, GPU/VRAM usage (AMD, Intel, NVIDIA)
- **Debug window** — effect state, log viewer, error display
- **Auto-apply** — changes apply after configurable delay
- **Up to 200 effects** with VRAM estimates
- **Safe Anti-Cheat mode** — per-profile toggle that blocks depth-using shaders and disables depth capture, keeping safe shaders like Vibrance usable
- **Shader test tool** — batch-tests all `.fx` shaders for compilation errors and depth usage
- **Graceful error handling** — failed effects show errors instead of crashing

### Additional Platform and Overlay Work

- **Wayland input blocking** — `wl_proxy_add_listener` interposition wraps game's pointer/keyboard listeners to suppress events when the overlay has focus
- **X11 input blocking** — `XGrabPointer`/`XGrabKeyboard` when overlay is active
- **Reliable Wayland mouse input** — time-based auto-release handles missing button releases from compositor grabs; motion-aware idle detection keeps buttons held during drags at any framerate
- **Game pointer mirroring** — interpose layer mirrors button state from the game's pointer to the overlay, ensuring reliable press/release tracking via Wayland implicit grab
- **Right-click context menus** on parameter sliders to reset to defaults
- **Depth buffer ready flag** — `bufready_depth` uniform now correctly reports whether depth is available to shaders

### Depth Buffer

The layer automatically picks a depth buffer that fits the game window, which in almost all cases works perfectly.s

### ReShade Shader Support

The setup_sober.sh script fetches all ReShade shaders and sets them up for you.

## Usage

The setup_sober.sh script:
- Fetches org.gnome.Sdk if necessary
- Compiles the project locally
- Installs it as a local Flatpak repo and Vulkan Layer extension
- Deploys shader manager configurations and all ReShade shaders
Please open an issue if anything goes wrong

### Key Bindings

| Key | Default | Description |
|-----|---------|-------------|
| Toggle Effects | `End` | Enable/disable all effects |
| Reload Config | `F10` | Reload configuration and recompile shaders |
| Toggle Overlay | `Home` | Show/hide the overlay GUI |
