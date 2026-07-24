set shell := ["sh", "-eu", "-c"]

repo_root := justfile_directory()
build_dir := repo_root / "build"
lib_dir := build_dir / "src"
layer_dir := build_dir / "config"
dev_layer_dir := "/tmp/vkintox-dev-layer"
dev_layer_manifest := dev_layer_dir / "VKIntox.json"
layer_name := "VK_LAYER_VKINTOX_post_processing"
dev_layer_name := "VK_LAYER_VKINTOX_post_processing_dev"
default_log := "/tmp/vkintox.log"

default:
    @just --list

flatpak_manifest := repo_root / "org.freedesktop.Platform.VulkanLayer.VKIntox.json"
flatpak_build_dir := repo_root / ".flatpak-build"
flatpak_repo_dir := repo_root / ".flatpak-repo"

flatpak-build:
    flatpak-builder \
      --force-clean \
      --repo={{ flatpak_repo_dir }} \
      {{ flatpak_build_dir }} \
      {{ flatpak_manifest }}
    flatpak remote-delete --user vkintox-local || true
    flatpak remote-add --user vkintox-local {{ flatpak_repo_dir }} --no-gpg-verify
    flatpak remote-modify --user --no-gpg-verify vkintox-local
    flatpak install --user -y vkintox-local org.freedesktop.Platform.VulkanLayer.VKIntox

nsight_dir := "/opt/nsight-graphics/NVIDIA-Nsight-Graphics-2026.1/host/linux-desktop-nomad-x64"
nsight_capture_bin := nsight_dir / "ngfx-capture.bin"
nsight_capture_dir := "/tmp/nsight-captures"

build:
    meson compile -C {{ build_dir }}

prepare-layer:
    mkdir -p {{ dev_layer_dir }}
    sed \
      -e 's#"name": "{{ layer_name }}"#"name": "{{ dev_layer_name }}"#' \
      -e 's#"/usr/local/lib/libvkintox.so"#"{{ lib_dir / "libvkintox.so" }}"#' \
      {{ layer_dir }}/VKIntox.json > {{ dev_layer_manifest }}

run +args: prepare-layer
    env \
      VK_ADD_LAYER_PATH={{ dev_layer_dir }} \
      VK_INSTANCE_LAYERS={{ dev_layer_name }} \
      LD_LIBRARY_PATH={{ lib_dir }}${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH} \
      ENABLE_VKINTOX=${ENABLE_VKINTOX:-1} \
      {{ args }}

run-debug log +args: prepare-layer
    env \
      VK_ADD_LAYER_PATH={{ dev_layer_dir }} \
      VK_INSTANCE_LAYERS={{ dev_layer_name }} \
      LD_LIBRARY_PATH={{ lib_dir }}${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH} \
      ENABLE_VKINTOX=${ENABLE_VKINTOX:-1} \
      VKINTOX_LOG_LEVEL=debug \
      VKINTOX_LOG_FILE={{ log }} \
      {{ args }}

vkcube +args:
    just run vkdcube {{ args }}

vkcube-debug +args:
    just run-debug {{ default_log }} vkdcube {{ args }}

vkcube-debug-log log +args:
    just run-debug {{ log }} vkdcube {{ args }}

prime-run +args: prepare-layer
    #!/usr/bin/env bash
    set -euo pipefail
    prime-run env \
      VK_ADD_LAYER_PATH={{ dev_layer_dir }} \
      VK_INSTANCE_LAYERS={{ dev_layer_name }} \
      LD_LIBRARY_PATH={{ lib_dir }}${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH} \
      ENABLE_VKINTOX=${ENABLE_VKINTOX:-1} \
      {{ args }}

prime-run-debug log +args: prepare-layer
    #!/usr/bin/env bash
    set -euo pipefail
    prime-run env \
      VK_ADD_LAYER_PATH={{ dev_layer_dir }} \
      VK_INSTANCE_LAYERS={{ dev_layer_name }} \
      LD_LIBRARY_PATH={{ lib_dir }}${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH} \
      ENABLE_VKINTOX=${ENABLE_VKINTOX:-1} \
      VKINTOX_LOG_LEVEL=debug \
      VKINTOX_LOG_FILE={{ log }} \
      {{ args }}

