#!/usr/bin/env bash
# Generate the typed entity classes and console of the TypeScript, Go and Luau
# mod libraries from the game dump in data/dump, which modlock-host --dump
# writes.
set -euo pipefail
cd "$(dirname "$0")/.."

go run ./cmd/modlock-entitygen -dump data/dump -ts js/src -go mod -luau luau/modlock
