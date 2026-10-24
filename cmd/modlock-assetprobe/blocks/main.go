// Command blocks prints each block of one resource in an archive with its
// size and first bytes.
package main

import (
	"encoding/hex"
	"fmt"
	"os"
	"path/filepath"

	"github.com/paralin/modlock/source2/resource"
	"github.com/paralin/modlock/source2/vpk"
)

func main() {
	archive, err := vpk.OpenArchive(os.Args[1])
	if err != nil {
		panic(err)
	}
	data, err := archive.ReadFile(os.Args[2])
	if err != nil {
		panic(err)
	}
	blocks, err := resource.Read(data)
	if err != nil {
		panic(err)
	}
	end := 0
	for i, b := range blocks {
		head := b.Data
		if len(head) > 48 {
			head = head[:48]
		}
		fmt.Printf("%2d %s %9d  %s\n", i, b.Tag, len(b.Data), hex.EncodeToString(head))
		_ = end
		if len(os.Args) > 3 {
			_ = os.WriteFile(filepath.Join(os.Args[3], fmt.Sprintf("%02d.%s", i, b.Tag)), b.Data, 0o644)
		}
	}
	fmt.Println("file", len(data))
}
