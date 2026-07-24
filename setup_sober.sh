#!/usr/bin/env bash
set -euo pipefail

# ─── Colors ───────────────────────────────────────────────────────────────────
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
CYAN='\033[0;36m'
BOLD='\033[1m'
DIM='\033[2m'
NC='\033[0m'

# ─── Helper Functions ─────────────────────────────────────────────────────────
info()    { echo -e "${BLUE}  →${NC} $1"; }
success() { echo -e "${GREEN}  ✓${NC} $1"; }
warn()    { echo -e "${YELLOW}  !${NC} $1"; }
error()   { echo -e "${RED}  ✗${NC} $1"; }
step()    { echo -e "\n${CYAN}${BOLD}[$1]${NC}"; }

check_cmd() {
    if ! command -v "$1" &>/dev/null; then
        error "'$1' is required but not installed."
        echo -e "    ${DIM}Please install it with your package manager and try again.${NC}"
        exit 1
    fi
}

# ─── Header ───────────────────────────────────────────────────────────────────
echo -e "${BLUE}${BOLD}"
echo "  ╔═══════════════════════════════════════════════════╗"
echo "  ║           VKIntox Setup for Sober                ║"
echo "  ╚═══════════════════════════════════════════════════╝"
echo -e "${NC}"

# ─── Pre-flight Checks ───────────────────────────────────────────────────────
step "1/4 · Pre-flight checks"

info "Verifying required tools..."
check_cmd "flatpak"
check_cmd "just"
check_cmd "curl"
check_cmd "unzip"
check_cmd "python3"
success "All required tools are available"

info "Ensuring Flathub remote is configured..."
if flatpak remote-list | grep -q "^flathub"; then
    success "Flathub remote already exists"
else
    warn "Flathub remote not found — adding it now"
    flatpak remote-add --if-not-exists flathub https://flathub.org/repo/flathub.flatpakrepo
    success "Flathub remote added"
fi

# ─── Install GNOME SDK ───────────────────────────────────────────────────────
step "2/4 · Installing GNOME SDK"

info "Installing org.gnome.Sdk (latest)..."
info "${DIM}Answer 'y' if prompted about remotes or permissions${NC}"
echo ""

if flatpak install -y --user flathub runtime/org.gnome.Sdk/x86_64/50 2>&1; then
    success "GNOME SDK installed successfully"
else
    error "Failed to install GNOME SDK"
    exit 1
fi

# ─── Build VKIntox ───────────────────────────────────────────────────────────
step "3/4 · Building VKIntox"

info "Running flatpak-build via the Justfile..."
echo ""

if just flatpak-build; then
    success "Build completed successfully"
else
    error "Build failed — check the output above for details"
    exit 1
fi

# ─── Install ReShade Shaders ─────────────────────────────────────────────────
INI_URL="https://raw.githubusercontent.com/crosire/reshade-shaders/list/EffectPackages.ini"
BASE_TARGET_DIR="$HOME/.var/app/org.vinegarhq.Sober/config/VKIntox/reshade/"
TARGET_SHADERS="${BASE_TARGET_DIR}/Shaders"
TARGET_TEXTURES="${BASE_TARGET_DIR}/Textures"

step "4/4 · Installing ReShade shader packages"

# ─── Shader Paths (Sober Flatpak config) ─────────────────────────────────────
SOBER_CONFIG="$HOME/.var/app/org.vinegarhq.Sober/config/VKIntox"
RESHADE_DIR="$SOBER_CONFIG/reshade"
SHADERS_DIR="$RESHADE_DIR/Shaders"
TEXTURES_DIR="$RESHADE_DIR/Textures"
SHADER_MANAGER_CONF="$SOBER_CONFIG/shader_manager.conf"

# ─── Write shader_manager.conf ───────────────────────────────────────────────
info "Generating VKIntox configuration..."
mkdir -p "$(dirname "$SHADER_MANAGER_CONF")"
echo "parentDir = $RESHADE_DIR/" > "$SHADER_MANAGER_CONF"
success "Written $SHADER_MANAGER_CONF"

info "Creating shader directories..."
mkdir -p "$SHADERS_DIR" "$TEXTURES_DIR"
success "Created $SHADERS_DIR"
success "Created $TEXTURES_DIR"

# ─── Install Font ────────────────────────────────────────────────────
FONT_DIR="$SOBER_CONFIG/font"
mkdir -p "$FONT_DIR"

# Copy font from the Flatpak app's assets (if available) or skip
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
if [ -f "$SCRIPT_DIR/assets/font/GoogleSansFlex.ttf" ]; then
    cp "$SCRIPT_DIR/assets/font/GoogleSansFlex.ttf" "$FONT_DIR/"
    success "Installed GoogleSansFlex.ttf to $FONT_DIR"
fi
if [ -f "$SCRIPT_DIR/assets/font/LICENSE" ]; then
    cp "$SCRIPT_DIR/assets/font/LICENSE" "$FONT_DIR/"
    success "Installed font LICENSE to $FONT_DIR"
fi

# ─── Fetch & Install Shaders via Embedded Python ─────────────────────────────
WORK_DIR=$(mktemp -d)
trap 'rm -rf "${WORK_DIR}"' EXIT

info "Fetching latest package list from GitHub..."
INI_DATA=$(curl -sSL "${INI_URL}")

if [[ -z "${INI_DATA}" ]]; then
    error "Failed to fetch EffectPackages.ini"
    exit 1
fi
success "Package list downloaded"

info "Processing ReShade shader packages..."

python3 - "${WORK_DIR}" "${TARGET_SHADERS}" "${TARGET_TEXTURES}" "${INI_DATA}" <<'PYEOF'
import configparser
import os
import sys
import urllib.request
import zipfile
import shutil

