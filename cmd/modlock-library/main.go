// Command modlock-library builds the TypeScript library and the interface
// renderer from this checkout into the archive a release publishes:
//
//	modlock-library dist/modlock-library.tar.gz
package main

import (
	"context"
	"fmt"
	"os"

	"github.com/paralin/modlock/js"
)

func main() {
	if len(os.Args) != 2 {
		fmt.Fprintln(os.Stderr, "usage: modlock-library <archive>")
		os.Exit(2)
	}
	if err := js.Pack(context.Background(), ".", os.Args[1]); err != nil {
		fmt.Fprintln(os.Stderr, err)
		os.Exit(1)
	}
}
