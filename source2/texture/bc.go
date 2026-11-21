package texture

// decodeBC1 decodes a BC1 (DXT1) block, whose endpoint order selects the
// three color mode with transparent black.
func decodeBC1(block []byte, texels *[64]byte) {
	colorBlock(block, texels, true)
}

// decodeBC3 decodes a BC3 (DXT5) block: an alpha block, then a color block
// that is always four colors.
func decodeBC3(block []byte, texels *[64]byte) {
	colorBlock(block[8:], texels, false)
	channelBlock(block, texels, 3)
}

// decodeBC4 decodes a BC4 block to gray.
func decodeBC4(block []byte, texels *[64]byte) {
	channelBlock(block, texels, 0)
	for t := 0; t < 64; t += 4 {
		texels[t+1], texels[t+2], texels[t+3] = texels[t], texels[t], 255
	}
}

// decodeBC5 decodes a BC5 block to red and green.
func decodeBC5(block []byte, texels *[64]byte) {
	channelBlock(block, texels, 0)
	channelBlock(block[8:], texels, 1)
	for t := 0; t < 64; t += 4 {
		texels[t+2], texels[t+3] = 0, 255
	}
}

// colorBlock decodes the 8 byte color half of a BC1 or BC3 block.
func colorBlock(block []byte, texels *[64]byte, threeColor bool) {
	// Build the palette from the two 565 endpoints.
	c0 := int(block[0]) | int(block[1])<<8
	c1 := int(block[2]) | int(block[3])<<8
	rgb := func(c int) [3]int {
		return [3]int{int(expand(c>>11&31, 5)), int(expand(c>>5&63, 6)), int(expand(c&31, 5))}
	}
	a, b := rgb(c0), rgb(c1)
	var palette [4][4]byte
	for i := range 3 {
		palette[0][i], palette[1][i] = byte(a[i]), byte(b[i])
		if c0 > c1 || !threeColor {
			palette[2][i] = byte((2*a[i] + b[i]) / 3)
			palette[3][i] = byte((a[i] + 2*b[i]) / 3)
		} else {
			palette[2][i] = byte((a[i] + b[i]) / 2)
		}
	}
	palette[0][3], palette[1][3], palette[2][3] = 255, 255, 255
	if c0 > c1 || !threeColor {
		palette[3][3] = 255
	}

	// Sixteen 2 bit indexes follow, one byte per row, low bits first.
	for t := range 16 {
		index := block[4+t>>2] >> (t & 3 * 2) & 3
		copy(texels[t*4:t*4+4], palette[index][:])
	}
}

// channelBlock decodes an 8 byte BC4 block into one channel of texels.
func channelBlock(block []byte, texels *[64]byte, channel int) {
	// Build the eight value palette from the two endpoints.
	a, b := int(block[0]), int(block[1])
	values := [8]byte{byte(a), byte(b)}
	if a > b {
		for i := 1; i < 7; i++ {
			values[i+1] = byte(((7-i)*a + i*b) / 7)
		}
	} else {
		for i := 1; i < 5; i++ {
			values[i+1] = byte(((5-i)*a + i*b) / 5)
		}
		values[6], values[7] = 0, 255
	}

	// Sixteen 3 bit indexes follow in 48 bits, low bits first.
	var indexes uint64
	for i := range 6 {
		indexes |= uint64(block[2+i]) << (8 * i)
	}
	for t := range 16 {
		texels[t*4+channel] = values[indexes>>(3*t)&7]
	}
}

// expand widens a value of n bits to 8 by repeating its high bits.
func expand(v, n int) byte {
	v <<= 8 - n
	return byte(v | v>>n)
}
