#!/usr/bin/env bash
# Build the files of one version into dist/: the Windows game server's archive
# and the command line for every platform, which embeds the built library.
# Usage: scripts/dist.sh VERSION, such as v0.1.9 or master-0123456789ab.
set -euo pipefail
version=${1:?name the version to build}
repo=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
cd "$repo"
mkdir -p dist

# The game server, as the command line downloads it.
scripts/proton-build.sh
rm -f dist/modlock-host-windows-x64.zip
(cd build-proton/sdk/bin && zip -r "$repo/dist/modlock-host-windows-x64.zip" .)

# The library and SDL3, which the Windows command line reads controllers
# through, then the command line that embeds them.
go run ./cmd/modlock-library
sdl3=3.4.20
sdl3_sha256=368bed8b24e44ad9ea372990814f2343fcfb9e468abb7f8f4a3bc146c7304752
scratch=$(mktemp -d)
trap 'rm -rf "$scratch"' EXIT
curl -fsSL -o "$scratch/sdl3.zip" \
  "https://github.com/libsdl-org/SDL/releases/download/release-$sdl3/SDL3-$sdl3-win32-x64.zip"
echo "$sdl3_sha256  $scratch/sdl3.zip" | shasum -a 256 -c -
unzip -o -j -q "$scratch/sdl3.zip" SDL3.dll -d controllers/built
for target in windows/amd64 linux/amd64 linux/arm64 darwin/amd64 darwin/arm64; do
  os=${target%/*}
  arch=${target#*/}
  name="modlock-$os-$arch"
  if [ "$os" = windows ]; then name="$name.exe"; fi
  CGO_ENABLED=0 GOOS=$os GOARCH=$arch go build -trimpath \
    -ldflags "-X main.Version=$version" -o "dist/$name" ./cmd/modlock
done
