#!/usr/bin/env bash
# Generate Go/TypeScript protocols and C++ messages exported by the SDK DLL,
# then the mod libraries' calls from them.
#
# protoc runs in-process through go-protoc-wasi (no native protoc build). The
# WASI protoc cannot pass --cpp_out=dllexport_decl, so restore-proto-exports.py
# re-applies the MODLOCK_API markers after generation.
#
# The generator reads only proto files Git tracks, so stage a new one first
# (git add -N). When a new Go package is imported before it exists, go mod
# vendor fails; a file holding only its package clause lets the first run
# through.
set -euo pipefail
cd "$(dirname "$0")/.."

if [[ -e vendor ]]; then
  echo 'Generation needs an unvendored checkout; retain or relocate vendor first.' >&2
  exit 1
fi

GOFLAGS=-mod=mod go mod vendor
trap 'rm -rf vendor' EXIT

# The generator formats TypeScript with any oxfmt on PATH, and bun run puts
# node_modules/.bin there, but generated messages stay as protoc writes them.
PATH=$(tr ':' '\n' <<<"$PATH" | grep -v '/node_modules/\.bin$' | paste -sd:)

GOFLAGS=-mod=mod go run -mod=mod -tags=purego github.com/aperturerobotics/common/cmd/aptre generate \
  --language cpp --language go --language ts \
  --rpc none --targets './proto/modlock/*.proto' --targets './proto/modlock/control/*.proto' "$@"

# The publishing messages and the command line's events serve the command line
# and the programs that read its --json output; the spot document serves the
# mod libraries.
GOFLAGS=-mod=mod go run -mod=mod -tags=purego github.com/aperturerobotics/common/cmd/aptre generate \
  --language go --language ts --rpc none \
  --targets './proto/modlock/publish/*.proto' --targets './proto/modlock/cli/*.proto' \
  --targets './proto/modlock/spot/*.proto'

# The game dump serves the host that writes it and the generators that read it.
GOFLAGS=-mod=mod go run -mod=mod -tags=purego github.com/aperturerobotics/common/cmd/aptre generate \
  --language cpp --language go --rpc none --targets './proto/modlock/dump/*.proto'

python3 scripts/restore-proto-exports.py

# Each language's mod library and the host's dispatch follow wasm.proto.
GOFLAGS=-mod=mod go run ./cmd/modlock-sdkgen
clang-format -i src/wasm/host_service.gen.h

GOFLAGS=-mod=mod go run ./cmd/proto-export
