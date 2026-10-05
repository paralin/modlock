#!/usr/bin/env bash
# Generate the typed entity classes of the TypeScript and Go mod libraries
# from a DumpSource2 schemas directory, such as the one GameTracking-Deadlock
# publishes.
set -euo pipefail
if [ $# -ne 1 ]; then
  echo "usage: $0 DUMPSOURCE2_SCHEMAS_DIR" >&2
  exit 2
fi
schemas="$(cd "$1" && pwd)"
cd "$(dirname "$0")/.."

go run ./cmd/modlock-entitygen -schemas "$schemas" -ts js/src/entities.ts -go mod/entity/entity.go
