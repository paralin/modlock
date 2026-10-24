// Package kv3 reads binary KeyValues3 versions 4 and 5 and writes version 5,
// the format Source 2 compiled resources store their data in.
//
// Decode accepts untrusted input: malformed data returns an error and
// never panics, and a value budget bounds the work one buffer can demand.
package kv3

import (
	"encoding/binary"

	"github.com/klauspost/compress/zstd"
	"github.com/pierrec/lz4/v4"
	"github.com/pkg/errors"
)

// magic is the binary KeyValues3 version 5 header magic, "\x053VK" in file
// order.
const magic = 0x4b563305

// magic4 is the version 4 header magic, "\x043VK".
const magic4 = 0x4b563304

// header4Size is the length of the version 4 header.
const header4Size = 72

// headerSize is the length of the version 5 header that precedes the
// buffers.
const headerSize = 120

// end is the marker that follows the types of a KeyValues3 buffer.
const end = 0xffeedd00

// maxDepth bounds how deeply values may nest.
const maxDepth = 256

// maxValues bounds how many values one buffer may hold. A typed array of
// empty values costs no bytes, so its length alone cannot bound the work.
const maxValues = 1 << 22

// Compression methods named by the header.
const (
	// compressionNone stores both buffers as they are.
	compressionNone = 0
	// compressionLZ4 stores each buffer as one LZ4 block.
	compressionLZ4 = 1
	// compressionZstd stores each buffer as one Zstandard frame.
	compressionZstd = 2
)

// errTruncated reports a buffer whose values run past its lanes.
var errTruncated = errors.New("KeyValues3 data is truncated")

// Blob is a binary blob's size in bytes; the reader skips its payload.
type Blob int

// Decode reads binary KeyValues3 data. Objects decode as map[string]any and
// arrays as []any; the scalars as nil, bool, int64, uint64, float64, string
// and Blob. Flags are dropped.
func Decode(data []byte) (any, error) {
	// Check the format.
	if len(data) >= 4 && binary.LittleEndian.Uint32(data) == magic4 {
		return decode4(data)
	}
	if len(data) < headerSize {
		return nil, errTruncated
	}
	if binary.LittleEndian.Uint32(data) != magic {
		return nil, errors.New("Expected binary KeyValues3 version 4 or 5")
	}

	// Read the compression and the lane counts of both buffers.
	field := func(at int) int {
		return int(int32(binary.LittleEndian.Uint32(data[at:])))
	}
	compression := field(20)
	counts1 := [4]int{field(28), field(64), field(32), field(36)}
	typeCount := field(40)
	blobs := field(56)
	size1, packed1, size2, packed2 := field(72), field(76), field(80), field(84)
	counts2 := [4]int{field(88), field(92), field(96), field(100)}
	objectCount := field(108)

	// Unpack both buffers, which follow the header.
	if compression == compressionNone {
		packed1, packed2 = size1, size2
	}
	if packed1 < 0 || packed2 < 0 || headerSize+packed1+packed2 > len(data) {
		return nil, errTruncated
	}
	first, err := unpack(compression, data[headerSize:headerSize+packed1], size1)
	if err != nil {
		return nil, err
	}
	second, err := unpack(compression, data[headerSize+packed1:headerSize+packed1+packed2], size2)
	if err != nil {
		return nil, err
	}

	// The first buffer holds the auxiliary lanes, whose 1-byte lane starts
	// with the strings and whose 4-byte lane starts with their count.
	d := &decoder{budget: maxValues}
	auxiliary, _, ok := lanes(first, 0, counts1)
	if !ok {
		return nil, errTruncated
	}
	d.current = &auxiliary
	stringCount := int(int32(d.u32()))
	for len(d.strings) < stringCount {
		text, ok := d.cstring()
		if !ok {
			return nil, errTruncated
		}
		d.strings = append(d.strings, text)
	}

	// The second buffer starts with the object sizes and the main lanes.
	if objectCount < 0 || objectCount*4 > len(second) {
		return nil, errTruncated
	}
	d.objects = second[:objectCount*4]
	main, typesAt, ok := lanes(second, objectCount*4, counts2)
	if !ok {
		return nil, errTruncated
	}

	// The types and the blob sizes follow, then the end marker.
	endAt := typesAt + typeCount + 4*blobs
	if typeCount < 0 || blobs < 0 || endAt+4 > len(second) {
		return nil, errTruncated
	}
	if binary.LittleEndian.Uint32(second[endAt:]) != end {
		return nil, errors.New("KeyValues3 data has no end marker")
	}
	d.types = second[typesAt : typesAt+typeCount]
	d.blobs = second[typesAt+typeCount : endAt]

	// Read the root value from the main lanes.
	d.current, d.main, d.auxiliary = &main, &main, &auxiliary
	value := d.value(d.kind(), 0)
	if d.err != nil {
		return nil, d.err
	}
	return value, nil
}

