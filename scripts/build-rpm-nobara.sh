#!/usr/bin/env bash
# Build and install a Sunshine RPM on Fedora/Nobara from this repository.
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$REPO_ROOT"

if ! command -v dnf >/dev/null 2>&1; then
  echo "This script must run on Fedora/Nobara (dnf required)." >&2
  exit 1
fi

git submodule update --init third-party/moonlight-common-c third-party/libvirtualhid

TRACKPAD_PATCH="$REPO_ROOT/packaging/linux/patches/moonlight-common-c-trackpad.patch"
if ! grep -q 'SS_TRACKPAD_MAGIC' "$REPO_ROOT/third-party/moonlight-common-c/src/Input.h"; then
  echo "Applying moonlight-common-c trackpad protocol patch"
  patch -d "$REPO_ROOT/third-party/moonlight-common-c" -p1 < "$TRACKPAD_PATCH"
fi

TRACKPAD_JUMP_PATCH="$REPO_ROOT/packaging/linux/patches/libvirtualhid-trackpad-jump.patch"
echo "Applying libvirtualhid trackpad jump patch"
git -C "$REPO_ROOT/third-party/libvirtualhid" checkout -- src/
patch -d "$REPO_ROOT/third-party/libvirtualhid" -p1 < "$TRACKPAD_JUMP_PATCH"
if ! grep -q 'trackpad_axis_resolution_x' "$REPO_ROOT/third-party/libvirtualhid/src/platform/linux/uhid_backend.cpp"; then
  echo "libvirtualhid trackpad jump patch failed to apply" >&2
  exit 1
fi

# Avoid upgrading system packages (e.g. dkms-nvidia) and skip CUDA runfile install.
./scripts/linux_build.sh --skip-package-update --skip-cuda

RPM_PATH="$(find "$REPO_ROOT/build/cpack_artifacts" -maxdepth 1 \( -name 'Sunshine.rpm' -o -name 'Sunshine-*.rpm' \) -print -quit)"
if [[ -z "$RPM_PATH" ]]; then
  RPM_PATH="$(find "$REPO_ROOT/build" -name '*.rpm' -type f -print -quit)"
fi
if [[ -z "$RPM_PATH" ]]; then
  echo "RPM not found under $REPO_ROOT/build/cpack_artifacts or $REPO_ROOT/build" >&2
  exit 1
fi

echo "Installing $RPM_PATH"
# Reinstall so rebuilt RPMs with the same version (0.0.0-dirty) replace the installed files.
sudo dnf reinstall -y "$RPM_PATH"

# Sunshine ships a user service, not a system unit. sunshine.service is only an alias;
# systemd refuses enable/restart on linked unit files.
SUNSHINE_UNIT="app-dev.lizardbyte.app.Sunshine.service"
systemctl --user daemon-reload
if systemctl --user cat "$SUNSHINE_UNIT" >/dev/null 2>&1; then
  if systemctl --user is-active --quiet "$SUNSHINE_UNIT"; then
    systemctl --user restart "$SUNSHINE_UNIT"
  elif systemctl --user is-enabled --quiet "$SUNSHINE_UNIT" 2>/dev/null; then
    systemctl --user start "$SUNSHINE_UNIT"
  else
    systemctl --user enable --now "$SUNSHINE_UNIT"
  fi
  systemctl --user status "$SUNSHINE_UNIT" --no-pager || true
else
  echo "No Sunshine user service unit found. Start manually with: sunshine" >&2
fi
