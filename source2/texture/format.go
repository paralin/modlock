package texture

import "strconv"

// Format is how a texture encodes its texels, by the number its header
// stores.
type Format uint8

// The texture formats Deadlock ships.
const (
	// FormatDXT1 is BC1: 4x4 blocks of 8 bytes, two 565 colors.
	FormatDXT1 Format = 1
	// FormatDXT5 is BC3: a BC4 alpha block before a BC1 color block.
	FormatDXT5 Format = 2
	// FormatRGBA8888 stores four bytes per texel, red first.
	FormatRGBA8888 Format = 4
	// FormatBC6H is half-float color in 16 byte blocks.
	FormatBC6H Format = 19
	// FormatBC7 is 8-bit color and alpha in 16 byte blocks.
	FormatBC7 Format = 20
	// FormatATI2N is BC5: two BC4 blocks, red then green.
	FormatATI2N Format = 21
	// FormatATI1N is BC4: one channel in 8 byte blocks.
	FormatATI1N Format = 27
	// FormatBGRA8888 stores four bytes per texel, blue first.
	FormatBGRA8888 Format = 28
)

// formatNames names the formats Deadlock ships.
var formatNames = map[Format]string{
	FormatDXT1:     "DXT1",
	FormatDXT5:     "DXT5",
	FormatRGBA8888: "RGBA8888",
	FormatBC6H:     "BC6H",
	FormatBC7:      "BC7",
	FormatATI2N:    "BC5",
	FormatATI1N:    "BC4",
	FormatBGRA8888: "BGRA8888",
}

// String names the format, or gives its number.
func (f Format) String() string {
	if name, ok := formatNames[f]; ok {
		return name
	}
	return "format " + strconv.Itoa(int(f))
}

// BlockBytes returns the bytes in each 4x4 block of a block-compressed
// format, and zero for any other.
func (f Format) BlockBytes() int {
	switch f {
	case FormatDXT1, FormatATI1N:
		return 8
	case FormatDXT5, FormatBC6H, FormatBC7, FormatATI2N:
		return 16
	default:
		return 0
	}
}

// Size returns the bytes one w by h image takes in the format, and false
// for a format whose size is unknown.
func (f Format) Size(w, h int) (int, bool) {
	if block := f.BlockBytes(); block != 0 {
		return (w + 3) / 4 * ((h + 3) / 4) * block, true
	}
	if f == FormatRGBA8888 || f == FormatBGRA8888 {
		return w * h * 4, true
	}
	return 0, false
}
