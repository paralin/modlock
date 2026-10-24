// Command modlock-assetprobe inventories the compiled resources in a game
// archive: how many of each type, their total size, and which blocks each
// type carries, so the asset viewer knows which formats it must decode.
package main

import (
	"fmt"
	"os"
	"path"
	"slices"
	"strings"

	"github.com/paralin/modlock/source2/resource"
	"github.com/paralin/modlock/source2/vpk"
)

func main() {
	archive, err := vpk.OpenArchive(os.Args[1])
	if err != nil {
		fmt.Fprintln(os.Stderr, err)
		os.Exit(1)
	}
	type tally struct {
		count  int
		size   int64
		blocks map[string]int
		sample string
	}
	byExt := map[string]*tally{}
	sampleEvery := 50
	for _, name := range archive.Names() {
		ext := path.Ext(name)
		t := byExt[ext]
		if t == nil {
			t = &tally{blocks: map[string]int{}, sample: name}
			byExt[ext] = t
		}
		t.count++
		size, _ := archive.Size(name)
		t.size += size
		if !strings.HasSuffix(ext, "_c") || t.count%sampleEvery != 1 {
			continue
		}
		data, err := archive.ReadFile(name)
		if err != nil {
			continue
		}
		blocks, err := resource.Read(data)
		if err != nil {
			t.blocks["!"+err.Error()]++
			continue
		}
		var tags []string
		for _, b := range blocks {
			tags = append(tags, b.Tag)
		}
		t.blocks[strings.Join(tags, ",")]++
	}
	exts := make([]string, 0, len(byExt))
	for ext := range byExt {
		exts = append(exts, ext)
	}
	slices.SortFunc(exts, func(a, b string) int { return byExt[b].count - byExt[a].count })
	for _, ext := range exts {
		t := byExt[ext]
		fmt.Printf("%-14s %7d files %9.1f MB  e.g. %s\n", ext, t.count, float64(t.size)/1e6, t.sample)
		for layout, n := range t.blocks {
			fmt.Printf("    %4d× %s\n", n, layout)
		}
	}
}
