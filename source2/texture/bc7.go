package texture

// BC7 tables, from the BPTC format specification.

// bc7Partitions2 holds each two subset partition's subset bit per texel.
var bc7Partitions2 = [64]uint16{
	0xcccc, 0x8888, 0xeeee, 0xecc8, 0xc880, 0xfeec, 0xfec8, 0xec80,
	0xc800, 0xffec, 0xfe80, 0xe800, 0xffe8, 0xff00, 0xfff0, 0xf000,
	0xf710, 0x008e, 0x7100, 0x08ce, 0x008c, 0x7310, 0x3100, 0x8cce,
	0x088c, 0x3110, 0x6666, 0x366c, 0x17e8, 0x0ff0, 0x718e, 0x399c,
	0xaaaa, 0xf0f0, 0x5a5a, 0x33cc, 0x3c3c, 0x55aa, 0x9696, 0xa55a,
	0x73ce, 0x13c8, 0x324c, 0x3bdc, 0x6996, 0xc33c, 0x9966, 0x0660,
	0x0272, 0x04e4, 0x4e40, 0x2720, 0xc936, 0x936c, 0x39c6, 0x639c,
	0x9336, 0x9cc6, 0x817e, 0xe718, 0xccf0, 0x0fcc, 0x7744, 0xee22,
}

// bc7Anchors2 is each two subset partition's second subset anchor texel.
var bc7Anchors2 = [64]uint8{
	15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15,
	15, 2, 8, 2, 2, 8, 8, 15, 2, 8, 2, 2, 8, 8, 2, 2,
	15, 15, 6, 8, 2, 8, 15, 15, 2, 8, 2, 2, 2, 15, 15, 6,
	6, 2, 6, 8, 15, 15, 2, 2, 15, 15, 15, 15, 15, 2, 2, 15,
}

// bc7Partitions3 holds each three subset partition's two subset bits per
// texel.
var bc7Partitions3 = [64]uint32{
	0xaa685050, 0x6a5a5040, 0x5a5a4200, 0x5450a0a8, 0xa5a50000, 0xa0a05050, 0x5555a0a0, 0x5a5a5050,
	0xaa550000, 0xaa555500, 0xaaaa5500, 0x90909090, 0x94949494, 0xa4a4a4a4, 0xa9a59450, 0x2a0a4250,
	0xa5945040, 0x0a425054, 0xa5a5a500, 0x55a0a0a0, 0xa8a85454, 0x6a6a4040, 0xa4a45000, 0x1a1a0500,
	0x0050a4a4, 0xaaa59090, 0x14696914, 0x69691400, 0xa08585a0, 0xaa821414, 0x50a4a450, 0x6a5a0200,
	0xa9a58000, 0x5090a0a8, 0xa8a09050, 0x24242424, 0x00aa5500, 0x24924924, 0x24499224, 0x50a50a50,
	0x500aa550, 0xaaaa4444, 0x66660000, 0xa5a0a5a0, 0x50a050a0, 0x69286928, 0x44aaaa44, 0x66666600,
	0xaa444444, 0x54a854a8, 0x95809580, 0x96969600, 0xa85454a8, 0x80959580, 0xaa141414, 0x96960000,
	0xaaaa1414, 0xa05050a0, 0xa0a5a5a0, 0x96000000, 0x40804080, 0xa9a8a9a8, 0xaaaaaa44, 0x2a4a5254,
}

// bc7Anchors3 is each three subset partition's anchor texel for subsets one
// and two.
var bc7Anchors3 = [2][64]uint8{
	{
		3, 3, 15, 15, 8, 3, 15, 15, 8, 8, 6, 6, 6, 5, 3, 3,
		3, 3, 8, 15, 3, 3, 6, 10, 5, 8, 8, 6, 8, 5, 15, 15,
		8, 15, 3, 5, 6, 10, 8, 15, 15, 3, 15, 5, 15, 15, 15, 15,
		3, 15, 5, 5, 5, 8, 5, 10, 5, 10, 8, 13, 15, 12, 3, 3,
	},
	{
		15, 8, 8, 3, 15, 15, 3, 8, 15, 15, 15, 15, 15, 15, 15, 8,
		15, 8, 15, 3, 15, 8, 15, 8, 3, 15, 6, 10, 15, 15, 10, 8,
		15, 3, 15, 10, 10, 8, 9, 10, 6, 15, 8, 15, 3, 6, 6, 8,
		15, 3, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 3, 15, 15, 8,
	},
}

// bc7Weights are the interpolation weights out of 64 for 2, 3 and 4 bit
// indexes.
var bc7Weights = [3][]int{
	{0, 21, 43, 64},
	{0, 9, 18, 27, 37, 46, 55, 64},
	{0, 4, 9, 13, 17, 21, 26, 30, 34, 38, 43, 47, 51, 55, 60, 64},
}

// bc7Mode is one BC7 mode's layout, in the order its fields are stored.
type bc7Mode struct {
	// subsets counts the endpoint pairs, one to three.
	subsets int
	// partitionBits is the width of the partition number.
	partitionBits int
	// rotationBits is the width of the channel rotation.
	rotationBits int
	// selectionBits is the width of the index selection bit.
	selectionBits int
	// colorBits is the width of each color endpoint channel.
	colorBits int
	// alphaBits is the width of each alpha endpoint, or zero for opaque.
	alphaBits int
	// pBits is 1 when each endpoint gains a low p-bit.
	pBits int
	// shared reports one p-bit per subset rather than per endpoint.
	shared bool
	// indexBits are the widths of the primary and secondary indexes.
	indexBits [2]int
}