vkdcube-prime +args:
    just prime-run vkdcube {{ args }}

vkdcube-prime-debug +args:
    just prime-run-debug {{ default_log }} vkdcube {{ args }}

vkdcube-prime-debug-log log +args:
    just prime-run-debug {{ log }} vkdcube {{ args }}

vkcube-capture shot log delay='3':
    rm -f {{ shot }} {{ log }} /tmp/vkintox-vkcube-capture.out
    just vkcube-debug-log {{ log }} >/tmp/vkintox-vkcube-capture.out 2>&1 & pid=$!;       sleep {{ delay }};       spectacle -b -n -o {{ shot }};       kill -INT $pid;       wait $pid || true;       ls -l {{ shot }};       echo ---LOG---;       tail -n 60 {{ log }} || true

vkcube-capture-config config shot log delay='3':
    rm -f {{ shot }} {{ log }} /tmp/vkintox-vkcube-capture.out
    VKINTOX_CONFIG_FILE={{ config }} just vkcube-debug-log {{ log }} >/tmp/vkintox-vkcube-capture.out 2>&1 & pid=$!;       sleep {{ delay }};       spectacle -b -n -o {{ shot }};       kill -INT $pid;       wait $pid || true;       ls -l {{ shot }};       echo ---LOG---;       tail -n 60 {{ log }} || true

vkcube-capture-default:
    just vkcube-capture /tmp/vkintox-shot.png /tmp/vkintox.log 3

nsight-vkdcube-capture frame='10' count='1' capture_dir=nsight_capture_dir: prepare-layer
    #!/usr/bin/env bash
    set -euo pipefail
    mkdir -p {{ capture_dir }}
    prime-run env -u LD_LIBRARY_PATH \
      {{ nsight_capture_bin }} \
      -e "$(command -v vkdcube)" \
      --env=VK_ADD_LAYER_PATH={{ dev_layer_dir }} \
      --env=VK_INSTANCE_LAYERS={{ dev_layer_name }} \
      --env=ENABLE_VKINTOX=1 \
      --env=VKINTOX_USE_LD_AUDIT=0 \
      --env=GDK_PIXBUF_MODULEDIR=/usr/lib/gdk-pixbuf-2.0/2.10.0/loaders \
      --env=GDK_PIXBUF_MODULE_FILE=/usr/lib/gdk-pixbuf-2.0/2.10.0/loaders.cache \
      --env=XDG_DATA_DIRS=/usr/local/share:/usr/share \
      --working-dir {{ repo_root }} \
      --output-dir {{ capture_dir }} \
      --capture-frame {{ frame }} \
      -n {{ count }} \
      --terminate-after-capture \
      --diagnostic-mode

nsight-vkdcube-capture-hotkey count='1' capture_dir=nsight_capture_dir: prepare-layer
    #!/usr/bin/env bash
    set -euo pipefail
    mkdir -p {{ capture_dir }}
    prime-run env -u LD_LIBRARY_PATH \
      {{ nsight_capture_bin }} \
      -e "$(command -v vkdcube)" \
      --env=VK_ADD_LAYER_PATH={{ dev_layer_dir }} \
      --env=VK_INSTANCE_LAYERS={{ dev_layer_name }} \
      --env=ENABLE_VKINTOX=1 \
      --env=VKINTOX_USE_LD_AUDIT=0 \
      --env=GDK_PIXBUF_MODULEDIR=/usr/lib/gdk-pixbuf-2.0/2.10.0/loaders \
      --env=GDK_PIXBUF_MODULE_FILE=/usr/lib/gdk-pixbuf-2.0/2.10.0/loaders.cache \
      --env=XDG_DATA_DIRS=/usr/local/share:/usr/share \
      --working-dir {{ repo_root }} \
      --output-dir {{ capture_dir }} \
      --capture-hotkey \
      -n {{ count }} \
      --terminate-after-capture \
      --diagnostic-mode
