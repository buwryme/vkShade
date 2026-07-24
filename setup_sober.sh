#!/usr/bin/env bash
set -e

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
echo "  ║           vkShade Setup for Sober                ║"
echo "  ╚═══════════════════════════════════════════════════╝"
echo -e "${NC}"

# ─── Pre-flight Checks ───────────────────────────────────────────────────────
step "1/4 · Pre-flight checks"

info "Verifying required tools..."
check_cmd "flatpak"
check_cmd "just"
check_cmd "curl"
check_cmd "unzip"
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

# ─── Build vkShade ───────────────────────────────────────────────────────────
step "3/4 · Building vkShade"

info "Running flatpak-build via the Justfile..."
echo ""

if just flatpak-build; then
    success "Build completed successfully"
else
    error "Build failed — check the output above for details"
    exit 1
fi

# ─── Install ReShade Shaders ─────────────────────────────────────────────────
SHADER_LIST_URL="https://raw.githubusercontent.com/crosire/reshade-shaders/list/EffectPackages.ini"
TEMP_DIR=$(mktemp -d)
FAILED_PACKAGES=()

step "4/4 · Installing ReShade shader packages"

# ─── Shader Paths (Sober Flatpak config) ─────────────────────────────────────
SOBER_CONFIG="$HOME/.var/app/org.vinegarhq.Sober/config/vkShade"
RESHADE_DIR="$SOBER_CONFIG/reshade"
SHADERS_DIR="$RESHADE_DIR/Shaders"
TEXTURES_DIR="$RESHADE_DIR/Textures"
SHADER_MANAGER_CONF="$SOBER_CONFIG/shader_manager.conf"

# ─── Write shader_manager.conf ───────────────────────────────────────────────
info "Generating vkShade configuration..."
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

info "Fetching shader package list..."
if ! curl -sL --fail "$SHADER_LIST_URL" -o "$TEMP_DIR/packages.ini"; then
    error "Failed to fetch shader package list"
    rm -rf "$TEMP_DIR"
    exit 1
fi
success "Package list downloaded"

total_packages=$(grep -c '^\[' "$TEMP_DIR/packages.ini")
info "Found $total_packages shader packages to install"
echo ""