// bc7Modes are the eight BC7 modes.
var bc7Modes = [8]bc7Mode{
	{3, 4, 0, 0, 4, 0, 1, false, [2]int{3, 0}},
	{2, 6, 0, 0, 6, 0, 1, true, [2]int{3, 0}},
	{3, 6, 0, 0, 5, 0, 0, false, [2]int{2, 0}},
	{2, 6, 0, 0, 7, 0, 1, false, [2]int{2, 0}},
	{1, 0, 2, 1, 5, 6, 0, false, [2]int{2, 3}},
	{1, 0, 2, 0, 7, 8, 0, false, [2]int{2, 2}},
	{1, 0, 0, 0, 7, 7, 1, false, [2]int{4, 0}},
	{2, 6, 0, 0, 5, 5, 1, false, [2]int{2, 0}},
}

// bc7Bits reads count bits of a 16 byte block from bit offset at, low bits
// first.
func bc7Bits(block []byte, at, count int) int {
	value := 0
	for i := range count {
		bit := at + i
		value |= int(block[bit>>3]>>(bit&7)&1) << i
	}
	return value
}

// decodeBC7 decodes a BC7 block. A block with no mode decodes to
// transparent black, as the specification says.
func decodeBC7(block []byte, texels *[64]byte) {
	// The mode is the number of zero bits before the first one.
	mode := 0
	for mode < 8 && bc7Bits(block, mode, 1) == 0 {
		mode++
	}
	if mode == 8 {
		*texels = [64]byte{}
		return
	}
	m := &bc7Modes[mode]

	// Read the mode's partition, rotation and index selection in order.
	pos := mode + 1
	read := func(n int) int {
		v := bc7Bits(block, pos, n)
		pos += n
		return v
	}
	partition := read(m.partitionBits)
	rotation := read(m.rotationBits)
	selection := read(m.selectionBits)

	// Read the endpoints channel by channel, each subset's pair in turn.
	// Alpha stays opaque in a mode without it.
	endpoints := 2 * m.subsets
	var ep [4][6]int
	for c := range 3 {
		for e := range endpoints {
			ep[c][e] = read(m.colorBits)
		}
	}
	for e := range endpoints {
		ep[3][e] = 255
		if m.alphaBits != 0 {
			ep[3][e] = read(m.alphaBits)
		}
	}

	// Append the p-bits, one per endpoint or one per subset, then widen.
	var pbits [6]int
	if m.pBits != 0 {
		for s := range m.subsets {
			pbits[2*s] = read(1)
			pbits[2*s+1] = pbits[2*s]
			if !m.shared {
				pbits[2*s+1] = read(1)
			}
		}
	}
	for e := range endpoints {
		for c := range 3 {
			ep[c][e] = int(expand(ep[c][e]<<m.pBits|pbits[e], m.colorBits+m.pBits))
		}
		if m.alphaBits != 0 {
			ep[3][e] = int(expand(ep[3][e]<<m.pBits|pbits[e], m.alphaBits+m.pBits))
		}
	}

	// Each texel's index is one bit shorter at its subset's anchor. Modes 4
	// and 5 carry a second index set, which follows the first.
	bits0, bits1 := m.indexBits[0], m.indexBits[1]
	offset0, offset1 := pos, pos+16*bits0-1
	for t := range 16 {
		// Find the texel's subset and that subset's anchor.
		subset, anchor := 0, 0
		switch m.subsets {
		case 2:
			subset = int(bc7Partitions2[partition] >> t & 1)
			if subset != 0 {
				anchor = int(bc7Anchors2[partition])
			}
		case 3:
			subset = int(bc7Partitions3[partition] >> (2 * t) & 3)
			if subset != 0 {
				anchor = int(bc7Anchors3[subset-1][partition])
			}
		}

		// Read the texel's indexes.
		short := 0
		if t == anchor {
			short = 1
		}
		index0 := bc7Bits(block, offset0, bits0-short)
		offset0 += bits0 - short
		index1 := index0
		if bits1 != 0 {
			index1 = bc7Bits(block, offset1, bits1-short)
			offset1 += bits1 - short
		}

		// The selection bit swaps which index set drives color and alpha.
		w0 := bc7Weights[bits0-2]
		w1 := w0
		if bits1 != 0 {
			w1 = bc7Weights[bits1-2]
		}
		fc, fa := w0[index0], w1[index1]
		if selection != 0 {
			fc, fa = w1[index1], w0[index0]
		}

		// Interpolate between the subset's endpoints.
		texel := texels[t*4 : t*4+4]
		for c := range 4 {
			f := fc
			if c == 3 {
				f = fa
			}
			a, b := ep[c][2*subset], ep[c][2*subset+1]
			texel[c] = byte((a*(64-f) + b*f + 32) >> 6)
		}

		// Rotation swaps alpha with red, green or blue.
		if rotation != 0 {
			texel[3], texel[rotation-1] = texel[rotation-1], texel[3]
		}
	}
}
