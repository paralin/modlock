#!/usr/bin/env bash
# Generate the typed entity classes and console of the TypeScript, Go and Luau
# mod libraries from the game dump in data/dump: the documents modlock-host
# --dump writes and survey.json, which records what creating each designer name
# did.
set -euo pipefail
cd "$(dirname "$0")/.."

go run ./cmd/modlock-entitygen -dump data/dump -ts js/src -go mod -luau luau/modlock
