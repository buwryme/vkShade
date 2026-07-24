#!/bin/sh
# vkintox-run — launch a game with VKIntox overlay enabled.
# Sets ENABLE_VKINTOX=1 and LD_AUDIT for Wine Wayland input interposition.

if [ $# -eq 0 ] || [ "$1" = "--help" ] || [ "$1" = "-h" ]; then
  echo "Usage: vkintox-run <command...>"
  echo ""
  echo "Launch a game with VKIntox overlay enabled."
  echo "Sets ENABLE_VKINTOX=1 and LD_AUDIT for Wine Wayland input interposition."
  exit 0
fi

export ENABLE_VKINTOX=1

# LD_AUDIT for Wine Wayland: Wine loads winewayland.so via dlopen(RTLD_LOCAL),
# bypassing our wl_proxy_add_listener interposition. The audit library
# redirects symbol bindings in RTLD_LOCAL scopes to our wrapper.
audit_lib="@out@/lib/libvkintox-audit.so"
if [ -f "$audit_lib" ]; then
  export LD_AUDIT="${LD_AUDIT:+$LD_AUDIT:}$audit_lib"
fi

exec "$@"
