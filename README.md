# VKIntox

[![License](https://img.shields.io/badge/license-zlib-green)](./LICENSE)
[![NixOS](https://img.shields.io/badge/NixOS-unstable-78C0E8?logo=nixos&logoColor=white)](https://nixos.org)

A Flatpak/Sober-first fork of **vkShade** which also adds:
- Parallel depth buffer tracking
- Automatic preference of depth buffers that match the current window size
- More resolve ways to handle depth buffers
- A nicer-ish UI

> Compiling and using for native apps or Flatpak apps outside of Sober is still possible, of course... But this project has been made with Sober in mind

These help form a post effects processing experience just like ReShade, but accessible to Linux.

**If depth-dependent effects look wrong, please open the Advanced tab and try changing between depth resolve modes, which after each one, you press F10 (or your set keybind) to reload.

## ⚠️ Please open an issue if **anything** goes wrong, as long as it is indefinitely **vkShade**'s fault.

To set it up for Sober, run `./setup_sober.sh`

## Disclaimer

**Use at your own risk**: 
- unstable shaders or extreme GPU load can still crash or freeze games.
- VKIntox is driver-level but can still get you moderated
- this is a very experimental project that has yet to improve.

The codebase needs a lot of work and removage of extra unneeded features slobodaapl/**vkShade** bundles in...

## Features

The base project required editing config files and restarting. VKIntox adds:

- **In-game overlay** (`Home` key) with dockable/undockable tab windows and all management
- **Multiple effect instances** (e.g., cas, cas.1, cas.2)
- **Per-game profiles** with auto-detection and profile switching
- **Save/load named configs**
- **Shader manager** — browse directories, discover and load ReShade shaders (automatically setup by the script)

### Depth Buffer

The layer automatically picks a depth buffer that fits the game window, which in almost all cases works perfectly.
If misconfigured, for Roblox, the depth resolve mode should be Reverse-Z and inversion should be ON.

### ReShade Shader Support

The `setup_sober.sh` script fetches all ReShade shaders and sets them up for you.

**Requirements for shader installation:** `python3`, `curl`, `unzip` (all checked by the setup script).

## Usage

The `setup_sober.sh` script:
- Fetches org.gnome.Sdk if necessary
- Compiles the project locally
- Installs it as a local Flatpak repo and Vulkan Layer extension
- Deploys shader manager configurations and all ReShade shaders

### Key Bindings

| Key | Default | Description |
|-----|---------|-------------|
| Toggle Effects | `End` | Enable/disable all effects |
| Reload Config | `F10` | Reload configuration and recompile shaders |
| Toggle Overlay | `Home` | Show/hide the overlay GUI |

### Known Issues/Info

- Occasional crashes can happen; to minimise them, make sure your game is stabilised (e.g. in the catalog, everything, graphically speaking, finished loading in) before enabling the effects in the overlay.
- Startup takes long due to deferred reboot workaround
- The installation script isn't as efficient as it could be

### Special Thanks To
slobodaapl, for making **vkShade** which is the direct source code reprise of this project

DadSchoorse, for making vkBasalt, which the original **vkShade** is based on
