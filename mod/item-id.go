package mod

// ItemID returns the game's id for an item or ability name, such as
// "upgrade_magic_shield": MurmurHash2 of the name's bytes, seeded with its
// length, as the game hashes it.
func ItemID(name string) uint32 {
	// Seed the hash with the name's length.
	const m = 0x5bd1e995
	h := uint32(0x31415926) ^ uint32(len(name))
	i := 0

	// Mix in every whole four bytes.
	for ; len(name)-i >= 4; i += 4 {
		k := uint32(name[i]) | uint32(name[i+1])<<8 | uint32(name[i+2])<<16 | uint32(name[i+3])<<24
		k *= m
		k ^= k >> 24
		k *= m
		h = h*m ^ k
	}

	// Mix in the last one to three bytes, then finish.
	switch len(name) - i {
	case 3:
		h ^= uint32(name[i+2]) << 16
		fallthrough
	case 2:
		h ^= uint32(name[i+1]) << 8
		fallthrough
	case 1:
		h ^= uint32(name[i])
		h *= m
	}
	h ^= h >> 13
	h *= m
	return h ^ h>>15
}