// decode4 reads binary KeyValues3 version 4, which older resources in the
// game still use. Its one buffer holds the lanes, then the strings and the
// types, whose length together the header names, then the blob sizes and the
// end marker; an object's size is the next value of its 4-byte lane.
func decode4(data []byte) (any, error) {
	// Read the compression, the lane counts and the buffer's sizes.
	if len(data) < header4Size {
		return nil, errTruncated
	}
	field := func(at int) int {
		return int(int32(binary.LittleEndian.Uint32(data[at:])))
	}
	compression := field(20)
	counts := [4]int{field(28), field(64), field(32), field(36)}
	tail := field(40)
	size, packed := field(48), field(52)
	blobs := field(56)
	if compression == compressionNone {
		packed = size
	}
	if packed < 0 || header4Size+packed > len(data) {
		return nil, errTruncated
	}
	buffer, err := unpack(compression, data[header4Size:header4Size+packed], size)
	if err != nil {
		return nil, err
	}

	// The 4-byte lane starts with the string count, and the strings follow
	// the lanes at an 8-byte boundary, even when the 8-byte lane is empty.
	d := &decoder{budget: maxValues, inlineObjects: true}
	main, stringsAt, ok := lanes(buffer, 0, counts)
	stringsAt = (stringsAt + 7) / 8 * 8
	if !ok || stringsAt > len(buffer) {
		return nil, errTruncated
	}
	d.current = &main
	stringCount := int(int32(d.u32()))
	rest := [4][]byte{buffer[stringsAt:]}
	d.current = &rest
	for len(d.strings) < stringCount {
		text, ok := d.cstring()
		if !ok {
			return nil, errTruncated
		}
		d.strings = append(d.strings, text)
	}

	// The types fill the rest of the tail, then the blob sizes and the end
	// marker follow.
	typesAt, typesEnd := len(buffer)-len(rest[0]), stringsAt+tail
	endAt := typesEnd + 4*blobs
	if tail < 0 || blobs < 0 || typesEnd < typesAt || endAt+4 > len(buffer) {
		return nil, errTruncated
	}
	if binary.LittleEndian.Uint32(buffer[endAt:]) != end {
		return nil, errors.New("KeyValues3 data has no end marker")
	}
	d.types = buffer[typesAt:typesEnd]
	d.blobs = buffer[typesEnd:endAt]

	// Read the root value; version 4 has no auxiliary lanes.
	d.current, d.main, d.auxiliary = &main, &main, &main
	value := d.value(d.kind(), 0)
	if d.err != nil {
		return nil, d.err
	}
	return value, nil
}

// unpack decompresses one buffer into size bytes.
func unpack(compression int, packed []byte, size int) ([]byte, error) {
	// An uncompressed buffer is its own bytes.
	if size < 0 || size > 1<<30 {
		return nil, errTruncated
	}
	switch compression {
	case compressionNone:
		return packed, nil
	case compressionLZ4:
		out := make([]byte, size)
		n, err := lz4.UncompressBlock(packed, out)
		if err != nil {
			return nil, errors.Wrap(err, "Decompress KeyValues3 data")
		}
		if n != size {
			return nil, errTruncated
		}
		return out, nil
	case compressionZstd:
		decoder, err := zstd.NewReader(nil, zstd.WithDecoderMaxMemory(uint64(size)))
		if err != nil {
			return nil, err
		}
		defer decoder.Close()
		out, err := decoder.DecodeAll(packed, make([]byte, 0, size))
		if err != nil {
			return nil, errors.Wrap(err, "Decompress KeyValues3 data")
		}
		if len(out) != size {
			return nil, errTruncated
		}
		return out, nil
	default:
		return nil, errors.Errorf("Unknown KeyValues3 compression %d", compression)
	}
}

// lanes splits a buffer's 1-, 2-, 4- and 8-byte lanes from offset, each
// aligned to its width when it holds values, and returns them with their end.
func lanes(buffer []byte, offset int, counts [4]int) ([4][]byte, int, bool) {
	var out [4][]byte
	for index, count := range counts {
		width := 1 << index
		if count < 0 || count > len(buffer) {
			return out, 0, false
		}
		if count > 0 {
			offset = (offset + width - 1) / width * width
		}
		if offset+count*width > len(buffer) {
			return out, 0, false
		}
		out[index] = buffer[offset : offset+count*width]
		offset += count * width
	}
	return out, offset, true
}
