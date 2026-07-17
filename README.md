# vkShade

[![License](https://img.shields.io/badge/license-zlib-green)](./LICENSE)
[![NixOS](https://img.shields.io/badge/NixOS-unstable-78C0E8?logo=nixos&logoColor=white)](https://nixos.org)

A Vulkan post-processing layer with an in-game ImGui overlay for real-time effect configuration. Works on both **X11** and **Wayland**.

This project also includes many additional compatibility fixes for various ReShade shaders.

**Use at your own risk**: unstable shaders or extreme GPU load can still crash or freeze games.

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

### Input Architecture

| Platform | Keyboard | Mouse | Input Blocking |
|----------|----------|-------|----------------|
| **X11** | `XQueryKeymap` via separate `Display*` | `XQueryPointer` | `XGrabPointer`/`XGrabKeyboard` |
| **Wayland** | `wl_keyboard` on private event queue + xkbcommon | `wl_pointer` on private event queue | `wl_proxy_add_listener` interposition |

On Wayland, the overlay creates its own `wl_pointer` and `wl_keyboard` on a separate `wl_event_queue`. When input blocking is enabled, the interposition layer wraps the game's pointer/keyboard listeners and suppresses events (except `leave`, `keymap`, `modifiers`, and `repeat_info` which are always forwarded to maintain correct game state).

### GPU Diagnostics

The diagnostics tab auto-detects your GPU vendor via PCI vendor ID and reads stats accordingly:

| Vendor | GPU Usage | VRAM | How |
|--------|-----------|------|-----|
| **AMD** | Direct (`gpu_busy_percent`) | `mem_info_vram_*` + GTT | sysfs (`amdgpu` driver) |
| **Intel** | Frequency ratio estimate | If available (Arc discrete) | sysfs (`i915`/`xe` driver) |
| **NVIDIA** | Direct (NVML) | Direct (NVML) | Runtime `dlopen("libnvidia-ml.so.1")` |

- **AMD**: Full support — GPU utilization, dedicated VRAM, GTT (shared memory for iGPUs)
- **Intel**: GPU frequency ratio (`gt_act_freq_mhz / gt_max_freq_mhz`) as a utilization estimate. VRAM is available on discrete Arc GPUs if the driver exposes it.
- **NVIDIA**: Uses NVML (NVIDIA Management Library) loaded at runtime via `dlopen`. No build-time dependency — if `libnvidia-ml.so.1` is not present, GPU stats are simply unavailable. Install `nvidia-utils` (or equivalent) for NVML support.
- **Unknown/unsupported**: FPS and frame time are always shown. GPU-specific stats gracefully degrade to "not available".

### Depth Buffer

The layer intercepts `vkCreateImage` to detect depth images and adds `VK_IMAGE_USAGE_SAMPLED_BIT` so shaders can sample them. ReShade effects with `semantic = "DEPTH"` textures receive the actual depth buffer, and the `bufready_depth` uniform correctly reports availability.

### Safe Anti-Cheat Mode

Per-profile toggle (`safeAntiCheat = true`) that:
- Forces `depthCapture = off` — no depth buffer binding
- Blocks shaders that use depth at runtime (hidden in Add Effects, shows tooltip explaining why)
- Auto-tests all shaders on first Add Effects open (one per frame, progress bar shown)

**Depth detection** uses SPIR-V call graph analysis: builds a per-function call graph from the compiled shader bytecode, then BFS from entry points to check if any reachable function loads the depth sampler. This correctly distinguishes shaders that merely *include* depth declarations (via `ReShade.fxh`) from those that actually *use* depth at runtime. Zero false positives, zero false negatives.

### ReShade Shader Support

Download shader packs and point the Shader Manager at them:
- https://github.com/crosire/reshade-shaders
- https://github.com/HelelSingh/CRT-Guest-ReShade
- https://github.com/kevinlekiller/reshade-steam-proton

Place shaders in `~/.config/vkShade/reshade/Shaders/` and textures in `~/.config/vkShade/reshade/Textures/`, or use the Shader Manager's browse feature to add directories.

## Installation

**Warning:** If you have other Vulkan post-processing layers installed, avoid enabling them at the same time as vkShade.

### Dependencies

- GCC >= 9
- Meson + Ninja
- Vulkan Headers
- SPIR-V Headers
- glslang (glslangValidator)
- X11 + Xi development files
- wayland-client + wayland-protocols + wayland-scanner
- libxkbcommon

**Runtime requirement:** vkShade targets **Vulkan API 1.3 / SPIR-V 1.3** and does not support older stacks. In practice, most modern **NVIDIA** and **AMD** Linux drivers should work unless they are roughly older than about a year.

### From Source

```bash
git clone https://github.com/<your-user>/vkShade.git
cd vkShade
meson setup --buildtype=release --prefix=/usr build-release
ninja -C build-release
sudo ninja -C build-release install
```

### NixOS

This project can be built with a nix-shell for development:

```bash
nix-shell -p meson ninja pkg-config gcc wayland wayland-protocols wayland-scanner \
  libxkbcommon glslang spirv-headers vulkan-headers vulkan-loader xorg.libX11 xorg.libXi \
  --run "meson setup --buildtype=debug builddir && ninja -C builddir"
```

## Usage

### Steam

Add to launch options:
```
vkshade-run %command%
```

Example with Proton optimizations and GameMode:
```
PROTON_ENABLE_WAYLAND=1 PROTON_USE_NTSYNC=1 DXVK_ASYNC=1 PROTON_FSR4_UPGRADE=1 vkshade-run gamemoderun %command%
```

### Lutris

1. Right-click game -> Configure
2. System options -> Environment variables
3. Set command prefix to `vkshade-run`

### Debug Logging

```bash
VKSHADE_LOG_LEVEL=debug vkshade-run ./game
```

Log levels: `trace`, `debug`, `info`, `warn`, `error`, `none`

To log to a file: `VKSHADE_LOG_FILE=/tmp/vkshade.log`

### GPU Crash Diagnostics (Single-GPU Friendly)

For deeper device-lost diagnostics without Nsight Shader Debugger multi-GPU setup:

```bash
VKSHADE_GPU_CRASH_DIAGNOSTICS=1 VKSHADE_LOG_LEVEL=debug vkshade-run ./game
```

When enabled, vkShade will try to use supported Vulkan diagnostics extensions (`VK_NV_device_diagnostic_checkpoints`, `VK_NV_device_diagnostics_config`, `VK_EXT_device_fault`) and emit extra logs on `VK_ERROR_DEVICE_LOST`.

This mode is fully opt-in and may add overhead. Keep it off for normal gameplay.

### Layer Naming

vkShade uses its own layer and environment names:
- Enable env: `ENABLE_VKSHADE=1`
- Layer name: `VK_LAYER_VKSHADE_post_processing`
- Layer JSON: `vkShade.json`

## Configuration

vkShade configuration lives in `~/.config/vkShade/`. All required config files and subfolders are generated on first run.

### Key Bindings

| Key | Default | Description |
|-----|---------|-------------|
| Toggle Effects | `End` | Enable/disable all effects |
| Reload Config | `F10` | Reload configuration and recompile shaders |
| Toggle Overlay | `Home` | Show/hide the overlay GUI |

### Settings File

`~/.config/vkShade/vkShade.conf`:

```ini
# Maximum effects (requires restart, 1-200)
# Higher values use more VRAM (~8 bytes x width x height per slot)
maxEffects = 10

# Key bindings
toggleKey = End
reloadKey = F10
overlayKey = Home

# Startup behavior
enableOnLaunch = true
depthCapture = false

# Overlay options
overlayBlockInput = false
autoApplyDelay = 200  # ms delay before auto-applying changes
```

### Per-Game Profiles

When a game is detected, vkShade creates a config directory at `~/.config/vkShade/configs/<game>/`. Multiple named profiles can be created per game from the overlay UI. The active profile is stored in `~/.config/vkShade/configs/<game>/active_profile`.

### Shader Manager

ReShade shader and texture paths are managed through the Shader Manager tab in the overlay. Add parent directories and the manager will recursively discover `Shaders/` and `Textures/` subdirectories. Paths are stored in `~/.config/vkShade/shader_manager.conf`.

## Anti-Cheat Safety

vkShade is a **read-only visual filter** — it applies post-processing shaders to the final rendered image, similar to NVIDIA Freestyle, AMD Adrenalin filters, or monitor-level color adjustments. It does **not**:

- Modify game memory or game files
- Read game state (player positions, health, etc.)
- Inject code into the game process
- Provide any competitive advantage (no wallhacks, aimbots, ESP)
- Intercept or modify network traffic

**Anti-cheat compatibility varies by game and platform:**

- **On Linux/Proton**, EAC and BattlEye have limited kernel-level access compared to Windows. Vulkan implicit layers like vkShade and MangoHud generally work because the anti-cheat cannot deeply inspect the Vulkan layer chain. No confirmed bans from comparable post-processing layer usage are known.
- **On Windows**, some games actively block ReShade and even NVIDIA Freestyle (e.g., Arc Raiders blocks both). vkShade falls in the same category as ReShade from an anti-cheat perspective — it is a third-party rendering layer, not a whitelisted vendor feature.
- **Per-game policies**: Anti-cheat detection is configured per-game by the developer. A game that allows vkShade today could block it tomorrow. The original vkBasalt FAQ says: *"Will vkBasalt get me banned? Maybe. To my knowledge this hasn't happened yet but don't blame me if your frog dies."*

**No guarantee can be made** — use at your own discretion. vkShade only applies visual post-processing and provides zero competitive advantage, but anti-cheat systems don't always distinguish between cosmetic and malicious modifications.

## Known Limitations

- Some ReShade shaders with multiple techniques may not work
- Depth buffer access works but is experimental (depends on game's depth format)
- Input blocking may cause issues in some games with custom input handling
- Intel GPU usage is estimated from frequency ratio (not direct utilization)
- NVIDIA stats require `libnvidia-ml.so.1` (install `nvidia-utils`)
- ReShade mouse/key uniforms (`mousepoint`, `mousedelta`, `mousebutton`, `key`) are not yet implemented

## Architecture

### Vulkan Layer

vkShade is a Vulkan implicit layer that intercepts API calls:
- `vkCreateSwapchainKHR` — creates intermediate images for effect processing
- `vkQueuePresentKHR` — applies effects before presentation
- `vkCreateImage` — detects depth images and adds `SAMPLED_BIT`
- `vkGetSwapchainImagesKHR` — returns wrapped images

Effects read from one image and write to another in a chain.

### Effect System

Built-in effects: CAS (sharpening), DLS (denoised luma sharpening), FXAA, SMAA, Deband, LUT.

ReShade FX effects are compiled at runtime using an embedded ReShade shader compiler. Parameters are managed through the `EffectRegistry` (single source of truth for all runtime parameter values).

### Wayland Interposition

Since `wl_pointer_add_listener` is a `static inline` function in `<wayland-client-protocol.h>`, we cannot interpose it directly. Instead, we interpose on the underlying C function `wl_proxy_add_listener` using symbol visibility and `dlsym(RTLD_NEXT)`. The interposed function:

1. Checks `wl_proxy_get_class()` to identify `wl_pointer` and `wl_keyboard` proxies
2. Skips overlay-owned proxies (registered via `registerOverlayProxy()`)
3. Wraps the game's listener with callbacks that check `isInputBlocked()` before forwarding

For Wayland, use `vkshade-run` so these interposition symbols are injected early enough in all loader scopes (including `RTLD_LOCAL` clients).

## Credits

- Original **vkBasalt** by [@DadSchoorse](https://github.com/DadSchoorse) (Georg Lehmann) — [zlib license](LICENSE)
- **vkShade Overlay** by [@Boux](https://github.com/Boux/vkBasalt_overlay) — ImGui overlay, effect management, shader manager
- **Wayland support implementation** by [@Daaboulex](https://github.com/Daaboulex/vkBasalt_overlay_wayland) — Wayland input, input blocking interposition, depth fixes
- **Additional ReShade shader compatibility fixes** by this project maintainer
- **ReShade** shader compiler by [@crosire](https://github.com/crosire)
- **ImGui** by [@ocornut](https://github.com/ocornut)