# ── Package installer function ────────────────────────────────────────────────
install_shader_package() {
    local url="$1"
    local name="$2"
    local win_shader_path="$3"
    local win_texture_path="$4"
    local idx="$5"
    local total="$6"

    printf "  ${DIM}[%2d/%d] %-52s${NC}" "$idx" "$total" "$name"

    # Convert Windows paths (.\reshade-shaders\Shaders\SubDir) to target paths
    local target_shader_dir=""
    local target_texture_dir=""

    if [[ -n "$win_shader_path" ]]; then
        local rel="${win_shader_path#\\.\\reshade-shaders\\}"
        rel="${rel//\\//}"
        if [[ "$rel" == "Shaders" ]]; then
            target_shader_dir="$SHADERS_DIR"
        elif [[ "$rel" == Shaders/* ]]; then
            target_shader_dir="$SHADERS_DIR/${rel#Shaders/}"
        fi
    fi

    if [[ -n "$win_texture_path" ]]; then
        local rel="${win_texture_path#\\.\\reshade-shaders\\}"
        rel="${rel//\\//}"
        if [[ "$rel" == "Textures" ]]; then
            target_texture_dir="$TEXTURES_DIR"
        elif [[ "$rel" == Textures/* ]]; then
            target_texture_dir="$TEXTURES_DIR/${rel#Textures/}"
        fi
    fi

    # handle when something goes wrong
    if [[ -z "$target_shader_dir" && -z "$target_texture_dir" ]]; then
        printf "\r  ${YELLOW}[%2d/%d] %-52s SKIPPED (unmatched path: %s)${NC}\n" \
            "$idx" "$total" "$name" "${win_shader_path:-$win_texture_path}"
        FAILED_PACKAGES+=("$name (unmatched install path)")
        return 1
    fi

    local pkg_temp="$TEMP_DIR/pkg_${idx}_$RANDOM"
    mkdir -p "$pkg_temp"

    # Download — --fail makes curl return nonzero on HTTP 4xx/5xx instead of
    # silently saving an error page as the "archive"
    if ! curl -sL --fail --max-time 120 "$url" -o "$pkg_temp/archive.zip" 2>/dev/null; then
        printf "\r  ${RED}[%2d/%d] %-52s FAILED (download)${NC}\n" "$idx" "$total" "$name"
        FAILED_PACKAGES+=("$name (download failed)")
        rm -rf "$pkg_temp"
        return 1
    fi

    # Validate zip
    if ! unzip -t "$pkg_temp/archive.zip" &>/dev/null; then
        printf "\r  ${RED}[%2d/%d] %-52s FAILED (invalid zip)${NC}\n" "$idx" "$total" "$name"
        FAILED_PACKAGES+=("$name (invalid zip)")
        rm -rf "$pkg_temp"
        return 1
    fi

    # Extract
    if ! unzip -qo "$pkg_temp/archive.zip" -d "$pkg_temp/src" 2>/dev/null; then
        printf "\r  ${RED}[%2d/%d] %-52s FAILED (extract)${NC}\n" "$idx" "$total" "$name"
        FAILED_PACKAGES+=("$name (extract failed)")
        rm -rf "$pkg_temp"
        return 1
    fi

    # Copy shaders (.fx files) — count what actually got copied
    local copied_shaders=0
    local copied_textures=0

    if [[ -n "$target_shader_dir" ]]; then
        mkdir -p "$target_shader_dir"
        copied_shaders=$(find "$pkg_temp/src" -name "*.fx" -type f -exec cp -f {} "$target_shader_dir/" \; -print 2>/dev/null | wc -l)
    fi

    # Copy textures
    if [[ -n "$target_texture_dir" ]]; then
        mkdir -p "$target_texture_dir"
        copied_textures=$(find "$pkg_temp/src" -type f \( \
            -name "*.png" -o -name "*.jpg" -o -name "*.jpeg" -o \
            -name "*.dds" -o -name "*.tga" -o -name "*.bmp" -o \
            -name "*.hdr" -o -name "*.exr" \) \
            -exec cp -f {} "$target_texture_dir/" \; -print 2>/dev/null | wc -l)
    fi

    rm -rf "$pkg_temp"

    # If we had a target dir but copied nothing, that's a silent failure — surface it
    if [[ "$copied_shaders" -eq 0 && "$copied_textures" -eq 0 ]]; then
        printf "\r  ${YELLOW}[%2d/%d] %-52s WARN (0 files found in archive)${NC}\n" "$idx" "$total" "$name"
        FAILED_PACKAGES+=("$name (archive contained no matching files)")
        return 1
    fi

    printf "\r  ${GREEN}[%2d/%d] %-52s OK (%d shaders, %d textures)${NC}\n" \
        "$idx" "$total" "$name" "$copied_shaders" "$copied_textures"
    return 0
}

# ── Parse INI and install all packages ────────────────────────────────────────
current_idx=0
current_url=""
current_name=""
current_shader_path=""
current_texture_path=""

while IFS= read -r line; do
    if [[ "$line" =~ ^\[[0-9]+\]$ ]]; then
        if [[ -n "$current_url" && -n "$current_name" ]]; then
            ((current_idx++)) || true
            install_shader_package \
                "$current_url" \
                "$current_name" \
                "$current_shader_path" \
                "$current_texture_path" \
                "$current_idx" \
                "$total_packages" || true
        fi
        current_url=""
        current_name=""
        current_shader_path=""
        current_texture_path=""
    elif [[ "$line" =~ ^PackageName=(.*)$ ]]; then
        current_name="${BASH_REMATCH[1]}"
    elif [[ "$line" =~ ^DownloadUrl=(.*)$ ]]; then
        current_url="${BASH_REMATCH[1]}"
    elif [[ "$line" =~ ^InstallPath=(.*)$ ]]; then
        current_shader_path="${BASH_REMATCH[1]}"
    elif [[ "$line" =~ ^TextureInstallPath=(.*)$ ]]; then
        current_texture_path="${BASH_REMATCH[1]}"
    fi
done < "$TEMP_DIR/packages.ini"

# Process last package
if [[ -n "$current_url" && -n "$current_name" ]]; then
    ((current_idx++)) || true
    install_shader_package \
        "$current_url" \
        "$current_name" \
        "$current_shader_path" \
        "$current_texture_path" \
        "$current_idx" \
        "$total_packages" || true
fi

# Summary
echo ""
shader_count=$(find "$SHADERS_DIR" -name "*.fx" 2>/dev/null | wc -l)
texture_count=$(find "$TEXTURES_DIR" -type f 2>/dev/null | wc -l)

if [[ ${#FAILED_PACKAGES[@]} -eq 0 ]]; then
    success "All $total_packages shader packages installed"
else
    warn "${#FAILED_PACKAGES[@]} of $total_packages package(s) failed:"
    for pkg in "${FAILED_PACKAGES[@]}"; do
        echo -e "    ${DIM}• $pkg${NC}"
    done
fi
info "Installed $shader_count shaders and $texture_count textures"

rm -rf "$TEMP_DIR"

# ─── Configure Sober Override ────────────────────────────────────────────────
echo ""
info "Configuring Sober to enable vkShade..."

if flatpak override --user org.vinegarhq.Sober --env=ENABLE_VKSHADE=1 2>/dev/null; then
    success "Override set: ENABLE_VKSHADE=1"
else
    warn "Could not set override — Sober may not be installed yet"
    warn "You can set it manually after installing Sober:"
    echo -e "    ${DIM}flatpak override --user org.vinegarhq.Sober --env=ENABLE_VKSHADE=1${NC}"
fi

# ─── Done ────────────────────────────────────────────────────────────────────
echo ""
echo -e "${GREEN}${BOLD}"
echo "  ╔═══════════════════════════════════════════════════╗"
echo "  ║              Setup Complete!                      ║"
echo "  ╚═══════════════════════════════════════════════════╝"
echo -e "${NC}"
echo -e "  ${DIM}vkShade is now configured for Sober.${NC}"
echo -e "  ${DIM}Launch Sober to start using Vulkan shaders.${NC}"
echo ""
echo -e "  ${CYAN}Shaders:                ${NC} ${DIM}$SHADERS_DIR${NC}"
echo -e "  ${CYAN}Textures:               ${NC} ${DIM}$TEXTURES_DIR${NC}"
echo -e "  ${CYAN}Shader manager config:  ${NC} ${DIM}$SHADER_MANAGER_CONF${NC}"
echo ""
