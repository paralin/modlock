#!/usr/bin/env bash
# Generate the typed entity classes of the TypeScript, Go and Luau mod libraries
# from the game dump in data/dump, which modlock-host --dump writes.
set -euo pipefail
cd "$(dirname "$0")/.."

go run ./cmd/modlock-entitygen -dump data/dump -ts js/src/entities.ts -go mod/entity/entity.go \
  -luau luau/modlock/entities.luau
