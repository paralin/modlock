#!/bin/sh
# Installs the modlock command line for this computer from the latest release.
#
#   curl -fsSL https://raw.githubusercontent.com/paralin/modlock/master/install.sh | sh
#
# MODLOCK_INSTALL_DIR chooses the directory, which defaults to ~/.local/bin so
# that `modlock update` can replace the file later without sudo.
# MODLOCK_VERSION installs one release, such as v0.1.78, instead of the latest.
set -eu

case "$(uname -s)" in
  Darwin) os=darwin ;;
  Linux) os=linux ;;
  *)
    echo "modlock: this script installs on macOS and Linux; on Windows run install.ps1 in PowerShell" >&2
    exit 1
    ;;
esac

case "$(uname -m)" in
  x86_64 | amd64) arch=amd64 ;;
  arm64 | aarch64) arch=arm64 ;;
  *)
    echo "modlock: no release for $(uname -m) processors" >&2
    exit 1
    ;;
esac

releases=https://github.com/paralin/modlock/releases
if [ -n "${MODLOCK_VERSION:-}" ]; then
  url="$releases/download/$MODLOCK_VERSION/modlock-$os-$arch"
else
  url="$releases/latest/download/modlock-$os-$arch"
fi

dir=${MODLOCK_INSTALL_DIR:-$HOME/.local/bin}
mkdir -p "$dir"

# Download beside the destination and rename into place, so an interrupted
# download leaves any installed modlock working.
staged="$dir/.modlock.install"
trap 'rm -f "$staged"' EXIT
echo "Downloading $url"
curl -fL --progress-bar -o "$staged" "$url"
chmod 755 "$staged"
mv "$staged" "$dir/modlock"

echo "Installed $("$dir/modlock" --version) at $dir/modlock"

case ":$PATH:" in
  *":$dir:"*)
    echo "Start a mod with: modlock new my-mod"
    ;;
  *)
    echo "$dir is not on your PATH. Add this line to your shell profile, then open a new terminal:"
    echo "  export PATH=\"$dir:\$PATH\""
    ;;
esac
