#!/usr/bin/env bash
# Build and install a Sunshine RPM on Fedora/Nobara from this repository.
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$REPO_ROOT"

if ! command -v dnf >/dev/null 2>&1; then
  echo "This script must run on Fedora/Nobara (dnf required)." >&2
  exit 1
fi

git submodule update --init third-party/moonlight-common-c

TRACKPAD_PATCH="$REPO_ROOT/packaging/linux/patches/moonlight-common-c-trackpad.patch"
if ! grep -q 'SS_TRACKPAD_MAGIC' "$REPO_ROOT/third-party/moonlight-common-c/src/Input.h"; then
  echo "Applying moonlight-common-c trackpad protocol patch"
  patch -d "$REPO_ROOT/third-party/moonlight-common-c" -p1 < "$TRACKPAD_PATCH"
fi

./scripts/linux_build.sh --step=deps,cmake,build,package

RPM_PATH="$(find "$REPO_ROOT/build" -maxdepth 1 -name 'Sunshine-*.rpm' -print -quit)"
if [[ -z "$RPM_PATH" ]]; then
  echo "RPM not found under $REPO_ROOT/build" >&2
  exit 1
fi

echo "Installing $RPM_PATH"
sudo dnf install -y "$RPM_PATH"
sudo systemctl restart sunshine || true
systemctl status sunshine --no-pager || true
