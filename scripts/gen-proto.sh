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

aptre() {
  GOFLAGS=-mod=mod go run -mod=mod -tags=purego github.com/aperturerobotics/common/cmd/aptre generate "$@"
}

# The mod boundary, its geometry and the host's control stream serve the host
# in C++ and the tools and mod libraries in Go and TypeScript.
aptre --rpc none --language cpp --language go --language ts \
  --targets './src/geometry/*.proto' --targets './src/wasm/*.proto' --targets './src/wasm/*/*.proto' \
  --targets './src/host_app/control/*.proto' "$@"

# The game's own messages, camera paths, signatures and hero builds serve only the host.
aptre --rpc none --language cpp \
  --targets './src/camera/*.proto' --targets './src/gameinterop/engine/*.proto' \
  --targets './src/gameinterop/signatures/*.proto' --targets './src/gamedata/*.proto'

# The publishing messages and the command line's events serve the command line
# and the programs that read its --json output; the spot document serves the
# mod libraries; the Studio messages serve its server and page.
aptre --rpc none --language go --language ts \
  --targets './publish/*.proto' --targets './cli/*.proto' --targets './mod/spot/*.proto' \
  --targets './studio/*.proto'

# The daemon's service serves the modlock processes on one PC.
aptre --rpc starpc --language go --targets './daemon/*.proto'

# The peer's service serves the modlock processes on one PC; its relay
# credentials mirror the answer of the service that issues them.
aptre --rpc starpc --language go --targets './peernet/*.proto'

# The game dump serves the host that writes it and the generators that read it.
aptre --rpc none --language cpp --language go --targets './src/host_app/dump/*.proto'

python3 scripts/restore-proto-exports.py

# Each language's mod library and the host's dispatch follow src/wasm/wasm.proto.
GOFLAGS=-mod=mod go run -tags=purego ./cmd/modlock-sdkgen
clang-format -i src/wasm/host_service.gen.h

GOFLAGS=-mod=mod go run -tags=purego ./cmd/proto-export
