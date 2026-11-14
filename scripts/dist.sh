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

# The library, then the command line that embeds it.
go run ./cmd/modlock-library
for target in windows/amd64 linux/amd64 linux/arm64 darwin/amd64 darwin/arm64; do
  os=${target%/*}
  arch=${target#*/}
  name="modlock-$os-$arch"
  if [ "$os" = windows ]; then name="$name.exe"; fi
  CGO_ENABLED=0 GOOS=$os GOARCH=$arch go build -trimpath \
    -ldflags "-X main.Version=$version" -o "dist/$name" ./cmd/modlock
done
