// Command modlock-library builds the TypeScript library and the interface
// renderer from this checkout into js/built, which the command line built
// next embeds:
//
//	go run ./cmd/modlock-library
package main

import (
	"context"
	"fmt"
	"os"
	"path/filepath"

	"github.com/paralin/modlock/js"
)

func main() {
	if err := js.Build(context.Background(), ".", filepath.Join("js", js.Built)); err != nil {
		fmt.Fprintln(os.Stderr, err)
		os.Exit(1)
	}
}