work_dir = sys.argv[1]
target_shaders_base = sys.argv[2]
target_textures_base = sys.argv[3]
ini_text = sys.argv[4]

config = configparser.ConfigParser()
config.read_string(ini_text)

total_sections = len(config.sections())
processed = 0
failed = []

for section in config.sections():
    pkg = config[section]

    pkg_name = pkg.get("PackageName", section)
    download_url = pkg.get("DownloadUrl", None)

    if not download_url:
        continue

    processed += 1
    print(f"\n  [{processed}/{total_sections}] Processing: {pkg_name}")

    # parse relative paths
    raw_install_path = pkg.get("InstallPath", r".\reshade-shaders\Shaders")
    raw_texture_path = pkg.get("TextureInstallPath", r".\reshade-shaders\Textures")

    # normalize paths
    install_rel = raw_install_path.replace("\\", "/").strip("./").strip("/")
    texture_rel = raw_texture_path.replace("\\", "/").strip("./").strip("/")

    # subfolder mapping
    sub_shader = install_rel.replace("reshade-shaders/Shaders", "").strip("/")
    sub_texture = texture_rel.replace("reshade-shaders/Textures", "").strip("/")

    pkg_target_shaders = os.path.join(target_shaders_base, sub_shader)
    pkg_target_textures = os.path.join(target_textures_base, sub_texture)

    os.makedirs(pkg_target_shaders, exist_ok=True)
    os.makedirs(pkg_target_textures, exist_ok=True)

    # parse file filters
    effect_files = [f.strip() for f in pkg.get("EffectFiles", "").split(",") if f.strip()]
    deny_files = [f.strip() for f in pkg.get("DenyEffectFiles", "").split(",") if f.strip()]

    zip_path = os.path.join(work_dir, f"{section}.zip")
    extract_dir = os.path.join(work_dir, section)

    # download package
    try:
        req = urllib.request.Request(
            download_url,
            headers={'User-Agent': 'Mozilla/5.0'}
        )
        with urllib.request.urlopen(req) as resp, open(zip_path, 'wb') as out_file:
            shutil.copyfileobj(resp, out_file)
    except Exception as e:
        print(f"    Failed to download: {e}")
        failed.append(f"{pkg_name} (download)")
        continue

    # extract zip
    try:
        with zipfile.ZipFile(zip_path, 'r') as zip_ref:
            zip_ref.extractall(extract_dir)
    except Exception as e:
        print(f"    Failed to extract archive: {e}")
        failed.append(f"{pkg_name} (extract)")
        continue

    # scan extracted content and locate shaders/textures
    copied_shaders = 0
    copied_textures = 0

    for root, dirs, files in os.walk(extract_dir):
        for file in files:
            file_path = os.path.join(root, file)

            # handle .fx and .fxh files
            if file.endswith('.fx') or file.endswith('.fxh'):
                if deny_files and file in deny_files:
                    continue
                if effect_files and file not in effect_files and not file.endswith('.fxh'):
                    continue
                shutil.copy2(file_path, pkg_target_shaders)
                copied_shaders += 1

            # handle textures / images / header files embedded in textures
            elif any(file.endswith(ext) for ext in ['.png', '.jpg', '.jpeg', '.dds', '.tga', '.bmp']):
                shutil.copy2(file_path, pkg_target_textures)
                copied_textures += 1

    if copied_shaders == 0 and copied_textures == 0:
        print(f"    Warning: No matching files found in archive")
        failed.append(f"{pkg_name} (empty)")
    else:
        print(f"    OK ({copied_shaders} shaders, {copied_textures} textures)")

print()
if failed:
    print(f"[!] {len(failed)} package(s) failed:")
    for f in failed:
        print(f"    • {f}")
else:
    print("[✓] All shader packages processed successfully!")
PYEOF

# ─── Summary ──────────────────────────────────────────────────────────────────
shader_count=$(find "$SHADERS_DIR" -name "*.fx" 2>/dev/null | wc -l)
texture_count=$(find "$TEXTURES_DIR" -type f 2>/dev/null | wc -l)

success "Installed $shader_count shaders and $texture_count textures"

# ─── Configure Sober Override ────────────────────────────────────────────────
echo ""
info "Configuring Sober to enable VKIntox..."

if flatpak override --user org.vinegarhq.Sober --env=ENABLE_VKINTOX=1 2>/dev/null; then
    success "Override set: ENABLE_VKINTOX=1"
else
    warn "Could not set override — Sober may not be installed yet"
    warn "You can set it manually after installing Sober:"
    echo -e "    ${DIM}flatpak override --user org.vinegarhq.Sober --env=ENABLE_VKINTOX=1${NC}"
fi

# ─── Done ────────────────────────────────────────────────────────────────────
echo ""
echo -e "${GREEN}${BOLD}"
echo "  ╔═══════════════════════════════════════════════════╗"
echo "  ║              Setup Complete!                      ║"
echo "  ╚═══════════════════════════════════════════════════╝"
echo -e "${NC}"
echo -e "  ${DIM}VKIntox is now configured for Sober.${NC}"
echo -e "  ${DIM}Launch Sober to start using Vulkan shaders.${NC}"
echo ""
echo -e "  ${CYAN}Shaders:                ${NC} ${DIM}$SHADERS_DIR${NC}"
echo -e "  ${CYAN}Textures:               ${NC} ${DIM}$TEXTURES_DIR${NC}"
echo -e "  ${CYAN}Shader manager config:  ${NC} ${DIM}$SHADER_MANAGER_CONF${NC}"
echo ""
