package session

// encodePlayerCommandRequest encodes a ControlRequest whose body is field 7,
// PlayerCommand, which a host built from this checkout delivers as if the
// player in slot sent line from their game. A generated ControlRequest schema
// that predates the field still forwards the bytes, and the host reads them
// from the request's unknown fields.
func encodePlayerCommandRequest(slot int32, line string) []byte {
	var inner []byte
	inner = appendVarintField(inner, 1, uint64(uint32(slot)))
	inner = appendBytesField(inner, 2, []byte(line))
	return appendBytesField(nil, 7, inner)
}

func appendVarintField(b []byte, field int, v uint64) []byte {
	b = appendVarint(b, uint64(field<<3))
	return appendVarint(b, v)
}

func appendBytesField(b []byte, field int, data []byte) []byte {
	b = appendVarint(b, uint64(field<<3|2))
	b = appendVarint(b, uint64(len(data)))
	return append(b, data...)
}

func appendVarint(b []byte, v uint64) []byte {
	for v >= 0x80 {
		b = append(b, byte(v)|0x80)
		v >>= 7
	}
	return append(b, byte(v))
}
