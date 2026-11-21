package texture

import (
	"image"

	"github.com/pkg/errors"
)

// blockDecoder decodes one block of a block-compressed format into 16
// texels of 4 bytes each, row by row.
type blockDecoder func(block []byte, texels *[64]byte)

// blockDecoders maps each block-compressed format the package decodes to
// its decoder.
var blockDecoders = map[Format]blockDecoder{
	FormatDXT1:  decodeBC1,
	FormatDXT5:  decodeBC3,
	FormatBC7:   decodeBC7,
	FormatATI2N: decodeBC5,
	FormatATI1N: decodeBC4,
}

// Image decodes the top mip's first face or slice, cropped to the visible
// size. BC4 decodes to gray and BC5 to red and green.
func (t *Texture) Image() (*image.NRGBA, error) {
	// The top mip is the last one stored, and its first face leads it.
	stored, err := t.Stored(0)
	if err != nil {
		return nil, err
	}
	top, _ := t.MipSize(0)
	face, _ := t.Format.Size(t.Width, t.Height)
	src := stored[len(stored)-top:][:face]

	// Copy the texels of an uncompressed format, swizzling BGRA.
	img := image.NewNRGBA(image.Rect(0, 0, t.Width, t.Height))
	switch t.Format {
	case FormatRGBA8888:
		copy(img.Pix, src)
	case FormatBGRA8888:
		for i := 0; i < len(src); i += 4 {
			img.Pix[i], img.Pix[i+1], img.Pix[i+2], img.Pix[i+3] = src[i+2], src[i+1], src[i], src[i+3]
		}
	default:
		decode, ok := blockDecoders[t.Format]
		if !ok {
			return nil, errors.Errorf("%s textures cannot be decoded to an image", t.Format)
		}
		decodeBlocks(img, src, decode, t.Format.BlockBytes())
	}
	return img.SubImage(image.Rect(0, 0, t.VisibleWidth, t.VisibleHeight)).(*image.NRGBA), nil
}

// decodeBlocks decodes src, stored as rows of 4x4 blocks, into img. Edge
// blocks overhang an image whose size is not a multiple of four.
func decodeBlocks(img *image.NRGBA, src []byte, decode blockDecoder, size int) {
	w, h := img.Rect.Dx(), img.Rect.Dy()
	across := (w + 3) / 4
	var texels [64]byte
	for by := 0; by < (h+3)/4; by++ {
		for bx := range across {
			// Decode the block, then copy the rows and texels inside the image.
			at := (by*across + bx) * size
			decode(src[at:at+size], &texels)
			n := min(4, w-bx*4) * 4
			for y := 0; y < 4 && by*4+y < h; y++ {
				copy(img.Pix[(by*4+y)*img.Stride+bx*16:][:n], texels[y*16:])
			}
		}
	}
}
