// Package texture reads Source 2 compiled textures (.vtex_c): the header in
// the resource's DATA block, the mips stored after it, smallest first, and
// the top mip decoded to an image.
package texture

import (
	"encoding/binary"
	"math"

	"github.com/paralin/modlock/source2/resource"
	"github.com/pkg/errors"
)

// FlagCube marks a cube map, whose every mip holds six faces in turn.
const FlagCube = 0x10

// extraFill is the extra data entry of a texture padded to a power of two,
// which records the size of the image inside the padding.
const extraFill = 3

// Texture is a compiled texture's header and its stored mips.
type Texture struct {
	// AverageColor is the stored linear RGB reflectivity used as the shader's
	// color-correction pivot, without a gamma conversion.
	AverageColor [3]float32
	// Width is the top mip's width in texels.
	Width int
	// Height is the top mip's height in texels.
	Height int
	// Depth is the top mip's depth in slices, at least one.
	Depth int
	// Format is how the mips encode their texels.
	Format Format
	// Mips counts the stored mips, at least one.
	Mips int
	// Flags holds the header's flags, such as FlagCube.
	Flags int
	// VisibleWidth is the width of the image inside any padding.
	VisibleWidth int
	// VisibleHeight is the height of the image inside any padding.
	VisibleHeight int
	// Pixels holds every mip as stored, smallest first, and aliases the
	// resource Read was given.
	Pixels []byte
}

// Read reads a compiled texture's header and locates its mips.
func Read(compiled []byte) (*Texture, error) {
	// The fixed header opens the DATA block.
	blocks, err := resource.Read(compiled)
	if err != nil {
		return nil, err
	}
	d, ok := resource.Find(blocks, "DATA")
	if !ok || len(d) < 40 {
		return nil, errors.New("The resource has no texture header")
	}
	t := &Texture{
		Flags:  int(binary.LittleEndian.Uint16(d[2:])),
		Width:  int(binary.LittleEndian.Uint16(d[20:])),
		Height: int(binary.LittleEndian.Uint16(d[22:])),
		Depth:  max(1, int(binary.LittleEndian.Uint16(d[24:]))),
		Format: Format(d[26]),
		Mips:   max(1, int(d[27])),
	}
	for i := range t.AverageColor {
		t.AverageColor[i] = math.Float32frombits(binary.LittleEndian.Uint32(d[4+i*4:]))
	}
	t.VisibleWidth, t.VisibleHeight = t.Width, t.Height

	// The mips follow the DATA block. Its slice shares compiled's backing
	// array to the end, so the two capacities differ by the block's start;
	// lengths would not, since the array may run past compiled.
	t.Pixels = compiled[cap(compiled)-cap(d)+len(d):]

	// Extra data entries follow their table's offset field, and each counts
	// its data from its own offset field. A fill entry names the visible size.
	table := 32 + int(binary.LittleEndian.Uint32(d[32:]))
	count := int(binary.LittleEndian.Uint32(d[36:]))
	for i := range count {
		at := table + 12*i
		if at < 0 || at+12 > len(d) {
			break
		}
		start := at + 4 + int(binary.LittleEndian.Uint32(d[at+4:]))
		if binary.LittleEndian.Uint32(d[at:]) != extraFill || start < 0 || start+6 > len(d) {
			continue
		}
		w, h := int(binary.LittleEndian.Uint16(d[start+2:])), int(binary.LittleEndian.Uint16(d[start+4:]))
		if w > 0 && h > 0 && w <= t.Width && h <= t.Height {
			t.VisibleWidth, t.VisibleHeight = w, h
		}
	}
	return t, nil
}

// MipSize returns the bytes one mip takes, every face and slice included,
// and false for a format whose size is unknown.
func (t *Texture) MipSize(mip int) (int, bool) {
	// A volume halves its slices with each mip; a cube keeps six faces.
	w, h := max(1, t.Width>>mip), max(1, t.Height>>mip)
	layers := max(1, t.Depth>>mip)
	if t.Flags&FlagCube != 0 {
		layers *= 6
	}
	size, ok := t.Format.Size(w, h)
	return size * layers, ok
}

// Stored returns the stored bytes of every mip from the given one down to
// the smallest, smallest first as the file holds them: one range, read in
// place.
func (t *Texture) Stored(from int) ([]byte, error) {
	// Check the mip and its format.
	if from < 0 || from >= t.Mips {
		return nil, errors.Errorf("The texture has no mip %d", from)
	}
	if _, ok := t.MipSize(from); !ok {
		return nil, errors.Errorf("%s textures are not supported", t.Format)
	}

	// The smaller mips come first, so the range starts at the pixels.
	size := 0
	for mip := from; mip < t.Mips; mip++ {
		s, _ := t.MipSize(mip)
		size += s
	}
	if size > len(t.Pixels) {
		return nil, errors.New("The texture is shorter than its mips")
	}
	return t.Pixels[:size], nil
}
