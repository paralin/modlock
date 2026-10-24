// Command textures tallies the texture formats in an archive: format, mip
// count, depth, flags and whether the mips are LZ4 compressed, sampling
// every Nth texture.
package main

import (
	"encoding/binary"
	"fmt"
	"os"
	"slices"
	"strconv"
	"strings"

	"github.com/paralin/modlock/source2/resource"
	"github.com/paralin/modlock/source2/vpk"
)

var formats = strings.Fields("UNKNOWN DXT1 DXT5 I8 RGBA8888 R16 RG1616 RGBA16161616 R16F RG1616F RGBA16161616F R32F RG3232F RGB323232F RGBA32323232F JPEG_RGBA8888 PNG_RGBA8888 JPEG_DXT5 PNG_DXT5 BC6H BC7 ATI2N IA88 ETC2 ETC2_EAC R11_EAC RG11_EAC ATI1N BGRA8888 WEBP_RGBA8888 WEBP_DXT5 R32_UINT A8 R8_UNORM")

func main() {
	archive, err := vpk.OpenArchive(os.Args[1])
	if err != nil {
		panic(err)
	}
	every, _ := strconv.Atoi(os.Args[2])
	tally := map[string]int{}
	bytes := map[string]int64{}
	var largest []string
	n := 0
	for _, name := range archive.Names() {
		if !strings.HasSuffix(name, ".vtex_c") {
			continue
		}
		if n++; n%every != 0 {
			continue
		}
		data, err := archive.ReadFile(name)
		if err != nil {
			continue
		}
		blocks, err := resource.Read(data)
		if err != nil {
			tally["!read"]++
			continue
		}
		d, ok := resource.Find(blocks, "DATA")
		if !ok || len(d) < 40 {
			tally["!data"]++
			continue
		}
		flags := binary.LittleEndian.Uint16(d[2:])
		w, h, depth := binary.LittleEndian.Uint16(d[20:]), binary.LittleEndian.Uint16(d[22:]), binary.LittleEndian.Uint16(d[24:])
		format, mips := int(d[26]), d[27]
		extraOffset, extraCount := binary.LittleEndian.Uint32(d[32:]), binary.LittleEndian.Uint32(d[36:])
		compressed := false
		var extras []string
		for i := range int(extraCount) {
			at := 32 + int(extraOffset) + 12*i
			if at+12 > len(d) {
				break
			}
			kind := binary.LittleEndian.Uint32(d[at:])
			extras = append(extras, strconv.Itoa(int(kind)))
			if kind == 2 {
				compressed = true
			}
		}
		fname := "?" + strconv.Itoa(format)
		if format < len(formats) {
			fname = formats[format]
		}
		key := fmt.Sprintf("%-14s cube=%v depth>1=%v lz4mips=%v extras=%s", fname, flags&4 != 0, depth > 1, compressed, strings.Join(extras, "+"))
		tally[key]++
		size, _ := archive.Size(name)
		bytes[key] += size
		if size > 20<<20 {
			largest = append(largest, fmt.Sprintf("%6.1f MB %dx%d %s %s", float64(size)/1e6, w, h, fname, name))
		}
		_ = mips
	}
	keys := make([]string, 0, len(tally))
	for k := range tally {
		keys = append(keys, k)
	}
	slices.SortFunc(keys, func(a, b string) int { return tally[b] - tally[a] })
	fmt.Printf("sampled %d of %d textures\n", n/every, n)
	for _, k := range keys {
		fmt.Printf("%5d %9.1f MB  %s\n", tally[k], float64(bytes[k])/1e6, k)
	}
	for _, l := range largest {
		fmt.Println(l)
	}
}
