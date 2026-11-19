#!/usr/bin/env bash
# Build the JavaScript library from a Go consumer's vendor directory.
set -euo pipefail
repo=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
scratch=$(mktemp -d)
trap 'cd "$repo" && rm -r -- "$scratch"' EXIT
export GOWORK=off

# Carry the module's dependency pins and replacements into the consumer.
cp "$repo/go.mod" "$repo/go.sum" "$scratch/"
cd "$scratch"
module=github.com/paralin/modlock
go mod edit -module=example.com/library-consumer -require="$module@v0.0.0" -replace="$module=$repo"
cat > consumer.go <<'GO'
package main

import (
	"context"
	"log"
	"os"
	"path/filepath"

	"github.com/paralin/modlock/js"
)

func main() {
	dir, err := filepath.Abs("vendor/github.com/paralin/modlock")
	if err != nil {
		log.Fatal(err)
	}
	if source := js.Source(); source != dir {
		log.Fatalf("library source is %q, want %q", source, dir)
	}

	built, err := js.Library(context.Background())
	if err != nil {
		log.Fatal(err)
	}
	for _, name := range []string{"index.d.ts", js.Renderer, js.Preview + "/preview.js", js.Studio + "/studio.js"} {
		if _, err := os.Stat(filepath.Join(built, name)); err != nil {
			log.Fatal(err)
		}
	}
}
GO
go mod tidy
go mod vendor
go run -mod=vendor .
